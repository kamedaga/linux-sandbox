/* SPDX-License-Identifier: GPL-2.0-only */

#include "host.h"
#include "../arch/x86_64/host_call.h"
#include "../arch/x86_64/task.h"
#include "boot.h"
#include "time_port.h"
#if !defined(KOBOX_BOOT_RUNTIME) || defined(KOBOX_RUNTIME_GATES)
#include "../tests/gates/task_port.h"
#endif
#include "../memory/port.h"

#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/clocksource.h>
#include <linux/completion.h>
#include <linux/context_tracking.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/irq_work.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/mm_types.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/idle.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/smpboot.h>
#include <linux/stop_machine.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include <trace/events/ipi.h>

#include <asm/smp.h>
#include <asm/irq_regs.h>
#include <asm/topology.h>
#ifdef KOBOX_BOOT_RUNTIME
#include "../mm/port.h"
#include "user.h"
#include "../boot/lifecycle.h"
#include "../boot/irq_host.h"
#endif

/* Hosted invariant failures must terminate even with CONFIG_BUG=n. */
#undef BUG
#define BUG() __builtin_trap()

extern void kobox_linux_memory_set_cpu(unsigned int cpu);
extern int kobox_linux_memory_early_boot(
	const struct kobox_linux_memory_layout *layout,
	struct kobox_linux_memory_report *report);
extern void sched_init(void);
extern void schedule_idle(void);
extern int sched_cpu_activate(unsigned int cpu);
extern int sched_cpu_starting(unsigned int cpu);
extern void cred_init(void);
extern void fork_init(void);
extern void pid_idr_init(void);
extern void proc_caches_init(void);
extern struct task_struct *idle_thread_get(unsigned int cpu);
extern void idle_threads_init(void);
extern int cpu_stop_init(void);
#ifdef CONFIG_ARCH_WANTS_DYNAMIC_TASK_STRUCT
extern int arch_task_struct_size;
#endif

struct kobox_task_port {
	struct task_struct *task;
	struct task_struct *previous;
	struct kobox_task_port *dead_previous;
	struct completion switched_out;
	void *host_task;
	int (*function)(void *);
	void *argument;
	unsigned int resume_cpu;
	bool idle;
	bool shutdown;
	bool aborted;
#ifdef KOBOX_BOOT_RUNTIME
	struct kobox_user_context *user;
#endif
};

static const struct kobox_linux_task_host_operations *task_host;

const struct kobox_linux_task_host_operations *kobox_task_host(void)
{
	return task_host;
}
static __thread struct task_struct *hosted_current;
static __thread unsigned int hosted_cpu;
static u64 clock_origin;
static struct kobox_linux_task_report *task_report;
#ifndef KOBOX_BOOT_RUNTIME
static struct kobox_task_port *secondary_idle_port;
static int gate_status;
static bool gate_done;
static bool secondary_cpu_ready;
#endif
static void (*boot_secondary_entry)(void);
static struct kobox_task_port boot_task_port;
static DEFINE_PER_CPU(u64, port_switch_count);
static DEFINE_PER_CPU(u64, port_ipi_count);

static struct kobox_task_port *task_port(const struct task_struct *task)
{
	return kobox_arch_task_binding(task);
}

static unsigned int port_cpu(void)
{
	unsigned int cpu = READ_ONCE(hosted_cpu);

	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		BUG();
	return cpu;
}

unsigned int kobox_provider_current_cpu_id(void)
{
	return port_cpu();
}

struct task_struct *kobox_provider_current_task(void)
{
	return hosted_current;
}

unsigned long kobox_provider_preempt_save(void)
{
	uint64_t mask;

	if (kobox_host_call(task_host->notifications_save(&mask)))
		BUG();
	return mask;
}

void kobox_provider_preempt_restore(unsigned long flags)
{
	if (kobox_host_call(task_host->notifications_restore(flags)))
		BUG();
}

unsigned long kobox_provider_irq_save_flags(void)
{
	unsigned long mask = kobox_provider_preempt_save();
	unsigned long flags = kobox_host_call(task_host->cpu_irq_disabled(port_cpu())) != 0;

	kobox_provider_preempt_restore(mask);
	return flags;
}

