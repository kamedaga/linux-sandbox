/* SPDX-License-Identifier: GPL-2.0-only */

#include "host.h"
#include "boot.h"
#include "time_port.h"
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
#include <asm/fpu/sched.h>
#include <asm/tlbflush.h>
#include "../mm/port.h"
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
static DEFINE_PER_CPU(unsigned long, gate_cpu_marker);
static DEFINE_PER_CPU(u64, port_switch_count);
static DEFINE_PER_CPU(u64, port_ipi_count);

#define KOBOX_TASK_GATE_TIMEOUT_NS (5ULL * NSEC_PER_SEC)

enum kobox_gate_phase {
	KOBOX_GATE_CREATED = 0,
	KOBOX_GATE_FIRST_SLEEP,
	KOBOX_GATE_LOCAL_SLEEP,
	KOBOX_GATE_PREEMPT_DISABLED,
	KOBOX_GATE_IRQ_DISABLED,
	KOBOX_GATE_SECOND_SLEEP,
	KOBOX_GATE_EXITING,
	KOBOX_GATE_MIGRATION_RUNNING,
	KOBOX_GATE_MIGRATED,
};

enum kobox_gate_command {
	KOBOX_GATE_HOLD = 0,
	KOBOX_GATE_RELEASE_PREEMPT,
	KOBOX_GATE_ENABLE_IRQ,
};

struct kobox_gate_worker {
	struct task_struct *task;
	struct task_struct *controller;
	unsigned int phase;
	unsigned int command;
	unsigned int first_cpu;
	unsigned int local_cpu;
	unsigned int remote_cpu;
	uint64_t preempt_switches;
	uint64_t irq_ipis;
	bool current_percpu_valid;
};

void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *task)
{
	/* Kernel tasks borrow an active mm; external process translations stay
	 * owned by the host address-space binding, not the native pthread's CR3.
	 */
	if (task->mm || !mm)
		BUG();
#ifdef KOBOX_BOOT_RUNTIME
	this_cpu_write(cpu_tlbstate_shared.is_lazy, true);
#endif
}

void kobox_provider_deactivate_mm(struct task_struct *task, struct mm_struct *mm)
{
	/* Clearing native FS/GS here would destroy the host pthread's TLS. */
	if (task != current || task->mm)
		BUG();
#ifndef KOBOX_BOOT_RUNTIME
	if (mm)
		BUG();
#endif
}

#ifndef KOBOX_BOOT_RUNTIME
void fpu_thread_struct_whitelist(unsigned long *offset, unsigned long *size)
{
	/* Hosted kernel tasks never save host FPU state into task_struct. */
	*offset = 0;
	*size = 0;
}
#endif

static struct kobox_task_port *task_port(const struct task_struct *task)
{
	return (struct kobox_task_port *)task->thread.sp;
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

	if (task_host->notifications_save(&mask))
		BUG();
	return mask;
}

void kobox_provider_preempt_restore(unsigned long flags)
{
	if (task_host->notifications_restore(flags))
		BUG();
}

unsigned long kobox_provider_irq_save_flags(void)
{
	unsigned long mask = kobox_provider_preempt_save();
	unsigned long flags = task_host->cpu_irq_disabled(port_cpu()) != 0;

	kobox_provider_preempt_restore(mask);
	return flags;
}

void kobox_provider_irq_disable(void)
{
	unsigned long mask = kobox_provider_preempt_save();

	if (!task_host->cpu_irq_disabled(port_cpu()) &&
	    task_host->cpu_irq_disable(port_cpu()))
		BUG();
	kobox_provider_preempt_restore(mask);
}

void kobox_provider_irq_enable(void)
{
	unsigned long mask = kobox_provider_preempt_save();

	if (task_host->cpu_irq_disabled(port_cpu()) &&
	    task_host->cpu_irq_enable(port_cpu()))
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
	if (!task_host || task_host->monotonic_ns(&now))
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

	if (task_host->realtime_ns(&wall_ns))
		BUG();
	*wall_time = ns_to_timespec64(wall_ns);
	*boot_offset = ns_to_timespec64(sched_clock());
}

