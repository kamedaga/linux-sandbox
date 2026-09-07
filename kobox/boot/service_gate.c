// SPDX-License-Identifier: GPL-2.0-only

#include "service_gate.h"
#include "../task/boot.h"

#include <linux/completion.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/irq_work.h>
#include <linux/jiffies.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define GATE_TASKLET_RUNS 256U
#define GATE_WAIT (5 * HZ)

struct service_cpu {
	struct irq_work irq;
	struct hrtimer tasklet_irq;
	struct tasklet_struct tasklet;
	struct work_struct work;
	struct completion irq_done;
	struct completion tasklet_done;
	struct completion work_done;
	struct kobox_linux_boot_report *report;
	unsigned int cpu;
	bool invalid;
};

struct service_rcu {
	struct rcu_head head;
	struct completion *done;
	struct kobox_linux_boot_report *report;
};

static void gate_irq_callback(struct irq_work *work)
{
	struct service_cpu *slot = container_of(work, struct service_cpu, irq);

	/* Remote irq_work can also run from idle's call-single queue flush. */
	if (!irqs_disabled() || raw_smp_processor_id() != slot->cpu ||
	    current != raw_cpu_read(current_task))
		slot->invalid = true;
	slot->report->irq_callbacks[slot->cpu]++;
	complete(&slot->irq_done);
}

static enum hrtimer_restart gate_tasklet_irq(struct hrtimer *timer)
{
	struct service_cpu *slot = container_of(timer, struct service_cpu,
					      tasklet_irq);

	if (!in_hardirq() || raw_smp_processor_id() != slot->cpu ||
	    current != raw_cpu_read(current_task))
		slot->invalid = true;
	slot->report->irq_callbacks[slot->cpu]++;
	tasklet_schedule(&slot->tasklet);
	return HRTIMER_NORESTART;
}

static void arm_tasklet_irq(void *argument)
{
	struct service_cpu *slot = argument;

	hrtimer_setup(&slot->tasklet_irq, gate_tasklet_irq, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_PINNED_HARD);
	hrtimer_start(&slot->tasklet_irq, NSEC_PER_MSEC,
		      HRTIMER_MODE_REL_PINNED_HARD);
}

static void gate_tasklet_callback(struct tasklet_struct *tasklet)
{
	struct service_cpu *slot = container_of(tasklet, struct service_cpu, tasklet);
	unsigned int cpu = raw_smp_processor_id();

	if (!in_serving_softirq() || cpu != slot->cpu ||
	    current != raw_cpu_read(current_task))
		slot->invalid = true;
	if (current == this_cpu_read(ksoftirqd))
		slot->report->ksoftirqd_runs[slot->cpu]++;
	else
		slot->report->irq_exit_tasklet_runs[slot->cpu]++;
	if (++slot->report->tasklet_runs[slot->cpu] < GATE_TASKLET_RUNS ||
	    !slot->report->ksoftirqd_runs[slot->cpu] ||
	    !slot->report->irq_exit_tasklet_runs[slot->cpu])
		tasklet_schedule(tasklet);
	else
		complete(&slot->tasklet_done);
}

static void gate_work_callback(struct work_struct *work)
{
	struct service_cpu *slot = container_of(work, struct service_cpu, work);

	if (!(current->flags & PF_WQ_WORKER) ||
	    raw_smp_processor_id() != slot->cpu || in_interrupt())
		slot->invalid = true;
	slot->report->work_runs[slot->cpu]++;
	complete(&slot->work_done);
}

static void gate_rcu_callback(struct rcu_head *head)
{
	struct service_rcu *item = container_of(head, struct service_rcu, head);
	struct completion *done = item->done;

	item->report->rcu_callbacks++;
	kfree(item);
	complete(done);
}

static int inspect_service_threads(struct kobox_linux_boot_report *report)
{
	struct task_struct *group, *task;
	unsigned int cpu;

	for_each_online_cpu(cpu)
		if (!per_cpu(ksoftirqd, cpu) ||
		    !(per_cpu(ksoftirqd, cpu)->flags & PF_KTHREAD))
			return -EINVAL;
	rcu_read_lock();
	for_each_process_thread(group, task) {
		char name[TASK_COMM_LEN];

		get_task_comm(name, task);
		if (!strcmp(name, "rcu_preempt") && task->flags & PF_KTHREAD)
			report->rcu_threads++;
		if (task->flags & PF_WQ_WORKER)
			report->worker_threads++;
	}
	rcu_read_unlock();
	return report->rcu_threads && report->worker_threads ? 0 : -EINVAL;
}