void kobox_provider_irq_disable(void)
{
	unsigned long mask = kobox_provider_preempt_save();

	if (!kobox_host_call(task_host->cpu_irq_disabled(port_cpu())) &&
	    kobox_host_call(task_host->cpu_irq_disable(port_cpu())))
		BUG();
	kobox_provider_preempt_restore(mask);
}

void kobox_provider_irq_enable(void)
{
	unsigned long mask = kobox_provider_preempt_save();

	if (kobox_host_call(task_host->cpu_irq_disabled(port_cpu())) &&
	    kobox_host_call(task_host->cpu_irq_enable(port_cpu())))
		BUG();
	kobox_provider_preempt_restore(mask);
}

unsigned long kobox_provider_irq_save(void)
{
	unsigned long flags = kobox_provider_irq_save_flags();

	kobox_provider_irq_disable();
	return flags;
}

void kobox_provider_irq_restore(unsigned long flags)
{
	if (flags)
		kobox_provider_irq_disable();
	else
		kobox_provider_irq_enable();
}

static u64 host_clock_read(struct clocksource *clock)
{
	uint64_t now;

	(void)clock;
	if (!task_host || kobox_host_call(task_host->monotonic_ns(&now)))
		BUG();
	return now;
}

u64 sched_clock(void)
{
	return host_clock_read(NULL) - clock_origin;
}

noinstr u64 sched_clock_noinstr(void)
{
	return host_clock_read(NULL) - clock_origin;
}

static struct clocksource host_clocksource = {
	.name = "kobox-monotonic",
	.rating = 400,
	.read = host_clock_read,
	.mask = CLOCKSOURCE_MASK(64),
	.mult = 1,
	.shift = 0,
	.max_cycles = S64_MAX,
	.max_raw_delta = S64_MAX,
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

#ifndef KOBOX_BOOT_RUNTIME
struct clocksource *clocksource_default_clock(void)
{
	return &host_clocksource;
}
#endif

int kobox_linux_task_register_clocksource(void)
{
	return clocksource_register_hz(&host_clocksource, NSEC_PER_SEC);
}

void read_persistent_wall_and_boot_offset(struct timespec64 *wall_time,
					struct timespec64 *boot_offset)
{
	uint64_t wall_ns;

	if (kobox_host_call(task_host->realtime_ns(&wall_ns)))
		BUG();
	*wall_time = ns_to_timespec64(wall_ns);
	*boot_offset = ns_to_timespec64(sched_clock());
}

static void send_reschedule(int cpu)
{
	if (cpu < 0 || cpu >= (int)KOBOX_LINUX_MEMORY_LOGICAL_CPUS ||
	    kobox_host_call(task_host->cpu_notify((uint32_t)cpu,
				  KOBOX_LINUX_TASK_RESCHEDULE)))
		BUG();
}

static void send_call_function_single(int cpu)
{
	if (cpu < 0 || cpu >= (int)KOBOX_LINUX_MEMORY_LOGICAL_CPUS ||
	    kobox_host_call(task_host->cpu_notify((uint32_t)cpu,
				  KOBOX_LINUX_TASK_CALL_FUNCTION)))
		BUG();
}

static void send_call_function_mask(const struct cpumask *mask)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		send_call_function_single((int)cpu);
}

#ifdef KOBOX_BOOT_RUNTIME
void kobox_linux_task_device_irq_raise(unsigned int cpu)
{
	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS ||
	    kobox_host_call(task_host->cpu_notify(cpu, KOBOX_LINUX_TASK_DEVICE_IRQ)))
		BUG();
}

void arch_irq_work_raise(void)
{
	/* The same transport interrupt drains both upstream queues. Remote
	 * irq_work already uses Linux's call-single queue; local work does not.
	 */
	send_call_function_single(raw_smp_processor_id());
}
#endif

static void stop_remote_cpus(int wait)
{
	unsigned int cpu, self = raw_smp_processor_id();

	/* Never resume Linux if a host CPU cannot be halted. In particular, a
	 * DMA invalidation panic must not release pins through a live CPU.
	 */
	for_each_online_cpu(cpu) {
		if (cpu == self)
			continue;
		if (kobox_host_call(task_host->cpu_stop(cpu)))
			BUG();
		set_cpu_online(cpu, false);
	}
}