static void send_reschedule(int cpu)
{
	if (cpu < 0 || cpu >= (int)KOBOX_LINUX_MEMORY_LOGICAL_CPUS ||
	    task_host->cpu_notify((uint32_t)cpu,
				  KOBOX_LINUX_TASK_RESCHEDULE))
		BUG();
}

static void send_call_function_single(int cpu)
{
	if (cpu < 0 || cpu >= (int)KOBOX_LINUX_MEMORY_LOGICAL_CPUS ||
	    task_host->cpu_notify((uint32_t)cpu,
				  KOBOX_LINUX_TASK_CALL_FUNCTION))
		BUG();
}

static void send_call_function_mask(const struct cpumask *mask)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		send_call_function_single((int)cpu);
}

#ifdef KOBOX_BOOT_RUNTIME
void arch_irq_work_raise(void)
{
	/* The same transport interrupt drains both upstream queues. Remote
	 * irq_work already uses Linux's call-single queue; local work does not.
	 */
	send_call_function_single(raw_smp_processor_id());
}
#endif

struct smp_ops smp_ops = {
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
	if (task_host->cpu_enter(port->resume_cpu, port->host_task))
		BUG();
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
				if (task_host->cpu_leave(port->resume_cpu))
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

	if (!task_host || !arguments->fn)
		return -EINVAL;
	/* user_mode_thread(kernel_init) has a kernel entry and no user mm. */
	if (task->mm)
		return -EOPNOTSUPP;
#ifdef KOBOX_BOOT_RUNTIME
	/* Upstream CPU initialization sized this storage. Kernel-start tasks,
	 * including PID 1, use Linux's minimal FP-state initialization.
	 */
	status = fpu_clone(task, arguments->flags, true, 0);
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
	flags = kobox_provider_preempt_save();
	status = task_host->task_create(
		&port->host_task, task_bootstrap, port);
	if (status) {
		kobox_provider_preempt_restore(flags);
		kfree(port);
		return -status;
	}
	task->thread.sp = (unsigned long)port;
	kobox_provider_preempt_restore(flags);
	return 0;
}

void exit_thread(struct task_struct *task)
{
	struct kobox_task_port *port = task_port(task);
	unsigned long mask;

#ifdef KOBOX_BOOT_RUNTIME
	fpu__drop(task);
#endif
	/* copy_process() can fail after copy_thread() created a parked pthread. */
	if (!port || READ_ONCE(task->__state) != TASK_NEW)
		return;
	mask = kobox_provider_preempt_save();
	WRITE_ONCE(port->aborted, true);
	if (task_host->task_wake(port->host_task) ||
	    task_host->task_join_destroy(port->host_task))
		BUG();
	task->thread.sp = 0;
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
		if (task_host->task_join_destroy(port->host_task))
			BUG();
		kobox_provider_preempt_restore(mask);
	}
	task->thread.sp = 0;
	kfree(port);
}

void flush_thread(void)
{
	BUG();
}

