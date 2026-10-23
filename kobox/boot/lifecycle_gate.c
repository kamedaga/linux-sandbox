// SPDX-License-Identifier: GPL-2.0-only
#include "lifecycle_gate.h"
#include "lifecycle.h"

#include <linux/atomic.h>
#include <linux/completion.h>
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
/* Executor phases after the ordinary steps: the opening task executes an
 * item that sleeps (lend from schedule), then one that runs while a control
 * event arrives (lend from the IRQ path); each must be received meanwhile by
 * the deputy, and only ever by one task inside advance() at a time. */
enum {
	EXEC_IDLE,
	EXEC_SLEEP,
	EXEC_EVENT,
	EXEC_DONE,
};

static struct {
	struct task_struct *owner, *worker, *signal;
	wait_queue_head_t ready;
	atomic_t requested, completed, error;
	unsigned int collected;
	bool started, announced, finished, armed_window;
	struct completion lent;
	atomic_t advancing;
	int phase;
	bool event_executing, event_received, lent_denied;
	unsigned int deputy_advances, executed;
} gate;

static void async_gate_fail(int error)
{
	atomic_cmpxchg(&gate.error, 0, error);
}

static int async_gate_signal(void *unused)
{
	unsigned long deadline = jiffies + HZ;

	/* Raise the control event only while the opening task is executing,
	 * as a peer's doorbell would. Process context reaches the same entry. */
	while (!READ_ONCE(gate.event_executing)) {
		if (kthread_should_stop())
			return 0;
		if (time_after(jiffies, deadline)) {
			async_gate_fail(-ETIMEDOUT);
			return 0;
		}
		msleep(1);
	}
	local_irq_disable();
	kobox_linux_lifecycle_interrupt();
	local_irq_enable();
	while (!kthread_should_stop())
		msleep(1);
	return 0;
}

static void async_gate_sleep(void *unused)
{
	/* An ordinary Linux sleep: the opening task must lend from schedule(). */
	if (!wait_for_completion_timeout(&gate.lent, HZ))
		async_gate_fail(-ETIMEDOUT);
	/* Still lent until execute returns: the deputy is the receiver. */
	if (kobox_linux_lifecycle_receiving(gate.owner))
		gate.lent_denied = true;
	gate.executed++;
}

static void async_gate_event(void *unused)
{
	u64 deadline = ktime_get_mono_fast_ns() + NSEC_PER_SEC;

	/* Never sleeps: only the control event can lend the right here. */
	WRITE_ONCE(gate.event_executing, true);
	while (!READ_ONCE(gate.event_received)) {
		if (ktime_get_mono_fast_ns() > deadline) {
			async_gate_fail(-ETIMEDOUT);
			break;
		}
		cpu_relax();
	}
	if (kobox_linux_lifecycle_receiving(gate.owner))
		gate.lent_denied = true;
	gate.executed++;
}

static int async_gate_run(void *context)
{
	if (context != &gate || current != gate.owner)
		return -EINVAL;
	if (gate.collected != ASYNC_STEPS || gate.phase == EXEC_DONE)
		return 0;
	gate.phase++;
	if (gate.phase == EXEC_SLEEP)
		kobox_linux_lifecycle_execute(async_gate_sleep, NULL);
	else
		kobox_linux_lifecycle_execute(async_gate_event, NULL);
	if (!kobox_linux_lifecycle_receiving(gate.owner))
		return -EINVAL;
	if (gate.phase == EXEC_EVENT) {
		gate.phase = EXEC_DONE;
		return 1;
	}
	/* The event item still waits: a 1 would let the task sleep first. */
	return 2;
}

/* An advance() by the deputy while the opening task executes. */
static int async_gate_deputy(void)
{
	if (!kobox_linux_lifecycle_receiving(gate.owner))
		return -EINVAL;
	gate.deputy_advances++;
	if (gate.phase == EXEC_SLEEP && !completion_done(&gate.lent)) {
		complete(&gate.lent);
		/* Stay inside advance() until the executed item has returned, so
		 * the opening task must wait for this call to hand the right back. */
		mdelay(5);
		return 2;
	}
	if (gate.phase == EXEC_EVENT && !READ_ONCE(gate.event_received)) {
		WRITE_ONCE(gate.event_received, true);
		mdelay(5);
		return 2;
	}
	return 0;
}

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
	int error;

	if (context != &gate || service != &gate || current != gate.owner)
		return -EINVAL;
	error = kobox_linux_lifecycle_executor(async_gate_run, &gate);
	if (error)
		return error;
	gate.worker = kthread_create(async_gate_worker, NULL, "lifecycle-worker");
	if (IS_ERR(gate.worker))
		return PTR_ERR(gate.worker);
	gate.signal = kthread_create(async_gate_signal, NULL, "lifecycle-signal");
	if (IS_ERR(gate.signal)) {
		kthread_stop(gate.worker);
		return PTR_ERR(gate.signal);
	}
	kthread_bind(gate.signal, raw_smp_processor_id() ^ 1);
	wake_up_process(gate.signal);
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
	if (READ_ONCE(gate.collected) != ASYNC_STEPS)
		return 0;
	if (READ_ONCE(gate.phase) == EXEC_EVENT && !READ_ONCE(gate.event_received))
		return 2;
	return READ_ONCE(gate.phase) == EXEC_DONE ? 1 : 0;
}

static int async_gate_advance_locked(void *context, void *service);

static int async_gate_advance(void *context, void *service)
{
	int result;

	if (atomic_inc_return(&gate.advancing) != 1)
		async_gate_fail(-EBUSY);
	result = current == gate.owner ? async_gate_advance_locked(context, service) :
		 context == &gate && service == &gate ? async_gate_deputy() : -EINVAL;
	atomic_dec(&gate.advancing);
	return result;
}

static int async_gate_advance_locked(void *context, void *service)
{
	u64 deadline;
	unsigned int next = gate.collected + 1;

	if (context != &gate || service != &gate || current != gate.owner ||
	    !gate.announced || !kobox_linux_lifecycle_receiving(gate.owner))
		return -EINVAL;
	if (gate.collected == ASYNC_STEPS)
		return 0;
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
	kthread_stop(gate.signal);
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
	init_completion(&gate.lent);
	error = kobox_linux_lifecycle_serve(&host, &gate);
	set_cpus_allowed_ptr(current, &saved);
	if (!error && (!gate.finished || !gate.armed_window ||
		       gate.collected != ASYNC_STEPS || gate.phase != EXEC_DONE ||
		       gate.executed != 2 || gate.deputy_advances < 2 ||
		       gate.lent_denied || kobox_linux_lifecycle_gate_returns() < 1))
		error = -EINVAL;
	pr_info("LIFECYCLE_ASYNC status=%d completed=%u armed_window=%u joined=%u native_completions=0 executed=%u deputy_advances=%u returns=%u\n",
		error, gate.collected, gate.armed_window, gate.finished,
		gate.executed, gate.deputy_advances, kobox_linux_lifecycle_gate_returns());
	return error;
}