struct smp_ops smp_ops = {
	.stop_other_cpus = stop_remote_cpus,
	.smp_send_reschedule = send_reschedule,
	.send_call_func_ipi = send_call_function_mask,
	.send_call_func_single_ipi = send_call_function_single,
};

void kobox_linux_task_dispatch(
	uint32_t cpu,
	enum kobox_linux_task_notification notification,
	uint64_t count)
{
	struct pt_regs regs = {.cs = __KERNEL_CS};
	struct pt_regs *previous_regs;
	unsigned int previous_count;
	bool watching;

	if (!task_host || cpu != port_cpu() || !count)
		BUG();
	local_irq_disable();
	previous_count = preempt_count();
	watching = rcu_is_watching_curr_cpu();
	previous_regs = set_irq_regs(&regs);
	irq_enter();
	if (!in_hardirq() || !rcu_is_watching_curr_cpu() ||
	    current != raw_cpu_read(current_task) || task_cpu(current) != cpu)
		BUG();
	task_report->hardirq_entries[cpu]++;
	if (!watching)
		task_report->idle_irq_entries[cpu]++;
	switch (notification) {
	case KOBOX_LINUX_TASK_RESCHEDULE:
		raw_cpu_add(port_ipi_count, count);
		__atomic_fetch_add(&task_report->remote_reschedule_ipis, count,
				   __ATOMIC_RELAXED);
		scheduler_ipi();
		break;
	case KOBOX_LINUX_TASK_CALL_FUNCTION:
		while (count--)
			generic_smp_call_function_single_interrupt();
#ifdef KOBOX_BOOT_RUNTIME
		irq_work_run();
#endif
		break;
	case KOBOX_LINUX_TASK_CLOCKEVENT:
		kobox_task_clock_interrupt();
		break;
#ifdef KOBOX_BOOT_RUNTIME
	case KOBOX_LINUX_TASK_DEVICE_IRQ:
		kobox_linux_irq_dispatch(cpu);
		break;
	case KOBOX_LINUX_TASK_CONTROL_EVENT:
		kobox_linux_lifecycle_interrupt();
		break;
	case KOBOX_LINUX_TASK_VM_EVENT:
		kobox_vm_interrupt();
		break;
#endif
	default:
		BUG();
	}
	irq_exit();
	set_irq_regs(previous_regs);
	if (preempt_count() != previous_count ||
	    rcu_is_watching_curr_cpu() != watching)
		BUG();
	/* The architecture interrupt-return boundary; Linux chooses the task. */
	if (!preempt_count() && need_resched())
		preempt_schedule_irq();
	/* The host restores the interrupted IRQ state after this return. */
	if (!irqs_disabled())
		BUG();
}

static void *task_bootstrap(void *argument)
{
	struct kobox_task_port *port = argument;

	if (READ_ONCE(port->aborted))
		return NULL;
	hosted_current = port->task;
	hosted_cpu = port->resume_cpu;
	kobox_linux_memory_set_cpu(port->resume_cpu);
	if (kobox_host_call(task_host->cpu_enter(port->resume_cpu, port->host_task)))
		BUG();
#ifdef KOBOX_BOOT_RUNTIME
	kobox_arch_task_stack_current(current);
#endif
	if (port->idle) {
		if (boot_secondary_entry) {
			boot_secondary_entry();
			BUG();
		}
#ifdef KOBOX_BOOT_RUNTIME
		BUG();
#else
		current->flags |= PF_IDLE;
		local_irq_disable();
		mmgrab(&init_mm);
		current->active_mm = &init_mm;
		rcutree_report_cpu_starting(port->resume_cpu);
		if (sched_cpu_starting(port->resume_cpu) ||
		    hrtimers_cpu_starting(port->resume_cpu) ||
		    rcutree_online_cpu(port->resume_cpu))
			BUG();
		kobox_task_clock_init();
		smp_store_release(&secondary_cpu_ready, true);
		local_irq_enable();
		for (;;) {
			if (READ_ONCE(port->shutdown)) {
				kobox_task_clock_stop();
				if (kobox_host_call(task_host->cpu_leave(port->resume_cpu)))
					BUG();
				return NULL;
			}
			if (need_resched()) {
				schedule_idle();
				continue;
			}
			local_irq_disable();
			default_idle_call();
			if (!rcu_is_watching_curr_cpu())
				BUG();
			task_report->idle_exits[port_cpu()]++;
		}
#endif
	}
	if (!port->previous)
		BUG();
	schedule_tail(port->previous);
#ifdef KOBOX_BOOT_RUNTIME
	if (port->user)
		kobox_user_enter(port->user);
#endif
	port->function(port->argument);
	/* PID 1 begins in kernel_init(), but a userspace return needs a port. */
	if (!(current->flags & PF_KTHREAD))
		BUG();
	do_exit(0);
}