struct task_struct *__switch_to_asm(
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
	next_port->previous = previous;
	next_port->dead_previous = READ_ONCE(previous->__state) == TASK_DEAD ?
		previous_port : NULL;
	next_port->resume_cpu = cpu;
	raw_cpu_write(current_task, next);
	raw_cpu_inc(port_switch_count);
	__atomic_fetch_add(&task_report->context_switches, 1,
			   __ATOMIC_RELAXED);
	status = task_host->cpu_switch(
		cpu, previous_port->host_task, next_port->host_task,
		READ_ONCE(previous->__state) == TASK_DEAD);
	if (status)
		BUG();
	hosted_cpu = previous_port->resume_cpu;
	kobox_linux_memory_set_cpu(hosted_cpu);
	if (task_host->cpu_enter(previous_port->resume_cpu,
				 previous_port->host_task))
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
	sequence = task_host->cpu_notification_sequence(port_cpu());
	if (task_host->cpu_wait(port_cpu(), sequence, &sequence))
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
	sequence = task_host->cpu_notification_sequence(cpu);
	local_irq_enable();
	if (task_host->cpu_wait(cpu, sequence, &sequence))
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

static void gate_publish_phase(
	struct kobox_gate_worker *worker,
	enum kobox_gate_phase phase)
{
	smp_store_release(&worker->phase, (unsigned int)phase);
	wake_up_process(worker->controller);
}

static unsigned int gate_phase(const struct kobox_gate_worker *worker)
{
	return smp_load_acquire(&worker->phase);
}

static int gate_wait_phase(
	const struct kobox_gate_worker *worker,
	enum kobox_gate_phase phase)
{
	u64 deadline = sched_clock() + KOBOX_TASK_GATE_TIMEOUT_NS;

	for (;;) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (gate_phase(worker) >= (unsigned int)phase)
			break;
		if (sched_clock() >= deadline) {
			__set_current_state(TASK_RUNNING);
			return -ETIMEDOUT;
		}
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int gate_wait_counter(uint64_t *counter, uint64_t minimum)
{
	u64 deadline = sched_clock() + KOBOX_TASK_GATE_TIMEOUT_NS;

	while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < minimum) {
		cpu_relax();
		if (sched_clock() >= deadline)
			return -ETIMEDOUT;
	}
	return 0;
}

static bool gate_current_matches(void)
{
	return current == raw_cpu_read(current_task) &&
		task_cpu(current) == (int)raw_smp_processor_id() &&
		raw_cpu_read(gate_cpu_marker) == 0xabc000UL + port_cpu();
}

static void gate_sleep(
	struct kobox_gate_worker *worker,
	enum kobox_gate_phase phase)
{
	set_current_state(TASK_UNINTERRUPTIBLE);
	gate_publish_phase(worker, phase);
	schedule();
	__set_current_state(TASK_RUNNING);
}

static int primary_gate_worker(void *argument)
{
	struct kobox_gate_worker *worker = argument;
	u64 switches;
	u64 ipis;

	worker->task = current;
	worker->first_cpu = raw_smp_processor_id();
	worker->current_percpu_valid = gate_current_matches();
	smp_wmb();
	gate_sleep(worker, KOBOX_GATE_FIRST_SLEEP);

	worker->local_cpu = raw_smp_processor_id();
	worker->current_percpu_valid &= gate_current_matches();
	gate_sleep(worker, KOBOX_GATE_LOCAL_SLEEP);

	worker->remote_cpu = raw_smp_processor_id();
	worker->current_percpu_valid &= gate_current_matches();
	preempt_disable();
	switches = raw_cpu_read(port_switch_count);
	worker->preempt_switches = switches;
	gate_publish_phase(worker, KOBOX_GATE_PREEMPT_DISABLED);
	while (smp_load_acquire(&worker->command) <
	       KOBOX_GATE_RELEASE_PREEMPT)
		cpu_relax();
	if (gate_current_matches() && preempt_count() && need_resched() &&
	    raw_cpu_read(port_switch_count) == switches)
		task_report->preempt_disable_ready = 1;
	preempt_enable();
	schedule();

	local_irq_disable();
	ipis = raw_cpu_read(port_ipi_count);
	worker->irq_ipis = ipis;
	gate_publish_phase(worker, KOBOX_GATE_IRQ_DISABLED);
	while (smp_load_acquire(&worker->command) < KOBOX_GATE_ENABLE_IRQ)
		cpu_relax();
	if (raw_cpu_read(port_ipi_count) == ipis) {
		local_irq_enable();
		if (!gate_wait_counter(&per_cpu(port_ipi_count, 1), ipis + 1))
			task_report->irq_disable_ready = 1;
	} else {
		local_irq_enable();
	}
	schedule();
	worker->current_percpu_valid &= gate_current_matches();
	gate_publish_phase(worker, KOBOX_GATE_EXITING);
	return 0;
}

static int secondary_gate_worker(void *argument)
{
	struct kobox_gate_worker *worker = argument;

	worker->task = current;
	worker->first_cpu = raw_smp_processor_id();
	worker->current_percpu_valid = gate_current_matches();
	smp_wmb();
	gate_sleep(worker, KOBOX_GATE_FIRST_SLEEP);
	worker->remote_cpu = raw_smp_processor_id();
	worker->current_percpu_valid &= gate_current_matches();
	gate_sleep(worker, KOBOX_GATE_SECOND_SLEEP);
	worker->current_percpu_valid &= gate_current_matches();
	gate_publish_phase(worker, KOBOX_GATE_EXITING);
	return 0;
}

static int gate_join_task(struct task_struct *task)
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
	status = task_host->task_join_destroy(port->host_task);
	kobox_provider_preempt_restore(mask);
	if (status)
		goto out_put;
	task->thread.sp = 0;
	kfree(port);
out_put:
	put_task_struct(task);
	return status > 0 ? -status : status;
}

