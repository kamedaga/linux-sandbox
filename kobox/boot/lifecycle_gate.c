// SPDX-License-Identifier: GPL-2.0-only
#include "lifecycle_gate.h"
#include "lifecycle.h"

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/smp.h>
#include <linux/wait.h>

#define ASYNC_STEPS 16
#define ASYNC_ARMED_STEP 8

/* Callbacks remain reachable from the lifecycle control-event IRQ until this
 * fixture process exits. Do not leave its borrowed context on a returned stack.
 */
static struct {
	struct task_struct *owner, *worker;
	wait_queue_head_t ready;
	atomic_t requested, completed, error;
	unsigned int collected;
	bool started, announced, finished, armed_window;
} gate;

static int async_gate_worker(void *unused)
{
	unsigned int last = 0;
	unsigned long deadline;
	int step;

	while (!kthread_should_stop()) {
		wait_event_idle(gate.ready, kthread_should_stop() ||
			atomic_read(&gate.requested) != last);
		if (kthread_should_stop())
			break;
		step = atomic_read(&gate.requested);
		deadline = jiffies + HZ;
		while (READ_ONCE(gate.owner->__state) != TASK_INTERRUPTIBLE) {
			if (kthread_should_stop())
				return 0;
			if (time_after(jiffies, deadline)) {
				atomic_set(&gate.error, -ETIMEDOUT);
				wake_up_process(gate.owner);
				return 0;
			}
			msleep(1);
		}
		atomic_set_release(&gate.completed, step);
		/* No native IRQ or lifecycle completion is generated. This is the
		 * same actual Linux task wake used by the persistent FS workers.
		 */
		wake_up_process(gate.owner);
		last = step;
	}
	return 0;
}

static int async_gate_start(void *context, void *service)
{
	if (context != &gate || service != &gate || current != gate.owner)
		return -EINVAL;
	gate.worker = kthread_create(async_gate_worker, NULL, "lifecycle-worker");
	if (IS_ERR(gate.worker))
		return PTR_ERR(gate.worker);
	kthread_bind(gate.worker, raw_smp_processor_id() ^ 1);
	wake_up_process(gate.worker);
	gate.started = true;
	return 0;
}

static int async_gate_ready(void *context)
{
	if (context != &gate || !gate.started)
		return -EINVAL;
	gate.announced = true;
	return 0;
}

static int async_gate_pending(void *context)
{
	int error = atomic_read(&gate.error);

	if (error)
		return error;
	return READ_ONCE(gate.collected) == ASYNC_STEPS ? 1 : 0;
}

static int async_gate_advance(void *context, void *service)
{
	u64 deadline;
	unsigned int next = gate.collected + 1;

	if (context != &gate || service != &gate || current != gate.owner ||
	    !gate.announced)
		return -EINVAL;
	if (atomic_read_acquire(&gate.completed) == next) {
		gate.collected = next;
		return 2;
	}
	if (atomic_read(&gate.requested) != gate.collected)
		return 0;
	if (next == ASYNC_ARMED_STEP &&
	    READ_ONCE(current->__state) != TASK_INTERRUPTIBLE)
		return 0;
	atomic_set(&gate.requested, next);
	wake_up(&gate.ready);
	if (next != ASYNC_ARMED_STEP)
		return 0;
	/* Force wake-before-schedule in this test only. The real peer publishes
	 * while the owner is armed but still inside the final recheck callback.
	 * Return idle deliberately: schedule must not put the now-runnable owner
	 * to sleep, even though no native completion counter was incremented.
	 */
	deadline = ktime_get_mono_fast_ns() + NSEC_PER_SEC;
	while (atomic_read_acquire(&gate.completed) != next ||
	       READ_ONCE(current->__state) != TASK_RUNNING) {
		if (ktime_get_mono_fast_ns() > deadline)
			return -ETIMEDOUT;
		cpu_relax();
	}
	gate.armed_window = true;
	return 0;
}

static int async_gate_finish(void *context, void *service)
{
	if (context != &gate || service != &gate || current != gate.owner)
		return -EINVAL;
	kthread_stop(gate.worker);
	gate.finished = true;
	return 0;
}

int kobox_linux_lifecycle_async_verify(void)
{
	struct kobox_linux_lifecycle host = {
		.size = sizeof(host), .context = &gate,
		.ready = async_gate_ready, .pending = async_gate_pending,
		.start = async_gate_start, .advance = async_gate_advance,
		.finish = async_gate_finish,
	};
	cpumask_t saved;
	unsigned int cpu;
	int error;

	if (num_online_cpus() != 2 || task_pid_nr(current) != 1)
		return -EINVAL;
	cpumask_copy(&saved, current->cpus_ptr);
	cpu = get_cpu();
	put_cpu();
	error = set_cpus_allowed_ptr(current, cpumask_of(cpu));
	if (error)
		return error;
	gate.owner = current;
	init_waitqueue_head(&gate.ready);
	error = kobox_linux_lifecycle_serve(&host, &gate);
	set_cpus_allowed_ptr(current, &saved);
	if (!error && (!gate.finished || !gate.armed_window ||
		       gate.collected != ASYNC_STEPS))
		error = -EINVAL;
	pr_info("LIFECYCLE_ASYNC status=%d completed=%u armed_window=%u joined=%u native_completions=0\n",
		error, gate.collected, gate.armed_window, gate.finished);
	return error;
}