int copy_thread(struct task_struct *task,
		const struct kernel_clone_args *arguments)
{
	struct kobox_task_port *port;
	unsigned long flags;
	int status;

	if (!task_host)
		return -EINVAL;
#ifndef KOBOX_BOOT_RUNTIME
	if (!arguments->fn)
		return -EINVAL;
#endif
	/* Linux copy_mm/copy_files/copy_creds have already installed the
	 * child's process state. A kernel-start trampoline works with a user
	 * mm too; it must not substitute init_mm or a shared kernel FD table.
	 * User-register return uses a native execution context; no process
	 * semantics or child selection are implemented by the machine port.
	 */
#ifdef KOBOX_BOOT_RUNTIME
	/* Upstream CPU initialization sized this storage. Kernel-start tasks,
	 * including PID 1, use Linux's minimal FP-state initialization.
	 */
	status = kobox_arch_task_fp_clone(task, arguments);
	if (status)
		return -EINVAL;
#endif
	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	port->task = task;
	init_completion(&port->switched_out);
	port->function = arguments->fn;
	port->argument = arguments->fn_arg;
	port->idle = arguments->idle;
	kobox_arch_task_frame_init(task, arguments);
#ifdef KOBOX_BOOT_RUNTIME
	if (!arguments->fn) {
		port->user = kobox_user_clone(task, arguments);
		if (IS_ERR(port->user)) {
			status = PTR_ERR(port->user);
			kfree(port);
			return status;
		}
	}
#endif
	flags = kobox_provider_preempt_save();
	status = kobox_host_call(task_host->task_create(
		&port->host_task, task_bootstrap, port));
	if (status) {
		kobox_provider_preempt_restore(flags);
#ifdef KOBOX_BOOT_RUNTIME
		kobox_user_release(port->user, task);
#endif
		kfree(port);
		return -status;
	}
	kobox_arch_task_bind(task, port);
	kobox_provider_preempt_restore(flags);
	return 0;
}

#ifdef KOBOX_BOOT_RUNTIME
int kobox_task_user_attach(struct kobox_user_context *context)
{
	struct kobox_task_port *port = task_port(current);

	if (!context || !port || port->task != current || port->user ||
	    current->flags & PF_KTHREAD || !current->mm)
		return -EINVAL;
	port->user = context;
	return 0;
}
#endif

void exit_thread(struct task_struct *task)
{
	struct kobox_task_port *port = task_port(task);
	unsigned long mask;

#ifdef KOBOX_BOOT_RUNTIME
	kobox_arch_task_fp_drop(task);
	if (port && port->task == task && port->user) {
		kobox_user_release(port->user, task);
		port->user = NULL;
	}
#endif
	/* copy_process() can fail after copy_thread() created a parked pthread. */
	if (!port || READ_ONCE(task->__state) != TASK_NEW)
		return;
	mask = kobox_provider_preempt_save();
	WRITE_ONCE(port->aborted, true);
	if (kobox_host_call(task_host->task_wake(port->host_task)) ||
	    kobox_host_call(task_host->task_join_destroy(port->host_task)))
		BUG();
	kobox_arch_task_bind(task, NULL);
	kobox_provider_preempt_restore(mask);
	kfree(port);
}