static int running_migration_worker(void *argument)
{
	struct kobox_gate_worker *worker = argument;
	u64 deadline;

	worker->task = current;
	worker->current_percpu_valid = gate_current_matches();
	gate_sleep(worker, KOBOX_GATE_FIRST_SLEEP);
	worker->remote_cpu = port_cpu();
	gate_publish_phase(worker, KOBOX_GATE_MIGRATION_RUNNING);
	deadline = sched_clock() + KOBOX_TASK_GATE_TIMEOUT_NS;
	while (port_cpu() == 1 && sched_clock() < deadline)
		cpu_relax();
	worker->local_cpu = port_cpu();
	worker->current_percpu_valid &= gate_current_matches();
	gate_sleep(worker, KOBOX_GATE_MIGRATED);
	return 0;
}

static int gate_running_migration(void)
{
	struct kobox_gate_worker worker = {.controller = current};
	int status;

	status = kernel_thread(running_migration_worker, &worker,
		"kobox-migrate", SIGCHLD | CLONE_FS | CLONE_FILES);
	if (status < 0)
		return status;
	status = gate_wait_phase(&worker, KOBOX_GATE_FIRST_SLEEP);
	if (!status)
		status = set_cpus_allowed_ptr(worker.task, cpumask_of(1));
	if (status)
		return status;
	wake_up_process(worker.task);
	status = gate_wait_phase(&worker, KOBOX_GATE_MIGRATION_RUNNING);
	if (!status)
		status = set_cpus_allowed_ptr(worker.task, cpumask_of(0));
	if (!status)
		status = gate_wait_phase(&worker, KOBOX_GATE_MIGRATED);
	if (status)
		return status;
	if (worker.remote_cpu != 1 || worker.local_cpu != 0 ||
	    !worker.current_percpu_valid ||
	    !cpumask_equal(worker.task->cpus_ptr, cpumask_of(0)))
		return -EINVAL;
	wake_up_process(worker.task);
	return gate_join_task(worker.task);
}

#ifndef KOBOX_BOOT_RUNTIME
static int init_scheduler_threads(void)
{
	pid_t pid;
	int status;

	pid = kernel_thread(kthreadd, NULL, "kthreadd", CLONE_FS | CLONE_FILES);
	if (pid < 0)
		return pid;
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();
	if (!kthreadd_task)
		return -ESRCH;
	/* Run this upstream early initcall once, after kthreadd is available. */
	status = cpu_stop_init();
	if (!status)
		stop_machine_unpark(1);
	return status;
}
#endif

static int user_mode_kernel_entry(void *argument)
{
	struct kobox_gate_worker *worker = argument;

	worker->task = current;
	worker->current_percpu_valid = gate_current_matches() &&
		!(current->flags & PF_KTHREAD) && !current->mm;
	gate_sleep(worker, KOBOX_GATE_FIRST_SLEEP);
	/* No user instruction context is claimed by this kernel-entry test. */
	do_exit(0);
}

