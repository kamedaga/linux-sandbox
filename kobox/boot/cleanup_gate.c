// SPDX-License-Identifier: GPL-2.0-only

#include "cleanup_gate.h"
#include "host.h"

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#define GATE_WAIT (5 * HZ)
#define GATE_NS (5LL * NSEC_PER_SEC)
#define STAMP 0xc1ea7a5eU

enum callback_kind {
	HARD_IRQ, THREAD_IRQ, TIMER, HIGH_TIMER, WORK, BH_WORK, DELAYED,
	READER, RETIRE, FINAL_WORK, REGULAR_RCU, CALLBACK_KINDS,
};

enum cleanup_phase {
	LIVE, IRQ_WAIT, TIMER_WAIT, HRTIMER_WAIT, DELAYED_WAIT, BH_WAIT,
	GP_WAIT, WORK_WAIT, BARRIER_WAIT, FINAL_WAIT, REVOKED, DONE,
};

enum gate_error {
	BAD_CONTEXT = BIT(0), BAD_ORDER = BIT(1), HOLD_TIMEOUT = BIT(2),
	LATE_CALLBACK = BIT(3),
};

static const char * const names[] = {
	"hardirq", "threaded-irq", "timer", "hrtimer", "work", "bh-work",
	"delayed-work", "rcu-reader", "rcu-callback", "final-work", "stress",
};

struct cleanup_case;

struct gate_device {
	struct cleanup_case *test;
	u32 stamp;
	struct timer_list timer;
	struct hrtimer hrtimer;
	struct work_struct work;
	struct work_struct bh;
	struct delayed_work delayed;
	struct work_struct final;
	struct rcu_head regular;
	struct rcu_head retire;
	bool regular_queued;
	bool bh_queued;
	atomic_t retire_queued;
};

/* The interrupt source and the oracle outlive the device and its IRQ action. */
struct cleanup_case {
	struct kobox_linux_cleanup_report *report;
	struct gate_device __rcu *published;
	struct gate_device *device;
	struct workqueue_struct *wq;
	struct workqueue_struct *bh_wq;
	struct task_struct *starter;
	struct task_struct *cleaner;
	struct hrtimer source;
	struct hrtimer probe;
	raw_spinlock_t admission;
	struct completion start_cleanup;
	struct completion started;
	struct completion cleaned;
	struct completion release;
	atomic_t calls[CALLBACK_KINDS];
	atomic_t active[CALLBACK_KINDS];
	atomic_t denied[CALLBACK_KINDS];
	atomic_t errors;
	atomic_t rejected;
	atomic_t attempts;
	atomic_t acknowledged;
	atomic_t masked;
	atomic_t freed;
	atomic_t final_done;
	unsigned int cpu;
	unsigned int target;
	unsigned int phase;
	int irq;
	bool closing;
	bool retire_allowed;
	bool held;
	bool released;
	bool probed;
	bool gp_done;
	bool barrier_done;
	bool finished;
	ktime_t probe_deadline;
};

static int fail(struct cleanup_case *test, unsigned int line)
{
	test->report->line = line;
	/* Acquire the cleanup task's diagnostic phase publication. */
	test->report->phase = smp_load_acquire(&test->phase);
	test->report->errors = atomic_read(&test->errors);
	test->report->warnings = kobox_linux_exception_warnings();
	return -EINVAL;
}

static int await_flag(bool *flag)
{
	ktime_t deadline = ktime_get() + GATE_NS;

	/* Pair with the callback/task's preceding observations and publication. */
	while (!smp_load_acquire(flag)) {
		if (ktime_get() >= deadline)
			return -ETIMEDOUT;
		usleep_range(500, 1000);
	}
	return 0;
}

static void release_callback(struct cleanup_case *test)
{
	/* Publish the controller/probe's observations before the callback resumes. */
	smp_store_release(&test->released, true);
	complete_all(&test->release);
}