void arch_release_task_struct(struct task_struct *task)
{
	struct kobox_task_port *port = task_port(task);
	unsigned long mask;

	/* A failed fork before copy_thread() still contains its parent's sp. */
	if (!port || port->task != task)
		return;
	if (port->host_task) {
		if (READ_ONCE(task->__state) != TASK_DEAD ||
		    smp_load_acquire(&task->on_cpu))
			BUG();
		/* Final Linux release: the native thread no longer needs a CPU. */
		mask = kobox_provider_preempt_save();
		if (kobox_host_call(task_host->task_join_destroy(port->host_task)))
			BUG();
		kobox_provider_preempt_restore(mask);
	}
	kobox_arch_task_bind(task, NULL);
	kfree(port);
}

struct task_struct *kobox_task_switch(
	struct task_struct *previous,
	struct task_struct *next)
{
	struct kobox_task_port *previous_port = task_port(previous);
	struct kobox_task_port *next_port = task_port(next);
	unsigned int cpu = port_cpu();
	int status;

	if (current != previous || !irqs_disabled() ||
	    preempt_count() != 2 * PREEMPT_DISABLE_OFFSET ||
	    !previous_port || !next_port || !previous_port->host_task ||
	    !next_port->host_task)
		BUG();
#ifdef KOBOX_BOOT_RUNTIME
	/* Each OS thread has a different physical FP bank, including when it
	 * resumes on the same logical CPU. Retain upstream save/lazy-load.
	 */
	kobox_arch_task_fp_switch(previous, cpu);
#endif
	next_port->previous = previous;
	next_port->dead_previous = READ_ONCE(previous->__state) == TASK_DEAD ?
		previous_port : NULL;
	next_port->resume_cpu = cpu;
	raw_cpu_write(current_task, next);
#ifdef KOBOX_BOOT_RUNTIME
	kobox_arch_task_stack_current(next);
#endif
	raw_cpu_inc(port_switch_count);
	__atomic_fetch_add(&task_report->context_switches, 1,
			   __ATOMIC_RELAXED);
	status = kobox_host_call(task_host->cpu_switch(
		cpu, previous_port->host_task, next_port->host_task,
		READ_ONCE(previous->__state) == TASK_DEAD));
	if (status)
		BUG();
	hosted_cpu = previous_port->resume_cpu;
	kobox_linux_memory_set_cpu(hosted_cpu);
	if (kobox_host_call(task_host->cpu_enter(previous_port->resume_cpu,
				 previous_port->host_task)))
		BUG();
	return previous_port->previous;
}

void kobox_provider_finish_switch(void)
{
	struct kobox_task_port *port = task_port(current);
	struct kobox_task_port *dead = port->dead_previous;

	port->dead_previous = NULL;
	/* rq is unlocked and Linux has cleared the old task's on_cpu flag. */
	if (dead)
		complete(&dead->switched_out);
}

void kobox_provider_cpu_idle(void)
{
	struct kobox_task_port *port = task_port(current);
	uint64_t sequence;

	if (!port)
		BUG();
	sequence = kobox_host_call(task_host->cpu_notification_sequence(port_cpu()));
	if (kobox_host_call(task_host->cpu_wait(port_cpu(), sequence, &sequence)))
		BUG();
}

void arch_cpu_idle(void)
{
	unsigned int cpu = port_cpu();
	uint64_t sequence;

	/* default_idle_call() owns the upstream RCU idle transitions. */
	if (!is_idle_task(current) || !irqs_disabled() ||
	    rcu_is_watching_curr_cpu())
		BUG();
	task_report->idle_entries[cpu]++;
	/* Observe before enabling: an IRQ delivered by enable must skip wait. */
	sequence = kobox_host_call(task_host->cpu_notification_sequence(cpu));
	local_irq_enable();
	if (kobox_host_call(task_host->cpu_wait(cpu, sequence, &sequence)))
		BUG();
	local_irq_disable();
	if (rcu_is_watching_curr_cpu())
		BUG();
}

void kobox_linux_task_idle_exit(void)
{
	if (!rcu_is_watching_curr_cpu())
		BUG();
	task_report->idle_exits[port_cpu()]++;
}