static int test_user_mode_kernel_entry(void)
{
	struct kobox_gate_worker worker = {.controller = current};
	pid_t pid;
	int status;

	pid = user_mode_thread(user_mode_kernel_entry, &worker,
			       SIGCHLD | CLONE_FS | CLONE_FILES);
	if (pid < 0)
		return pid;
	status = gate_wait_phase(&worker, KOBOX_GATE_FIRST_SLEEP);
	if (status || !worker.current_percpu_valid)
		BUG();
	wake_up_process(worker.task);
	return gate_join_task(worker.task);
}

static int run_task_smp_gate(void)
{
	struct kobox_gate_worker primary = {.controller = current};
	struct kobox_gate_worker secondary = {.controller = current};
	struct cpumask mask;
	uint64_t ipis;
	pid_t pid;
	int status;

	cpumask_clear(&mask);
	cpumask_set_cpu(0, &mask);
	status = set_cpus_allowed_ptr(current, &mask);
#ifndef KOBOX_BOOT_RUNTIME
	if (!status)
		status = init_scheduler_threads();
#endif
	if (!status)
		status = test_user_mode_kernel_entry();
	if (status)
		return status;
	pid = kernel_thread(primary_gate_worker, &primary, "kobox-gate-a",
			    SIGCHLD | CLONE_FS | CLONE_FILES);
	if (pid < 0)
		return pid;
	status = gate_wait_phase(&primary, KOBOX_GATE_FIRST_SLEEP);
	if (status || !primary.task)
		return status ?: -EINVAL;
	status = set_cpus_allowed_ptr(primary.task, &mask);
	if (status)
		return status;
	if (!wake_up_process(primary.task)) {
		return -EINVAL;
	}
	status = gate_wait_phase(&primary, KOBOX_GATE_LOCAL_SLEEP);
	if (status)
		return status;
	task_report->local_switch_ready = primary.first_cpu == 0 &&
		primary.local_cpu == 0;

	cpumask_clear(&mask);
	cpumask_set_cpu(1, &mask);
	status = set_cpus_allowed_ptr(primary.task, &mask);
	if (status)
		return status;
	task_report->affinity_ready =
		cpumask_equal(primary.task->cpus_ptr, &mask);
	if (!wake_up_process(primary.task))
		return -EINVAL;
	status = gate_wait_phase(&primary, KOBOX_GATE_PREEMPT_DISABLED);
	if (status)
		return status;
	task_report->remote_switch_ready = primary.remote_cpu == 1;
	task_report->migration_ready = primary.remote_cpu == 1 &&
		task_cpu(primary.task) == 1;

	/* Affinity/priority changes can request reschedule before the wake call. */
	ipis = READ_ONCE(per_cpu(port_ipi_count, 1));
	pid = kernel_thread(secondary_gate_worker, &secondary, "kobox-gate-b",
			    SIGCHLD | CLONE_FS | CLONE_FILES);
	if (pid < 0)
		return pid;
	status = gate_wait_phase(&secondary, KOBOX_GATE_FIRST_SLEEP);
	if (status || !secondary.task)
		return status ?: -EINVAL;
	status = set_cpus_allowed_ptr(secondary.task, &mask);
	if (status)
		return status;
	sched_set_fifo(secondary.task);
	if (!wake_up_process(secondary.task))
		return -EINVAL;
	/* resched_curr() may coalesce the wake with an earlier tick request.
	 * Still exercise a delivered reschedule interrupt while preemption is
	 * disabled, without choosing or switching tasks in the machine port.
	 */
	smp_send_reschedule(1);
	status = gate_wait_counter(
		&per_cpu(port_ipi_count, 1), ipis + 1);
	if (status)
		return status;
	smp_store_release(&primary.command,
			  (unsigned int)KOBOX_GATE_RELEASE_PREEMPT);
	status = gate_wait_phase(&secondary, KOBOX_GATE_SECOND_SLEEP);
	if (status)
		return status;
	status = gate_wait_phase(&primary, KOBOX_GATE_IRQ_DISABLED);
	if (status)
		return status;
	if (!wake_up_process(secondary.task))
		return -EINVAL;
	/* With real tick/kworkers, NEED_RESCHED may already be set and Linux
	 * correctly coalesces the wake's IPI. Explicitly send one to test the
	 * masked-delivery contract, independently of scheduler coalescing.
	 */
	smp_send_reschedule(1);
	if (READ_ONCE(per_cpu(port_ipi_count, 1)) != primary.irq_ipis)
		return -EINVAL;
	smp_store_release(&primary.command,
			  (unsigned int)KOBOX_GATE_ENABLE_IRQ);
	status = gate_wait_phase(&secondary, KOBOX_GATE_EXITING);
	if (status)
		return status;
	status = gate_wait_phase(&primary, KOBOX_GATE_EXITING);
	if (status)
		return status;
	status = gate_join_task(secondary.task);
	if (!status)
		status = gate_join_task(primary.task);
	if (status)
		return status;
	status = gate_running_migration();
	if (status)
		return status;
	task_report->current_percpu_ready = primary.current_percpu_valid &&
		secondary.current_percpu_valid && gate_current_matches();
	task_report->exit_join_ready = 1;
	return 0;
}