static void enter_callback(struct gate_device *device, unsigned int kind)
{
	struct cleanup_case *test = device->test;
	ktime_t deadline = ktime_get() + GATE_NS;
	bool atomic_context = kind == HARD_IRQ || kind == HIGH_TIMER ||
		kind == TIMER || kind == BH_WORK || kind == RETIRE ||
		kind == REGULAR_RCU || kind == READER;

	if (device->stamp != STAMP || atomic_read(&test->freed))
		atomic_or(LATE_CALLBACK, &test->errors);
	preempt_disable();
	if (current != raw_cpu_read(current_task) ||
	    ((kind == HARD_IRQ || kind == HIGH_TIMER) != !!in_hardirq()) ||
	    ((kind == TIMER || kind == BH_WORK || kind == RETIRE ||
	      kind == REGULAR_RCU) && !in_serving_softirq()))
		atomic_or(BAD_CONTEXT, &test->errors);
	preempt_enable();
	atomic_inc(&test->calls[kind]);
	atomic_inc(&test->active[kind]);
	/* Acquire the controller's release; otherwise publish this live callback. */
	if (smp_load_acquire(&test->released) || test->target >= REGULAR_RCU ||
	    test->target != kind)
		return;
	/* The controller must see active/calls before attempting cleanup. */
	smp_store_release(&test->held, true);
	if (atomic_context) {
		/* A normal RCU reader may be preempted, but must never sleep. */
		while (!smp_load_acquire(&test->released) && ktime_get() < deadline)
			cpu_relax();
		/* Acquire the release even when the watchdog ends the loop. */
		if (!smp_load_acquire(&test->released))
			atomic_or(HOLD_TIMEOUT, &test->errors);
	} else if (!wait_for_completion_timeout(&test->release, GATE_WAIT)) {
		atomic_or(HOLD_TIMEOUT, &test->errors);
	}
}

static void leave_callback(struct gate_device *device, unsigned int kind)
{
	atomic_dec(&device->test->active[kind]);
}

static void regular_callback(struct rcu_head *head);

/* Check and enqueue share one lock: no check-then-rearm window at shutdown. */
static void admit(struct gate_device *device, unsigned int kind)
{
	struct cleanup_case *test = device->test;
	unsigned long flags;

	raw_spin_lock_irqsave(&test->admission, flags);
	if (test->closing) {
		atomic_inc(&test->rejected);
		atomic_inc(&test->denied[kind]);
	} else if (kind == WORK) {
		if (!timer_pending(&device->timer))
			mod_timer(&device->timer, jiffies + 1);
		if (!hrtimer_active(&device->hrtimer))
			hrtimer_start(&device->hrtimer, NSEC_PER_MSEC * 2,
				      HRTIMER_MODE_REL_PINNED_HARD);
		if (!device->bh_queued) {
			device->bh_queued = true;
			queue_work_on(test->cpu, test->bh_wq, &device->bh);
		}
		queue_delayed_work_on(test->cpu, test->wq, &device->delayed, 1);
		if (!device->regular_queued) {
			device->regular_queued = true;
			call_rcu(&device->regular, regular_callback);
		}
	} else {
		queue_work_on(test->cpu, test->wq, &device->work);
	}
	raw_spin_unlock_irqrestore(&test->admission, flags);
}

static void final_work(struct work_struct *work)
{
	struct gate_device *device = container_of(work, struct gate_device, final);
	struct cleanup_case *test = device->test;

	enter_callback(device, FINAL_WORK);
	/* The final consumer must observe the cleaner's completed reader GP. */
	if (!smp_load_acquire(&test->gp_done) ||
	    atomic_inc_return(&test->final_done) != 1)
		atomic_or(BAD_ORDER, &test->errors);
	leave_callback(device, FINAL_WORK);
}

static void retire_callback(struct rcu_head *head)
{
	struct gate_device *device = container_of(head, struct gate_device, retire);
	struct cleanup_case *test = device->test;

	enter_callback(device, RETIRE);
	/* One bounded teardown edge, not a reopening of normal admission. */
	if (!queue_work_on(test->cpu, test->wq, &device->final))
		atomic_or(BAD_ORDER, &test->errors);
	leave_callback(device, RETIRE);
}

static void normal_work(struct work_struct *work)
{
	struct gate_device *device = container_of(work, struct gate_device, work);
	struct cleanup_case *test = device->test;

	enter_callback(device, WORK);
	admit(device, WORK);
	/* The cleaner permits this finite edge only after the public-reader GP. */
	if (smp_load_acquire(&test->retire_allowed) &&
	    !atomic_cmpxchg(&device->retire_queued, 0, 1))
		call_rcu(&device->retire, retire_callback);
	leave_callback(device, WORK);
}