#if !defined(KOBOX_BOOT_RUNTIME) || defined(KOBOX_RUNTIME_GATES)
int kobox_task_gate_join(struct task_struct *task)
{
	struct kobox_task_port *port;
	unsigned long mask;
	pid_t pid;
	int exit_status = 0;
	int status;

	if (!task)
		return -EINVAL;
	port = task_port(task);
	if (!port || !port->host_task)
		return -EINVAL;
	pid = task_pid_nr(task);
	get_task_struct(task);
	status = kernel_wait(pid, &exit_status);
	if (status != pid || exit_status) {
		put_task_struct(task);
		return -EINVAL;
	}
	wait_for_completion(&port->switched_out);
	if (READ_ONCE(task->__state) != TASK_DEAD ||
	    smp_load_acquire(&task->on_cpu))
		BUG();
	mask = kobox_provider_preempt_save();
	status = kobox_host_call(task_host->task_join_destroy(port->host_task));
	kobox_provider_preempt_restore(mask);
	if (status)
		goto out_put;
	kobox_arch_task_bind(task, NULL);
	kfree(port);
out_put:
	put_task_struct(task);
	return status > 0 ? -status : status;
}


u64 *kobox_task_gate_switches(unsigned int cpu)
{
	return &per_cpu(port_switch_count, cpu);
}

u64 *kobox_task_gate_ipis(unsigned int cpu)
{
	return &per_cpu(port_ipi_count, cpu);
}
#endif

#ifndef KOBOX_BOOT_RUNTIME
static int gate_init(void *argument)
{
	int status;

	(void)argument;
	status = kobox_task_smp_gate(task_report);
	if (!status)
		status = kobox_task_time_gate(task_report);
	WRITE_ONCE(gate_status, status);
	/* PID 1, like the idle tasks, lives until the sandbox process exits. */
	set_current_state(TASK_UNINTERRUPTIBLE);
	smp_store_release(&gate_done, true);
	schedule();
	BUG();
}
#endif

#if defined(KOBOX_BOOT_RUNTIME) && defined(KOBOX_RUNTIME_GATES)
int kobox_linux_task_verify_boot(void)
{
	int status;

	/* Verification must not initialize any scheduler or runtime service. */
	if (!boot_secondary_entry || system_state != SYSTEM_RUNNING ||
	    num_online_cpus() != KOBOX_LINUX_MEMORY_LOGICAL_CPUS ||
	    !kthreadd_task || !rcu_inkernel_boot_has_ended())
		return -EINVAL;
	task_report->logical_cpu_count = nr_cpu_ids;
	status = kobox_task_smp_gate(task_report);
	if (!status)
		status = kobox_task_time_gate(task_report);
	if (status)
		return status;
	task_report->upstream_schedule_ready = task_report->context_switches != 0;
	task_report->upstream_try_to_wake_up_ready =
		task_report->local_switch_ready && task_report->remote_switch_ready;
	return kobox_task_smp_report_ready(task_report) ? 0 : -EINVAL;
}
#endif

static int validate_layout(const struct kobox_linux_task_layout *layout)
{
	const struct kobox_linux_task_host_operations *operations;

	if (!layout || layout->size != sizeof(*layout) ||
	    layout->identity != KOBOX_LINUX_TASK_HOST_IDENTITY ||
	    !layout->boot_task || !layout->operations)
		return -EINVAL;
	operations = layout->operations;
	if (operations->size != sizeof(*operations) ||
	    operations->identity != KOBOX_LINUX_TASK_HOST_IDENTITY ||
	    !operations->task_create || !operations->task_wake ||
	    !operations->task_park || !operations->task_join_destroy ||
	    !operations->task_exit || !operations->cpu_enter ||
	    !operations->cpu_leave || !operations->cpu_switch ||
	    !operations->cpu_wait || !operations->cpu_stop || !operations->cpu_notify ||
	    !operations->cpu_irq_disable || !operations->cpu_irq_enable ||
	    !operations->cpu_irq_disabled ||
	    !operations->notifications_save ||
	    !operations->notifications_restore ||
	    !operations->cpu_notification_sequence ||
	    !operations->monotonic_ns || !operations->realtime_ns ||
	    !operations->clockevent_arm || !operations->clockevent_cancel ||
	    !operations->clockevent_stop)
		return -EINVAL;
	return 0;
}

