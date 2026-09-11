// SPDX-License-Identifier: GPL-2.0-only
#include "task_port.h"

#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/smp.h>
#include <linux/stop_machine.h>
#include <trace/events/ipi.h>

#undef BUG
#define BUG() __builtin_trap()

extern int cpu_stop_init(void);
static struct kobox_linux_task_report *task_report;
static DEFINE_PER_CPU(unsigned long, gate_cpu_marker);

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
		raw_cpu_read(gate_cpu_marker) == 0xabc000UL + raw_smp_processor_id();
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
	switches = READ_ONCE(*kobox_task_gate_switches(raw_smp_processor_id()));
	worker->preempt_switches = switches;
	gate_publish_phase(worker, KOBOX_GATE_PREEMPT_DISABLED);
	while (smp_load_acquire(&worker->command) <
	       KOBOX_GATE_RELEASE_PREEMPT)
		cpu_relax();
	if (gate_current_matches() && preempt_count() && need_resched() &&
	    READ_ONCE(*kobox_task_gate_switches(raw_smp_processor_id())) == switches)
		task_report->preempt_disable_ready = 1;
	preempt_enable();
	schedule();

	local_irq_disable();
	ipis = READ_ONCE(*kobox_task_gate_ipis(raw_smp_processor_id()));
	worker->irq_ipis = ipis;
	gate_publish_phase(worker, KOBOX_GATE_IRQ_DISABLED);
	while (smp_load_acquire(&worker->command) < KOBOX_GATE_ENABLE_IRQ)
		cpu_relax();
	if (READ_ONCE(*kobox_task_gate_ipis(raw_smp_processor_id())) == ipis) {
		local_irq_enable();
		if (!gate_wait_counter(kobox_task_gate_ipis(1), ipis + 1))
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

static int running_migration_worker(void *argument)
{
	struct kobox_gate_worker *worker = argument;
	u64 deadline;

	worker->task = current;
	worker->current_percpu_valid = gate_current_matches();
	gate_sleep(worker, KOBOX_GATE_FIRST_SLEEP);
	worker->remote_cpu = raw_smp_processor_id();
	gate_publish_phase(worker, KOBOX_GATE_MIGRATION_RUNNING);
	deadline = sched_clock() + KOBOX_TASK_GATE_TIMEOUT_NS;
	while (raw_smp_processor_id() == 1 && sched_clock() < deadline)
		cpu_relax();
	worker->local_cpu = raw_smp_processor_id();
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
	return kobox_task_gate_join(worker.task);
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
	return kobox_task_gate_join(worker.task);
}

int kobox_task_smp_gate(struct kobox_linux_task_report *report)
{
	struct kobox_gate_worker primary = {.controller = current};
	struct kobox_gate_worker secondary = {.controller = current};
	struct cpumask mask;
	uint64_t ipis;
	pid_t pid;
	int status;

	task_report = report;
	per_cpu(gate_cpu_marker, 0) = 0xabc000UL;
	per_cpu(gate_cpu_marker, 1) = 0xabc001UL;
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
	ipis = READ_ONCE(*kobox_task_gate_ipis(1));
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
		kobox_task_gate_ipis(1), ipis + 1);
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
	if (READ_ONCE(*kobox_task_gate_ipis(1)) != primary.irq_ipis)
		return -EINVAL;
	smp_store_release(&primary.command,
			  (unsigned int)KOBOX_GATE_ENABLE_IRQ);
	status = gate_wait_phase(&secondary, KOBOX_GATE_EXITING);
	if (status)
		return status;
	status = gate_wait_phase(&primary, KOBOX_GATE_EXITING);
	if (status)
		return status;
	status = kobox_task_gate_join(secondary.task);
	if (!status)
		status = kobox_task_gate_join(primary.task);
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

bool kobox_task_smp_report_ready(const struct kobox_linux_task_report *task_report)
{
	return task_report->upstream_schedule_ready &&
		task_report->upstream_try_to_wake_up_ready &&
		task_report->current_percpu_ready && task_report->local_switch_ready &&
		task_report->remote_switch_ready && task_report->migration_ready &&
		task_report->affinity_ready && task_report->remote_reschedule_ipis &&
		task_report->preempt_disable_ready && task_report->irq_disable_ready &&
		task_report->exit_join_ready;
}