static bool task_smp_report_ready(void)
{
	return task_report->upstream_schedule_ready &&
		task_report->upstream_try_to_wake_up_ready &&
		task_report->current_percpu_ready && task_report->local_switch_ready &&
		task_report->remote_switch_ready && task_report->migration_ready &&
		task_report->affinity_ready && task_report->remote_reschedule_ipis &&
		task_report->preempt_disable_ready && task_report->irq_disable_ready &&
		task_report->exit_join_ready;
}

#ifndef KOBOX_BOOT_RUNTIME
static int gate_init(void *argument)
{
	int status;

	(void)argument;
	status = run_task_smp_gate();
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

#ifdef KOBOX_BOOT_RUNTIME
int kobox_linux_task_verify_boot(void)
{
	int status;

	/* Verification must not initialize any scheduler or runtime service. */
	if (!boot_secondary_entry || system_state != SYSTEM_RUNNING ||
	    num_online_cpus() != KOBOX_LINUX_MEMORY_LOGICAL_CPUS ||
	    !kthreadd_task || !rcu_inkernel_boot_has_ended())
		return -EINVAL;
	per_cpu(gate_cpu_marker, 0) = 0xabc000UL;
	per_cpu(gate_cpu_marker, 1) = 0xabc001UL;
	task_report->logical_cpu_count = nr_cpu_ids;
	status = run_task_smp_gate();
	if (!status)
		status = kobox_task_time_gate(task_report);
	if (status)
		return status;
	task_report->upstream_schedule_ready = task_report->context_switches != 0;
	task_report->upstream_try_to_wake_up_ready =
		task_report->local_switch_ready && task_report->remote_switch_ready;
	return task_smp_report_ready() ? 0 : -EINVAL;
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
	    !operations->cpu_wait || !operations->cpu_notify ||
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
	return -task_host->task_wake(port->host_task);
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
	init_task.thread.sp = (unsigned long)&boot_task_port;
	if (task_host->monotonic_ns(&clock_origin))
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
	if (task_host->monotonic_ns(&clock_origin))
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
	init_task.thread.sp = (unsigned long)boot_port;
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
	per_cpu(gate_cpu_marker, 0) = 0xabc000UL;
	per_cpu(gate_cpu_marker, 1) = 0xabc001UL;
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
		status = task_host->task_wake(secondary_idle_port->host_task);
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
	status = task_host->cpu_notify(1, KOBOX_LINUX_TASK_RESCHEDULE);
	if (!status)
		status = task_host->task_join_destroy(
			secondary_idle_port->host_task);
	if (status)
		return status > 0 ? -status : status;
	secondary_idle_port->host_task = NULL;
	set_cpu_active(1, false);
	set_cpu_online(1, false);
	return task_smp_report_ready() ? 0 : -EINVAL;
}
#endif