static void bh_work(struct work_struct *work)
{
	struct gate_device *device = container_of(work, struct gate_device, bh);

	enter_callback(device, BH_WORK);
	admit(device, BH_WORK);
	leave_callback(device, BH_WORK);
}

static void delayed_work(struct work_struct *work)
{
	struct gate_device *device = container_of(to_delayed_work(work),
						struct gate_device, delayed);

	enter_callback(device, DELAYED);
	admit(device, DELAYED);
	leave_callback(device, DELAYED);
}

static void timer_callback(struct timer_list *timer)
{
	struct gate_device *device = timer_container_of(device, timer, timer);

	enter_callback(device, TIMER);
	admit(device, TIMER);
	leave_callback(device, TIMER);
}

static enum hrtimer_restart high_timer_callback(struct hrtimer *timer)
{
	struct gate_device *device = container_of(timer, struct gate_device, hrtimer);

	enter_callback(device, HIGH_TIMER);
	admit(device, HIGH_TIMER);
	leave_callback(device, HIGH_TIMER);
	return HRTIMER_NORESTART;
}

static void regular_callback(struct rcu_head *head)
{
	struct gate_device *device = container_of(head, struct gate_device, regular);

	enter_callback(device, REGULAR_RCU);
	admit(device, REGULAR_RCU);
	leave_callback(device, REGULAR_RCU);
}

static irqreturn_t device_irq(int irq, void *argument)
{
	struct cleanup_case *test = argument;
	struct gate_device *device;
	irqreturn_t result = IRQ_HANDLED;

	rcu_read_lock();
	device = rcu_dereference(test->published);
	if (device) {
		enter_callback(device, HARD_IRQ);
		admit(device, HARD_IRQ);
		leave_callback(device, HARD_IRQ);
		result = IRQ_WAKE_THREAD;
	}
	rcu_read_unlock();
	return result;
}

static irqreturn_t device_irq_thread(int irq, void *argument)
{
	struct cleanup_case *test = argument;
	struct gate_device *device = test->device;

	/* synchronize_irq/free_irq, not RCU, protects a sleeping IRQ thread. */
	enter_callback(device, THREAD_IRQ);
	admit(device, THREAD_IRQ);
	leave_callback(device, THREAD_IRQ);
	return IRQ_HANDLED;
}

static void source_mask(struct irq_data *data)
{
	struct cleanup_case *test = irq_data_get_irq_chip_data(data);

	atomic_set(&test->masked, 1);
}

static void source_unmask(struct irq_data *data)
{
	struct cleanup_case *test = irq_data_get_irq_chip_data(data);

	atomic_set(&test->masked, 0);
}

static void source_ack(struct irq_data *data)
{
	struct cleanup_case *test = irq_data_get_irq_chip_data(data);

	atomic_inc(&test->acknowledged);
}

/* A fixture interrupt controller, with an actual mask and acknowledge path. */
static struct irq_chip source_chip = {
	.name = "cleanup-source",
	.irq_mask = source_mask,
	.irq_unmask = source_unmask,
	.irq_ack = source_ack,
};

static enum hrtimer_restart source_interrupt(struct hrtimer *timer)
{
	struct cleanup_case *test = container_of(timer, struct cleanup_case, source);

	atomic_inc(&test->attempts);
	if (!in_hardirq() || raw_smp_processor_id() != test->cpu)
		atomic_or(BAD_CONTEXT, &test->errors);
	if (!atomic_read(&test->masked)) {
		if (generic_handle_irq(test->irq))
			atomic_or(BAD_ORDER, &test->errors);
	} else {
		atomic_inc(&test->rejected);
	}
	hrtimer_forward_now(timer, NSEC_PER_MSEC);
	return HRTIMER_RESTART;
}