static int init_hosted_cpu_topology(void)
{
	unsigned int cpu;

	/* Logical CPUs are independent cores, not host hardware SMT siblings. */
	for_each_possible_cpu(cpu) {
		if (!zalloc_cpumask_var(&per_cpu(cpu_sibling_map, cpu), GFP_KERNEL) ||
		    !zalloc_cpumask_var(&per_cpu(cpu_core_map, cpu), GFP_KERNEL) ||
		    !zalloc_cpumask_var(&per_cpu(cpu_die_map, cpu), GFP_KERNEL) ||
		    !zalloc_cpumask_var(&per_cpu(cpu_llc_shared_map, cpu), GFP_KERNEL) ||
		    !zalloc_cpumask_var(&per_cpu(cpu_l2c_shared_map, cpu), GFP_KERNEL))
			return -ENOMEM;
		cpumask_set_cpu(cpu, topology_sibling_cpumask(cpu));
		cpumask_copy(topology_core_cpumask(cpu), cpu_possible_mask);
		cpumask_copy(topology_die_cpumask(cpu), cpu_possible_mask);
		cpumask_set_cpu(cpu, cpu_llc_shared_mask(cpu));
		cpumask_set_cpu(cpu, cpu_l2c_shared_mask(cpu));
		cpumask_set_cpu(cpu, &__cpu_primary_thread_mask);
		cpu_data(cpu) = boot_cpu_data;
		cpu_data(cpu).cpu_index = cpu;
		cpu_data(cpu).topo.core_id = cpu;
		cpu_data(cpu).topo.logical_core_id = cpu;
		cpu_data(cpu).topo.llc_id = cpu;
		cpu_data(cpu).topo.l2c_id = cpu;
	}
	return 0;
}

int kobox_linux_task_prepare_cpus(void)
{
	return init_hosted_cpu_topology();
}

int kobox_linux_task_kick_cpu(unsigned int cpu, struct task_struct *idle)
{
	struct kobox_task_port *port;

	if (!boot_secondary_entry || !cpu || cpu >= nr_cpu_ids || !idle)
		return -EINVAL;
	port = task_port(idle);
	if (!port || !port->idle || !port->host_task)
		return -EINVAL;
	port->resume_cpu = cpu;
	per_cpu(current_task, cpu) = idle;
	/* CPUHP owns the online transition and callback execution. */
	return -kobox_host_call(task_host->task_wake(port->host_task));
}

int kobox_linux_task_install_secondary_entry(void (*entry)(void))
{
	if (!task_host || !entry || boot_secondary_entry ||
	    num_online_cpus() != 1 || raw_smp_processor_id())
		return -EINVAL;
	boot_secondary_entry = entry;
	return 0;
}

int kobox_linux_task_bind_boot(const struct kobox_linux_task_layout *layout,
			       struct kobox_linux_task_report *report)
{
	int status;

	if (task_host || !report ||
	    report->size != sizeof(*report) ||
	    report->identity != KOBOX_LINUX_TASK_HOST_IDENTITY)
		return -EINVAL;
	status = validate_layout(layout);
	if (status)
		return status;
	task_host = layout->operations;
	task_report = report;
	hosted_current = &init_task;
	hosted_cpu = 0;
	boot_task_port.task = &init_task;
	boot_task_port.host_task = layout->boot_task;
	boot_task_port.idle = true;
	kobox_arch_task_bind(&init_task, &boot_task_port);
	if (kobox_host_call(task_host->monotonic_ns(&clock_origin)))
		return -EIO;
	return kobox_linux_memory_bind(&layout->memory);
}

#ifndef KOBOX_BOOT_RUNTIME
__attribute__((visibility("default")))
int kobox_linux_task_smp_boot(
	const struct kobox_linux_task_layout *layout,
	struct kobox_linux_task_report *report)
{
	struct kobox_linux_memory_report memory_report = {
		.size = sizeof(memory_report),
		.identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
	};
	struct kobox_task_port *boot_port;
	struct task_struct *secondary_idle;
	int status;