__attribute__((visibility("default")))
int kobox_linux_boot_verify(struct kobox_linux_boot_report *report)
{
	struct service_cpu *slots;
	struct service_rcu *item;
	struct completion *rcu_done;
	cpumask_t saved_affinity;
	unsigned int cpu;
	int status;

	if (!report || report->size != sizeof(*report) ||
	    task_pid_nr(current) != 1 || system_state != SYSTEM_RUNNING ||
	    num_online_cpus() != KOBOX_LINUX_MEMORY_LOGICAL_CPUS ||
	    !rcu_inkernel_boot_has_ended())
		return -EINVAL;
	report->phase = 1;
	report->warnings = kobox_linux_exception_warnings();
	if (report->warnings || !system_wq || !system_highpri_wq ||
	    !system_bh_wq || !system_bh_highpri_wq)
		return -EINVAL;
	status = inspect_service_threads(report);
	if (status)
		return status;
	/* Force one genuinely local and one remote irq_work submission. A
	 * migrating test controller could otherwise exercise the same path twice.
	 */
	cpumask_copy(&saved_affinity, current->cpus_ptr);
	status = set_cpus_allowed_ptr(current, cpumask_of(0));
	if (status)
		return status;
	slots = kcalloc(KOBOX_LINUX_MEMORY_LOGICAL_CPUS, sizeof(*slots), GFP_KERNEL);
	item = kzalloc(sizeof(*item), GFP_KERNEL);
	rcu_done = kzalloc(sizeof(*rcu_done), GFP_KERNEL);
	if (!slots || !item || !rcu_done) {
		kfree(slots);
		kfree(item);
		kfree(rcu_done);
		return -ENOMEM;
	}
	for_each_online_cpu(cpu) {
		struct service_cpu *slot = &slots[cpu];

		slot->report = report;
		slot->cpu = cpu;
		init_completion(&slot->irq_done);
		init_completion(&slot->tasklet_done);
		init_completion(&slot->work_done);
		slot->irq = IRQ_WORK_INIT_HARD(gate_irq_callback);
		tasklet_setup(&slot->tasklet, gate_tasklet_callback);
		INIT_WORK(&slot->work, gate_work_callback);
	}
	report->phase = 2;
	for_each_online_cpu(cpu) {
		struct service_cpu *slot = &slots[cpu];

		if (!irq_work_queue_on(&slot->irq, cpu) ||
		    !wait_for_completion_timeout(&slot->irq_done, GATE_WAIT))
			return -ETIMEDOUT;
		irq_work_sync(&slot->irq);
		if (slot->invalid || report->irq_callbacks[cpu] != 1)
			return -EINVAL;
	}
	report->phase = 3;
	for_each_online_cpu(cpu) {
		struct service_cpu *slot = &slots[cpu];
		u64 deadline;

		/* An arbitrary remote timer can interrupt ksoftirqd itself, which
		 * legitimately defers every tasklet until it resumes. Start from
		 * this ordinary task on each CPU instead: retain task ownership,
		 * but leave IRQs and BH enabled for the actual timer IRQ/exit.
		 */
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
			return -EINVAL;
		preempt_disable();
		arm_tasklet_irq(slot);
		deadline = ktime_get_mono_fast_ns() + NSEC_PER_SEC;
		while (!READ_ONCE(report->irq_exit_tasklet_runs[cpu])) {
			if (ktime_get_mono_fast_ns() >= deadline) {
				preempt_enable();
				return -ETIMEDOUT;
			}
			cpu_relax();
		}
		preempt_enable();
		if (!wait_for_completion_timeout(&slot->tasklet_done, GATE_WAIT))
			return -ETIMEDOUT;
		hrtimer_cancel(&slot->tasklet_irq);
		tasklet_kill(&slot->tasklet);
		if (slot->invalid || report->tasklet_runs[cpu] < GATE_TASKLET_RUNS ||
		    report->irq_callbacks[cpu] != 2 ||
		    !report->irq_exit_tasklet_runs[cpu] || !report->ksoftirqd_runs[cpu])
			return -EINVAL;
	}
	report->phase = 4;
	for_each_online_cpu(cpu) {
		struct service_cpu *slot = &slots[cpu];

		if (!queue_work_on(cpu, system_wq, &slot->work) ||
		    !wait_for_completion_timeout(&slot->work_done, GATE_WAIT))
			return -ETIMEDOUT;
		flush_work(&slot->work);
		if (slot->invalid || report->work_runs[cpu] != 1)
			return -EINVAL;
	}
	report->phase = 5;
	init_completion(rcu_done);
	item->done = rcu_done;
	item->report = report;
	call_rcu(&item->head, gate_rcu_callback);
	if (!wait_for_completion_timeout(rcu_done, GATE_WAIT))
		return -ETIMEDOUT;
	rcu_barrier();
	if (report->rcu_callbacks != 1)
		return -EINVAL;
	kfree(rcu_done);
	kfree(slots);
	status = set_cpus_allowed_ptr(current, &saved_affinity);
	if (status)
		return status;
	report->phase = 6;
	status = kobox_linux_task_verify_boot();
	report->warnings = kobox_linux_exception_warnings();
	if (status || report->warnings)
		return status ? status : -EINVAL;
	report->phase = 7;
	return 0;
}