static int wait_for_stop(void)
{
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int start_device(void *argument)
{
	struct cleanup_case *test = argument;
	struct gate_device *device = test->device;

	/* Seed every producer before a deliberately held work can block rearming. */
	local_irq_disable();
	mod_timer(&device->timer, jiffies + 1);
	hrtimer_start(&device->hrtimer, NSEC_PER_MSEC * 2,
		      HRTIMER_MODE_REL_PINNED_HARD);
	queue_work_on(test->cpu, test->bh_wq, &device->bh);
	device->bh_queued = true;
	queue_delayed_work_on(test->cpu, test->wq, &device->delayed, 1);
	admit(device, READER);
	hrtimer_start(&test->source, NSEC_PER_MSEC, HRTIMER_MODE_REL_PINNED_HARD);
	complete(&test->started);
	local_irq_enable();
	if (test->target == READER) {
		rcu_read_lock();
		device = rcu_dereference(test->published);
		if (device) {
			enter_callback(device, READER);
			admit(device, READER);
			leave_callback(device, READER);
		} else {
			atomic_or(BAD_ORDER, &test->errors);
		}
		rcu_read_unlock();
	}
	return wait_for_stop();
}

static void set_phase(struct cleanup_case *test, unsigned int phase)
{
	/* Publish the completed predecessor before a cross-CPU waiter observes it. */
	smp_store_release(&test->phase, phase);
}

static int read_alias(const void *address, u32 *value)
{
	/* Probe the known fixture mapping using upstream's exception-table load.
	 * The native x86 copy_from_kernel_nofault address filter rejects all host
	 * userspace addresses, including this hosted kernel's own vmalloc range.
	 */
	pagefault_disable();
	__get_kernel_nofault(value, address, u32, fault);
	pagefault_enable();
	return 0;
fault:
	pagefault_enable();
	return -EFAULT;
}

static int cleanup_device(void *argument)
{
	struct cleanup_case *test = argument;
	struct gate_device *device = test->device;
	unsigned long flags;
	u32 probe;

	wait_for_completion(&test->start_cleanup);
	raw_spin_lock_irqsave(&test->admission, flags);
	test->closing = true;
	rcu_assign_pointer(test->published, NULL);
	raw_spin_unlock_irqrestore(&test->admission, flags);

	disable_irq_nosync(test->irq);
	set_phase(test, IRQ_WAIT);
	synchronize_irq(test->irq);
	set_phase(test, TIMER_WAIT);
	timer_shutdown_sync(&device->timer);
	set_phase(test, HRTIMER_WAIT);
	hrtimer_cancel(&device->hrtimer);
	set_phase(test, DELAYED_WAIT);
	disable_delayed_work_sync(&device->delayed);
	set_phase(test, BH_WAIT);
	disable_work_sync(&device->bh);
	set_phase(test, GP_WAIT);
	synchronize_rcu();
	/* Publish the GP result to the final worker and the controller's oracle. */
	smp_store_release(&test->gp_done, true);

	/* This final producer runs only after public readers have departed. */
	set_phase(test, WORK_WAIT);
	/* Pair with the final producer's acquire before it queues a callback. */
	smp_store_release(&test->retire_allowed, true);
	queue_work_on(test->cpu, test->wq, &device->work);
	flush_work(&device->work);
	disable_work_sync(&device->work);
	set_phase(test, BARRIER_WAIT);
	rcu_barrier();
	/* Separate callback completion publication from the earlier reader GP. */
	smp_store_release(&test->barrier_done, true);
	set_phase(test, FINAL_WAIT);
	flush_work(&device->final);
	disable_work_sync(&device->final);

	if (!atomic_read(&device->retire_queued) ||
	    atomic_read(&test->final_done) != 1 ||
	    atomic_read(&test->active[RETIRE]) ||
	    timer_pending(&device->timer) || hrtimer_active(&device->hrtimer) ||
	    work_busy(&device->work) || work_busy(&device->bh) ||
	    work_busy(&device->delayed.work) || work_busy(&device->final) ||
	    timer_pending(&device->delayed.timer))
		atomic_or(BAD_ORDER, &test->errors);

	/* Still-live storage: shutdown/disable must reject even raw late attempts. */
	mod_timer(&device->timer, jiffies + 1);
	if (timer_pending(&device->timer) ||
	    queue_work_on(test->cpu, test->wq, &device->work) ||
	    queue_work_on(test->cpu, test->bh_wq, &device->bh) ||
	    queue_delayed_work_on(test->cpu, test->wq, &device->delayed, 1))
		atomic_or(BAD_ORDER, &test->errors);
	destroy_workqueue(test->bh_wq);
	destroy_workqueue(test->wq);
	free_irq(test->irq, test);
	/* Never reclaim failed asynchronous state under a still-running callback. */
	if (atomic_read(&test->errors))
		goto done;
	device->stamp = 0;
	if (atomic_inc_return(&test->freed) != 1)
		atomic_or(BAD_ORDER, &test->errors);
	vfree(device);
	/* Upstream vmalloc batches TLB invalidation; explicitly drain lazy aliases
	 * before using an inaccessible mapping as the late-access oracle.
	 */
	vm_unmap_aliases();
	/* The host vmap alias really is revoked, not just a poisoned magic word. */
	if (read_alias(device, &probe) != -EFAULT)
		atomic_or(BAD_ORDER, &test->errors);
	set_phase(test, REVOKED);
done:
	/* Keep the final join wait distinguishable from waits inside teardown APIs. */
	smp_store_release(&test->finished, true);
	complete(&test->cleaned);
	return wait_for_stop();
}

static unsigned int held_phase(unsigned int target)
{
	static const unsigned int phases[] = {
		IRQ_WAIT, IRQ_WAIT, TIMER_WAIT, HRTIMER_WAIT, WORK_WAIT,
		BH_WAIT, DELAYED_WAIT, GP_WAIT, BARRIER_WAIT, FINAL_WAIT,
	};

	return phases[target];
}

static bool needs_atomic_probe(unsigned int target)
{
	return target == HARD_IRQ || target == TIMER || target == HIGH_TIMER ||
		target == BH_WORK;
}

static enum hrtimer_restart probe_cleanup(struct hrtimer *timer)
{
	struct cleanup_case *test = container_of(timer, struct cleanup_case, probe);

	/* Observe the cleaner inside its announced API, not just unscheduled. */
	if (smp_load_acquire(&test->phase) == held_phase(test->target) &&
	    current == test->cleaner &&
	    atomic_read(&test->active[test->target])) {
		/* Publish this interrupt's successful observation before releasing it. */
		smp_store_release(&test->probed, true);
		release_callback(test);
		return HRTIMER_NORESTART;
	}
	if (ktime_get() >= test->probe_deadline) {
		atomic_or(HOLD_TIMEOUT, &test->errors);
		release_callback(test);
		return HRTIMER_NORESTART;
	}
	hrtimer_forward_now(timer, NSEC_PER_MSEC);
	return HRTIMER_RESTART;
}

static int await_cleanup_wait(struct cleanup_case *test)
{
	ktime_t deadline = ktime_get() + GATE_NS;
	unsigned int phase = held_phase(test->target);

	do {
		/* Acquire both publications before considering the task's sleep state. */
		if (smp_load_acquire(&test->finished))
			return -EINVAL;
		/* A phase beyond this one would mean the API returned too early. */
		if (smp_load_acquire(&test->phase) > phase)
			return -EINVAL;
		/* Exclude merely unscheduled tasks and the final kthread join wait. */
		if (smp_load_acquire(&test->phase) == phase &&
		    wait_task_inactive(test->cleaner, TASK_NORMAL)) {
			if ((test->target == READER && test->gp_done) ||
			    (test->target == RETIRE &&
			     (!test->gp_done || test->barrier_done)) ||
			    (test->target == FINAL_WORK &&
			     (!test->barrier_done || atomic_read(&test->final_done))))
				return -EINVAL;
			return 0;
		}
		usleep_range(500, 1000);
	} while (ktime_get() < deadline);
	return -ETIMEDOUT;
}

static struct cleanup_case *new_case(struct kobox_linux_cleanup_report *report,
				     unsigned int cpu, unsigned int target)
{
	struct cleanup_case *test;
	struct gate_device *device;
	struct task_struct *irq_thread;
	ktime_t deadline;
	u32 stamp;

	test = kzalloc(sizeof(*test), GFP_KERNEL);
	device = vzalloc(sizeof(*device));
	if (!test || !device)
		return NULL;
	test->report = report;
	test->device = device;
	test->cpu = cpu;
	test->target = target;
	device->test = test;
	device->stamp = STAMP;
	if (read_alias(&device->stamp, &stamp) || stamp != STAMP)
		return NULL;
	raw_spin_lock_init(&test->admission);
	init_completion(&test->start_cleanup);
	init_completion(&test->started);
	init_completion(&test->cleaned);
	init_completion(&test->release);
	timer_setup(&device->timer, timer_callback, 0);
	hrtimer_setup(&device->hrtimer, high_timer_callback, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_PINNED_HARD);
	hrtimer_setup(&test->source, source_interrupt, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_PINNED_HARD);
	hrtimer_setup(&test->probe, probe_cleanup, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_PINNED_HARD);
	INIT_WORK(&device->work, normal_work);
	INIT_WORK(&device->bh, bh_work);
	INIT_WORK(&device->final, final_work);
	INIT_DELAYED_WORK(&device->delayed, delayed_work);
	test->wq = alloc_workqueue("cleanup-work", 0, 0);
	test->bh_wq = alloc_workqueue("cleanup-bh", WQ_BH, 0);
	if (!test->wq || !test->bh_wq)
		return NULL;
	test->irq = irq_alloc_descs(-1, 1, 1, NUMA_NO_NODE);
	if (test->irq < 0)
		return NULL;
	atomic_set(&test->masked, 1);
	irq_set_chip_data(test->irq, test);
	irq_set_chip_and_handler(test->irq, &source_chip, handle_level_irq);
	irq_clear_status_flags(test->irq, IRQ_NOREQUEST);
	if (request_threaded_irq(test->irq, device_irq, device_irq_thread,
				 IRQF_ONESHOT | IRQF_NOBALANCING,
				 "cleanup-device", test))
		return NULL;
	if (target < REGULAR_RCU) {
		/* A deliberately held softirq must not starve an IRQ thread needed
		 * by an earlier teardown phase. Let upstream finish its initial
		 * affinity setup before placing this test thread on the other CPU.
		 * Stress cases retain the ordinary IRQ-thread affinity.
		 */
		irq_thread = irq_to_desc(test->irq)->action->thread;
		deadline = ktime_get() + GATE_NS;
		while (!wait_task_inactive(irq_thread, TASK_INTERRUPTIBLE)) {
			if (ktime_get() >= deadline)
				return NULL;
			usleep_range(500, 1000);
		}
		if (set_cpus_allowed_ptr(irq_thread, cpumask_of(cpu ^ 1)))
			return NULL;
	}
	rcu_assign_pointer(test->published, device);
	/* All kthreads must exist before any CPU is held in IRQ/softirq context. */
	test->starter = kthread_create(start_device, test, "cleanup-start/%u", cpu);
	test->cleaner = kthread_create(cleanup_device, test, "cleanup-stop/%u", cpu);
	if (IS_ERR(test->starter) || IS_ERR(test->cleaner))
		return NULL;
	kthread_bind(test->starter, cpu);
	kthread_bind(test->cleaner, cpu ^ 1);
	return test;
}

static int run_case(struct kobox_linux_cleanup_report *report,
		    unsigned int cpu, unsigned int target, unsigned int iteration)
{
	struct cleanup_case *test;
	unsigned int index, callbacks = 0, attempts, rejected;
	ktime_t deadline;
	int snapshot[CALLBACK_KINDS];

	strscpy(report->scenario, names[target], sizeof(report->scenario));
	report->cpu = cpu;
	report->phase = LIVE;
	test = new_case(report, cpu, target);
	if (!test)
		return -ENOMEM;
	wake_up_process(test->cleaner);
	wake_up_process(test->starter);
	if (!wait_for_completion_timeout(&test->started, GATE_WAIT))
		return fail(test, __LINE__);
	if (target < RETIRE) {
		if (await_flag(&test->held))
			return fail(test, __LINE__);
	} else if (target == REGULAR_RCU) {
		/* Vary the cut across tick, delayed work and callback delivery. */
		usleep_range(15000 + iteration * 731, 16000 + iteration * 731);
	}
	deadline = ktime_get() + GATE_NS;
	while (!atomic_read(&test->calls[HARD_IRQ])) {
		if (ktime_get() >= deadline)
			return fail(test, __LINE__);
		usleep_range(500, 1000);
	}
	if (target == THREAD_IRQ && synchronize_hardirq(test->irq))
		return fail(test, __LINE__);
	if (target < RETIRE && needs_atomic_probe(target)) {
		test->probe_deadline = ktime_get() + GATE_NS;
		hrtimer_start(&test->probe, NSEC_PER_MSEC * 10,
			      HRTIMER_MODE_REL_PINNED_HARD);
	}
	complete(&test->start_cleanup);
	if (target < REGULAR_RCU && !needs_atomic_probe(target)) {
		if (await_flag(&test->held) || await_cleanup_wait(test))
			return fail(test, __LINE__);
		release_callback(test);
		report->probes++;
	}
	if (!wait_for_completion_timeout(&test->cleaned, GATE_WAIT))
		return fail(test, __LINE__);
	hrtimer_cancel(&test->probe);
	if (target < RETIRE && needs_atomic_probe(target)) {
		/* Acquire the hard timer's completed observation, not merely expiry. */
		if (!smp_load_acquire(&test->probed))
			return fail(test, __LINE__);
		report->probes++;
	}
	if (atomic_read(&test->errors) || atomic_read(&test->freed) != 1 ||
	    !test->gp_done || !test->barrier_done ||
	    !atomic_read(&test->acknowledged) ||
	    (target < RETIRE && !atomic_read(&test->denied[target])))
		return fail(test, __LINE__);
	for (index = 0; index < CALLBACK_KINDS; index++) {
		snapshot[index] = atomic_read(&test->calls[index]);
		if (atomic_read(&test->active[index]) ||
		    (target == REGULAR_RCU && index != READER && !snapshot[index]))
			return fail(test, __LINE__);
		callbacks += snapshot[index];
	}
	attempts = atomic_read(&test->attempts);
	rejected = atomic_read(&test->rejected);
	/* The source stays alive after vfree; delivery must remain masked. */
	usleep_range(20000, 25000);
	if (atomic_read(&test->attempts) <= attempts ||
	    atomic_read(&test->rejected) <= rejected ||
	    atomic_read(&test->errors) || atomic_read(&test->freed) != 1)
		return fail(test, __LINE__);
	for (index = 0; index < CALLBACK_KINDS; index++) {
		if (atomic_read(&test->calls[index]) != snapshot[index] ||
		    atomic_read(&test->active[index]))
			return fail(test, __LINE__);
	}
	hrtimer_cancel(&test->source);
	irq_set_status_flags(test->irq, IRQ_NOREQUEST);
	irq_set_chip_and_handler(test->irq, NULL, NULL);
	irq_set_chip_data(test->irq, NULL);
	irq_free_descs(test->irq, 1);
	if (kthread_stop(test->starter) || kthread_stop(test->cleaner))
		return fail(test, __LINE__);
	report->callbacks += callbacks;
	report->rejected += atomic_read(&test->rejected);
	report->freed += atomic_read(&test->freed);
	report->phase = DONE;
	report->cases++;
	kfree(test);
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_cleanup_verify(struct kobox_linux_cleanup_report *report)
{
	cpumask_t saved;
	unsigned int cpu, target, iteration;
	int status;

	if (!report || report->size != sizeof(*report) ||
	    system_state != SYSTEM_RUNNING || num_online_cpus() != 2 ||
	    kobox_linux_exception_warnings())
		return -EINVAL;
	cpumask_copy(&saved, current->cpus_ptr);
	for_each_online_cpu(cpu) {
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu ^ 1)))
			return -EINVAL;
		for (target = HARD_IRQ; target < REGULAR_RCU; target++) {
			status = run_case(report, cpu, target, 0);
			if (status)
				return status;
		}
		for (iteration = 0; iteration < 16; iteration++) {
			status = run_case(report, cpu, REGULAR_RCU, iteration);
			if (status)
				return status;
		}
	}
	report->warnings = kobox_linux_exception_warnings();
	if (report->warnings)
		return -EINVAL;
	return set_cpus_allowed_ptr(current, &saved);
}