	if (!report || report->size != sizeof(*report) ||
	    report->identity != KOBOX_LINUX_TASK_HOST_IDENTITY)
		return -EINVAL;
	status = validate_layout(layout);
	if (status)
		return status;
	task_host = layout->operations;
	hosted_current = &init_task;
	if (kobox_host_call(task_host->monotonic_ns(&clock_origin)))
		return -EIO;
	task_report = report;
	jump_label_init();
	status = kobox_linux_memory_early_boot(&layout->memory, &memory_report);
	if (status)
		return status;
	boot_port = kzalloc(sizeof(*boot_port), GFP_KERNEL);
	if (!boot_port)
		return -ENOMEM;
	boot_port->task = &init_task;
	boot_port->host_task = layout->boot_task;
	boot_port->resume_cpu = 0;
	kobox_arch_task_bind(&init_task, boot_port);
	status = init_hosted_cpu_topology();
	if (status)
		return status;
	sched_init();
	init_task.flags |= PF_IDLE;
	workqueue_init_early();
	rcu_init();
	tick_init();
	timers_init();
	hrtimers_init();
	softirq_init();
	status = clocksource_register_hz(&host_clocksource, NSEC_PER_SEC);
	if (status)
		return status;
	timekeeping_init();
	sched_clock_init();
	local_irq_disable();
	call_function_init();
	pid_idr_init();
	cred_init();
#ifdef CONFIG_ARCH_WANTS_DYNAMIC_TASK_STRUCT
	arch_task_struct_size = sizeof(struct task_struct);
#endif
	fork_init();
	proc_caches_init();
	rcu_scheduler_starting();
	idle_threads_init();
	secondary_idle = idle_thread_get(1);
	if (IS_ERR(secondary_idle))
		return PTR_ERR(secondary_idle);
	secondary_idle_port = task_port(secondary_idle);
	if (!secondary_idle_port || !secondary_idle_port->host_task)
		return -EINVAL;
	secondary_idle_port->resume_cpu = 1;
	per_cpu(current_task, 1) = secondary_idle;
	status = rcutree_prepare_cpu(1);
	if (!status)
		status = smpcfd_prepare_cpu(1);
	if (!status)
		status = timers_prepare_cpu(1);
	if (!status)
		status = hrtimers_prepare_cpu(1);
	if (status)
		return status;
	set_cpu_online(1, true);
	status = sched_cpu_activate(0);
	if (!status)
		status = sched_cpu_activate(1);
	/* CPU activation runs with IRQs enabled; CPU-starting/device setup do not. */
	local_irq_disable();
	if (!status)
		status = sched_cpu_starting(0);
	if (!status)
		kobox_task_clock_init();
	if (!status)
		status = kobox_host_call(task_host->task_wake(secondary_idle_port->host_task));
	if (status)
		return status > 0 ? -status : status;
	while (!smp_load_acquire(&secondary_cpu_ready))
		cpu_relax();
	local_irq_enable();
	report->logical_cpu_count = nr_cpu_ids;
	status = kernel_thread(gate_init, NULL, "kobox-gate-init",
			       CLONE_FS | CLONE_FILES);
	if (status < 0)
		return status;
	while (!smp_load_acquire(&gate_done)) {
		if (need_resched())
			schedule_idle();
		else if (!smp_load_acquire(&gate_done)) {
			local_irq_disable();
			default_idle_call();
			if (!rcu_is_watching_curr_cpu())
				BUG();
			task_report->idle_exits[0]++;
		}
	}
	status = READ_ONCE(gate_status);
	if (status)
		return status;
	report->upstream_schedule_ready = report->context_switches != 0;
	report->upstream_try_to_wake_up_ready =
		report->local_switch_ready && report->remote_switch_ready;
	kobox_task_clock_stop();
	WRITE_ONCE(secondary_idle_port->shutdown, true);
	status = kobox_host_call(task_host->cpu_notify(1, KOBOX_LINUX_TASK_RESCHEDULE));
	if (!status)
		status = kobox_host_call(task_host->task_join_destroy(
			secondary_idle_port->host_task));
	if (status)
		return status > 0 ? -status : status;
	secondary_idle_port->host_task = NULL;
	set_cpu_active(1, false);
	set_cpu_online(1, false);
	return kobox_task_smp_report_ready(task_report) ? 0 : -EINVAL;
}
#endif
