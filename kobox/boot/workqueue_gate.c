// SPDX-License-Identifier: GPL-2.0-only

#include "workqueue_gate.h"
#include "host.h"

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#define GATE_WAIT (5 * HZ)
#define GATE_NS (5LL * NSEC_PER_SEC)
#define ITEM_COUNT 16
#define STAMP 0x71ab2c93U

enum queue_kind { NORMAL, HIGHPRI, BH, BH_HIGHPRI, UNBOUND, ORDERED, KINDS };
enum waiter_op {
	FLUSH_ONE, FLUSH_QUEUE, DRAIN_QUEUE, CANCEL_SYNC, FLUSH_DELAYED,
	CANCEL_DELAYED_SYNC,
};
enum errors { CONTEXT = BIT(0), ORDER = BIT(1), TIMEOUT = BIT(2), EARLY = BIT(3) };

static const char * const queue_names[] = {
	"normal", "highpri", "bh", "bh-highpri", "unbound", "ordered",
};

struct wq_case;

struct gate_item {
	struct work_struct work;
	struct delayed_work delayed;
	struct wq_case *test;
	struct completion release;
	atomic_t runs;
	atomic_t active;
	bool held;
	bool released;
	bool started;
	bool requeue;
	bool requeue_before_hold;
	bool delayed_mode;
	struct task_struct *runner;
	unsigned int seen_cpu;
	unsigned int limit;
	unsigned int stamp;
	unsigned int index;
	unsigned long not_before;
	u64 result;
};

struct gate_waiter {
	struct task_struct *task;
	struct wq_case *test;
	struct gate_item *item;
	struct completion done;
	enum waiter_op op;
	bool started;
	bool finished;
	bool result;
};

struct wq_case {
	struct kobox_linux_workqueue_report *report;
	struct workqueue_struct *wq;
	struct gate_item items[ITEM_COUNT];
	struct gate_waiter waiter;
	enum queue_kind kind;
	unsigned int cpu;
	bool any_cpu;
	bool ordered_check;
	atomic_t sequence;
	atomic_t active;
	atomic_t errors;
};

static bool is_bh(struct wq_case *test)
{
	return test->kind == BH || test->kind == BH_HIGHPRI;
}

static int fail(struct wq_case *test, unsigned int line)
{
	test->report->line = line;
	test->report->errors = atomic_read(&test->errors);
	test->report->warnings = kobox_linux_exception_warnings();
	return -EINVAL;
}

static int await_flag(bool *flag)
{
	ktime_t deadline = ktime_get() + GATE_NS;

	/* Acquire publication of the callback/waiter's preceding observations. */
	while (!smp_load_acquire(flag)) {
		if (ktime_get() >= deadline)
			return -ETIMEDOUT;
		usleep_range(500, 1000);
	}
	return 0;
}

static struct work_struct *item_work(struct gate_item *item)
{
	return item->delayed_mode ? &item->delayed.work : &item->work;
}

static bool submit(struct gate_item *item, unsigned long delay)
{
	struct wq_case *test = item->test;

	if (item->delayed_mode)
		return queue_delayed_work_on(test->cpu, test->wq,
					    &item->delayed, delay);
	return queue_work_on(test->cpu, test->wq, &item->work);
}

static void release_item(struct gate_item *item)
{
	WRITE_ONCE(item->released, true);
	complete_all(&item->release);
}

static void execute_item(struct gate_item *item)
{
	struct wq_case *test = item->test;
	ktime_t deadline = ktime_get() + GATE_NS;
	unsigned int run;

	preempt_disable();
	item->runner = current;
	item->seen_cpu = raw_smp_processor_id();
	if (current != raw_cpu_read(current_task) ||
	    (!test->any_cpu && raw_smp_processor_id() != test->cpu) ||
	    irqs_disabled())
		atomic_or(CONTEXT, &test->errors);
	if (is_bh(test)) {
		if (!in_serving_softirq() || in_hardirq())
			atomic_or(CONTEXT, &test->errors);
	} else if (in_interrupt() || !(current->flags & PF_WQ_WORKER) ||
		   current_work() != item_work(item) ||
		   (test->kind == HIGHPRI ? task_nice(current) >= 0 :
					   task_nice(current) != 0)) {
		atomic_or(CONTEXT, &test->errors);
	}
	preempt_enable();
	if (atomic_inc_return(&item->active) != 1 ||
	    READ_ONCE(item->stamp) != STAMP ||
	    (item->not_before && time_before(jiffies, item->not_before)))
		atomic_or(ORDER, &test->errors);
	if (atomic_inc_return(&test->active) != 1 && test->ordered_check)
		atomic_or(ORDER, &test->errors);
	run = atomic_inc_return(&item->runs);
	if (item->requeue_before_hold && run == 1 && !submit(item, 0))
		atomic_or(ORDER, &test->errors);
	if (test->ordered_check && atomic_fetch_inc(&test->sequence) != item->index)
		atomic_or(ORDER, &test->errors);
	/* The hold starts only after the workqueue has entered this callback. */
	smp_store_release(&item->started, true);
	if (item->held) {
		if (is_bh(test)) {
			while (!READ_ONCE(item->released) && ktime_get() < deadline)
				cpu_relax();
			if (!item->released)
				atomic_or(TIMEOUT, &test->errors);
		} else if (!wait_for_completion_timeout(&item->release, GATE_WAIT)) {
			atomic_or(TIMEOUT, &test->errors);
		}
	}
	if (item->requeue && run < item->limit)
		submit(item, 0);
	WRITE_ONCE(item->result, STAMP);
	atomic_dec(&test->active);
	atomic_dec(&item->active);
}

static void work_callback(struct work_struct *work)
{
	execute_item(container_of(work, struct gate_item, work));
}

static void delayed_callback(struct work_struct *work)
{
	execute_item(container_of(to_delayed_work(work), struct gate_item, delayed));
}

static void init_item(struct wq_case *test, unsigned int index, bool delayed)
{
	struct gate_item *item = &test->items[index];

	memset(item, 0, sizeof(*item));
	item->test = test;
	item->index = index;
	item->stamp = STAMP;
	item->delayed_mode = delayed;
	init_completion(&item->release);
	INIT_WORK(&item->work, work_callback);
	INIT_DELAYED_WORK(&item->delayed, delayed_callback);
}

static int waiter_main(void *argument)
{
	struct gate_waiter *waiter = argument;
	struct gate_item *item = waiter->item;

	/* No intervening blocking operation before the tested upstream API. */
	smp_store_release(&waiter->started, true);
	switch (waiter->op) {
	case FLUSH_ONE:
		waiter->result = flush_work(item_work(item));
		break;
	case FLUSH_QUEUE:
		__flush_workqueue(waiter->test->wq);
		break;
	case DRAIN_QUEUE:
		drain_workqueue(waiter->test->wq);
		break;
	case CANCEL_SYNC:
		waiter->result = cancel_work_sync(&item->work);
		break;
	case FLUSH_DELAYED:
		waiter->result = flush_delayed_work(&item->delayed);
		break;
	case CANCEL_DELAYED_SYNC:
		waiter->result = cancel_delayed_work_sync(&item->delayed);
		break;
	}
	/* Publish the API return before allowing the controller to join. */
	smp_store_release(&waiter->finished, true);
	complete(&waiter->done);
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int prepare_waiter(struct wq_case *test, struct gate_item *item,
			  enum waiter_op op)
{
	struct gate_waiter *waiter = &test->waiter;

	memset(waiter, 0, sizeof(*waiter));
	waiter->test = test;
	waiter->item = item;
	waiter->op = op;
	init_completion(&waiter->done);
	waiter->task = kthread_create(waiter_main, waiter, "wq-gate-wait");
	if (IS_ERR(waiter->task))
		return PTR_ERR(waiter->task);
	kthread_bind(waiter->task, test->cpu ^ 1);
	return 0;
}

static int start_waiter(struct wq_case *test)
{
	wake_up_process(test->waiter.task);
	return await_flag(&test->waiter.started);
}

static int waiter_inside(struct wq_case *test, bool cancel)
{
	struct gate_waiter *waiter = &test->waiter;
	ktime_t deadline = ktime_get() + GATE_NS;

	do {
		/* The final kthread join wait is not a wait inside the API. */
		if (smp_load_acquire(&waiter->finished))
			return fail(test, __LINE__);
		if (cancel) {
			/* Observe Linux's temporary disable, never write work data. */
			unsigned long data = atomic_long_read(&item_work(waiter->item)->data);

			if (!(data & WORK_STRUCT_PWQ) && (data & WORK_OFFQ_DISABLE_MASK) &&
			    wait_task_inactive(waiter->task, TASK_NORMAL))
				return 0;
		} else if (wait_task_inactive(waiter->task, TASK_NORMAL)) {
			return 0;
		}
		usleep_range(500, 1000);
	} while (ktime_get() < deadline);
	return fail(test, __LINE__);
}

static int join_waiter(struct wq_case *test)
{
	if (!wait_for_completion_timeout(&test->waiter.done, GATE_WAIT) ||
	    kthread_stop(test->waiter.task))
		return fail(test, __LINE__);
	return 0;
}

static int held_work(struct wq_case *test, struct gate_item *item)
{
	item->held = true;
	if (!submit(item, 0) || await_flag(&item->started))
		return fail(test, __LINE__);
	return 0;
}

static int verify_items(struct wq_case *test)
{
	unsigned int i;

	for (i = 0; i < ITEM_COUNT; i++) {
		struct gate_item *item = &test->items[i];

		if (work_busy(item_work(item)) || timer_pending(&item->delayed.timer) ||
		    atomic_read(&item->active) ||
		    (atomic_read(&item->runs) && READ_ONCE(item->result) != STAMP))
			return fail(test, __LINE__);
		test->report->callbacks += atomic_read(&item->runs);
	}
	if (atomic_read(&test->errors) || kobox_linux_exception_warnings())
		return fail(test, __LINE__);
	test->report->cases++;
	return 0;
}

static int end_case(struct wq_case *test)
{
	__flush_workqueue(test->wq);
	return verify_items(test);
}

static void begin_case(struct wq_case *test, const char *scenario)
{
	unsigned int i;

	strscpy(test->report->scenario, scenario, sizeof(test->report->scenario));
	for (i = 0; i < ITEM_COUNT; i++)
		init_item(test, i, false);
}

static int pending_cancel(struct wq_case *test)
{
	struct gate_item *head = &test->items[0], *item = &test->items[1];

	begin_case(test, "pending-cancel-reuse");
	if (held_work(test, head) || !submit(item, 0) || submit(item, 0) ||
	    !cancel_work(&item->work) || cancel_work(&item->work) ||
	    !submit(item, 0) || !cancel_work_sync(&item->work) ||
	    atomic_read(&item->runs))
		return fail(test, __LINE__);
	release_item(head);
	__flush_workqueue(test->wq);
	if (atomic_read(&item->runs) || !submit(item, 0))
		return fail(test, __LINE__);
	flush_work(&item->work);
	if (atomic_read(&item->runs) != 1 || flush_work(&item->work) ||
	    cancel_work_sync(&item->work))
		return fail(test, __LINE__);
	return end_case(test);
}

struct cancel_probe {
	struct hrtimer timer;
	struct gate_item *item;
	struct task_struct *caller;
	bool fired;
};

static enum hrtimer_restart release_bh_cancel(struct hrtimer *timer)
{
	struct cancel_probe *probe = container_of(timer, struct cancel_probe, timer);
	struct gate_item *item = probe->item;
	unsigned long data = atomic_long_read(&item_work(item)->data);

	if (!in_hardirq() || current != probe->caller ||
	    raw_smp_processor_id() != (item->test->cpu ^ 1) ||
	    (data & WORK_STRUCT_PWQ) || !(data & WORK_OFFQ_DISABLE_MASK) ||
	    atomic_read(&item->active) != 1)
		atomic_or(CONTEXT, &item->test->errors);
	WRITE_ONCE(probe->fired, true);
	release_item(item);
	return HRTIMER_NORESTART;
}

static int cancel_bh_running(struct wq_case *test, struct gate_item *item,
			     bool expected_pending)
{
	struct cancel_probe probe = { .item = item, .caller = current };
	bool pending;

	/* Upstream explicitly permits BH-work cancellation in non-hardirq
	 * atomic context. The caller cannot be displaced by a witness task:
	 * a real hard timer must interrupt the API itself before it can return.
	 */
	preempt_disable();
	hrtimer_setup_on_stack(&probe.timer, release_bh_cancel, CLOCK_MONOTONIC,
			       HRTIMER_MODE_REL_PINNED_HARD);
	hrtimer_start(&probe.timer, 10 * NSEC_PER_MSEC, HRTIMER_MODE_REL_PINNED_HARD);
	pending = item->delayed_mode ? cancel_delayed_work_sync(&item->delayed) :
				       cancel_work_sync(&item->work);
	hrtimer_cancel(&probe.timer);
	destroy_hrtimer_on_stack(&probe.timer);
	preempt_enable();
	if (!probe.fired || pending != expected_pending || atomic_read(&item->runs) != 1)
		return fail(test, __LINE__);
	return end_case(test);
}

static int running_wait(struct wq_case *test, enum waiter_op op, bool self)
{
	struct gate_item *item = &test->items[0];
	bool cancel = op == CANCEL_SYNC || op == CANCEL_DELAYED_SYNC;

	begin_case(test, self ? "cancel-self-requeue" :
		   cancel ? "running-cancel" : "running-flush");
	if (op == FLUSH_DELAYED || op == CANCEL_DELAYED_SYNC)
		init_item(test, 0, true);
	item->requeue = self;
	item->limit = 32;
	if (cancel && is_bh(test)) {
		if (held_work(test, item) || (item->delayed_mode ?
		    cancel_delayed_work(&item->delayed) : cancel_work(&item->work)))
			return fail(test, __LINE__);
		return cancel_bh_running(test, item, false);
	}
	/* A held BH callback can keep kthreadd's CPU non-preemptible. */
	if (prepare_waiter(test, item, op) || held_work(test, item))
		return fail(test, __LINE__);
	if (cancel && (item->delayed_mode ? cancel_delayed_work(&item->delayed) :
					 cancel_work(&item->work)))
		return fail(test, __LINE__);
	if (start_waiter(test) || waiter_inside(test, cancel))
		return fail(test, __LINE__);
	if (atomic_read(&item->active) != 1 || test->waiter.finished)
		return fail(test, __LINE__);
	release_item(item);
	if (join_waiter(test) || test->waiter.result == cancel ||
	    atomic_read(&item->runs) != 1 || work_busy(item_work(item)))
		return fail(test, __LINE__);
	return end_case(test);
}

static int flush_snapshot(struct wq_case *test)
{
	struct gate_item *head = &test->items[0], *late = &test->items[1];

	begin_case(test, "flush-queue-snapshot");
	if (prepare_waiter(test, head, FLUSH_QUEUE) || held_work(test, head) ||
	    start_waiter(test) ||
	    waiter_inside(test, false))
		return fail(test, __LINE__);
	late->held = true;
	if (!submit(late, 0))
		return fail(test, __LINE__);
	release_item(head);
	if (await_flag(&late->started) || join_waiter(test) ||
	    atomic_read(&late->active) != 1)
		return fail(test, __LINE__);
	release_item(late);
	return end_case(test);
}

static int self_requeue(struct wq_case *test, bool delayed)
{
	struct gate_item *item = &test->items[0];
	ktime_t deadline = ktime_get() + GATE_NS;

	begin_case(test, delayed ? "delayed-self-requeue" : "self-requeue");
	init_item(test, 0, delayed);
	item->requeue = true;
	item->limit = 32;
	if (!submit(item, 0))
		return fail(test, __LINE__);
	while (atomic_read(&item->runs) != item->limit) {
		if (ktime_get() >= deadline)
			return fail(test, __LINE__);
		usleep_range(500, 1000);
	}
	return end_case(test);
}

static int drain_requeue(struct wq_case *test)
{
	struct gate_item *item = &test->items[0];

	/* Pinned upstream is_chained_work() recognizes threaded workers only.
	 * BH self-requeue is tested separately, outside drain's no-new-work phase.
	 */
	begin_case(test, is_bh(test) ? "bh-drain-running" : "drain-chained-requeue");
	item->requeue = !is_bh(test);
	item->limit = is_bh(test) ? 1 : 32;
	if (prepare_waiter(test, item, DRAIN_QUEUE) || held_work(test, item) ||
	    start_waiter(test) || waiter_inside(test, false))
		return fail(test, __LINE__);
	release_item(item);
	if (join_waiter(test) || atomic_read(&item->runs) != item->limit)
		return fail(test, __LINE__);
	return end_case(test);
}

static int cancel_running_pending(struct wq_case *test)
{
	struct gate_item *item = &test->items[0];

	begin_case(test, "cancel-running-and-pending");
	item->requeue_before_hold = true;
	if (is_bh(test)) {
		if (held_work(test, item) || !work_pending(&item->work))
			return fail(test, __LINE__);
		return cancel_bh_running(test, item, true);
	}
	if (prepare_waiter(test, item, CANCEL_SYNC) || held_work(test, item) ||
	    !work_pending(&item->work) || start_waiter(test) ||
	    waiter_inside(test, true))
		return fail(test, __LINE__);
	release_item(item);
	if (join_waiter(test) || !test->waiter.result || atomic_read(&item->runs) != 1)
		return fail(test, __LINE__);
	return end_case(test);
}

static int delayed_paths(struct wq_case *test)
{
	struct gate_item *head = &test->items[0], *item = &test->items[1];

	begin_case(test, "delayed-cancel-rearm-expire");
	init_item(test, 1, true);
	if (!submit(item, GATE_WAIT) || submit(item, GATE_WAIT) ||
	    !cancel_delayed_work(&item->delayed) ||
	    cancel_delayed_work_sync(&item->delayed) || !submit(item, GATE_WAIT) ||
	    !cancel_delayed_work_sync(&item->delayed) || atomic_read(&item->runs))
		return fail(test, __LINE__);
	item->not_before = jiffies + 5;
	/* mod of idle work queues it and returns false; pending mod returns true. */
	if (mod_delayed_work_on(test->cpu, test->wq, &item->delayed, GATE_WAIT) ||
	    !mod_delayed_work_on(test->cpu, test->wq, &item->delayed, 5) ||
	    await_flag(&item->started))
		return fail(test, __LINE__);
	flush_delayed_work(&item->delayed);
	if (atomic_read(&item->runs) != 1 || end_case(test))
		return fail(test, __LINE__);

	begin_case(test, "delayed-rearm-later");
	init_item(test, 1, true);
	if (!submit(item, 20))
		return fail(test, __LINE__);
	item->not_before = jiffies + 40;
	if (!mod_delayed_work_on(test->cpu, test->wq, &item->delayed, 40) ||
	    await_flag(&item->started))
		return fail(test, __LINE__);
	flush_delayed_work(&item->delayed);
	if (atomic_read(&item->runs) != 1 || end_case(test))
		return fail(test, __LINE__);

	begin_case(test, "delayed-flush-pending-timer");
	init_item(test, 1, true);
	item->held = true;
	if (prepare_waiter(test, item, FLUSH_DELAYED) ||
	    !submit(item, GATE_WAIT) || start_waiter(test) ||
	    await_flag(&item->started) || waiter_inside(test, false) ||
	    timer_pending(&item->delayed.timer))
		return fail(test, __LINE__);
	release_item(item);
	if (join_waiter(test) || !test->waiter.result || end_case(test))
		return fail(test, __LINE__);

	begin_case(test, is_bh(test) ? "delayed-zero-pending-cancel" :
		   "delayed-expired-pending-cancel");
	init_item(test, 1, true);
	if (held_work(test, head) || !submit(item, 1))
		return fail(test, __LINE__);
	/* A held BH callback also holds TIMER_SOFTIRQ. Move its timer to the
	 * work list through the public zero-delay API instead of demanding
	 * impossible nested softirq execution. Threaded queues exercise expiry.
	 */
	if (is_bh(test)) {
		if (!mod_delayed_work_on(test->cpu, test->wq, &item->delayed, 0))
			return fail(test, __LINE__);
	} else {
		msleep(20);
	}
	if (timer_pending(&item->delayed.timer) || atomic_read(&item->runs) ||
	    !cancel_delayed_work_sync(&item->delayed))
		return fail(test, __LINE__);
	release_item(head);
	return end_case(test);
}

static struct wq_case *new_case(struct kobox_linux_workqueue_report *report,
			       enum queue_kind kind, unsigned int cpu)
{
	static const unsigned int flags[] = {
		WQ_PERCPU, WQ_PERCPU | WQ_HIGHPRI, WQ_BH,
		WQ_BH | WQ_HIGHPRI, WQ_UNBOUND, WQ_UNBOUND,
	};
	struct wq_case *test = kzalloc(sizeof(*test), GFP_KERNEL);
	struct workqueue_attrs *attrs;

	if (!test)
		return NULL;
	test->report = report;
	test->kind = kind;
	test->cpu = cpu;
	strscpy(report->queue, queue_names[kind], sizeof(report->queue));
	report->cpu = cpu;
	test->wq = kind == ORDERED ? alloc_ordered_workqueue("wq-gate", 0) :
		alloc_workqueue("wq-gate", flags[kind], is_bh(test) ? 0 : 1);
	if (!test->wq)
		return NULL;
	if (kind == UNBOUND || kind == ORDERED) {
		attrs = alloc_workqueue_attrs();
		if (!attrs)
			return NULL;
		cpumask_copy(attrs->cpumask, cpumask_of(cpu));
		if (apply_workqueue_attrs(test->wq, attrs))
			return NULL;
		free_workqueue_attrs(attrs);
	}
	return test;
}

static void ordered_submit(void *argument)
{
	struct gate_item *item = argument;

	if (!queue_work(item->test->wq, &item->work))
		atomic_or(ORDER, &item->test->errors);
}

static int ordered_fifo(struct kobox_linux_workqueue_report *report)
{
	struct wq_case *test = new_case(report, ORDERED, 0);
	struct workqueue_attrs *attrs = alloc_workqueue_attrs();
	unsigned int i;

	if (!test || !attrs)
		return -ENOMEM;
	if (apply_workqueue_attrs(test->wq, attrs))
		return fail(test, __LINE__);
	free_workqueue_attrs(attrs);
	test->any_cpu = true;
	test->ordered_check = true;
	begin_case(test, "ordered-global-fifo");
	if (held_work(test, &test->items[0]))
		return fail(test, __LINE__);
	for (i = 1; i < ITEM_COUNT; i++)
		if (smp_call_function_single(i & 1, ordered_submit, &test->items[i], 1))
			return fail(test, __LINE__);
	if (atomic_read(&test->sequence) != 1)
		return fail(test, __LINE__);
	release_item(&test->items[0]);
	drain_workqueue(test->wq);
	if (atomic_read(&test->sequence) != ITEM_COUNT || end_case(test))
		return fail(test, __LINE__);
	destroy_workqueue(test->wq);
	kfree(test);
	return 0;
}

static int unbound_attributes(struct kobox_linux_workqueue_report *report)
{
	struct wq_case *test = new_case(report, UNBOUND, 0);
	struct workqueue_attrs *attrs = alloc_workqueue_attrs();
	struct gate_item *a, *b;

	if (!test || !attrs)
		return -ENOMEM;
	a = &test->items[0];
	b = &test->items[1];
	workqueue_set_max_active(test->wq, 4);
	attrs->affn_scope = WQ_AFFN_SYSTEM;
	if (apply_workqueue_attrs(test->wq, attrs))
		return fail(test, __LINE__);
	test->any_cpu = true;
	begin_case(test, "unbound-parallel-workers");
	if (held_work(test, a) || held_work(test, b) ||
	    atomic_read(&test->active) != 2 || a->runner == b->runner)
		return fail(test, __LINE__);
	release_item(a);
	release_item(b);
	if (end_case(test))
		return fail(test, __LINE__);

	begin_case(test, "unbound-live-affinity-change");
	cpumask_copy(attrs->cpumask, cpumask_of(0));
	if (apply_workqueue_attrs(test->wq, attrs) || held_work(test, a) ||
	    a->seen_cpu != 0)
		return fail(test, __LINE__);
	cpumask_copy(attrs->cpumask, cpumask_of(1));
	if (apply_workqueue_attrs(test->wq, attrs) || held_work(test, b) ||
	    b->seen_cpu != 1 || a->runner == b->runner)
		return fail(test, __LINE__);
	release_item(a);
	release_item(b);
	free_workqueue_attrs(attrs);
	if (end_case(test))
		return fail(test, __LINE__);
	destroy_workqueue(test->wq);
	kfree(test);
	return 0;
}

static void system_submit_batch(void *argument)
{
	struct wq_case *test = argument;
	unsigned int i;

	for (i = 0; i < ITEM_COUNT; i++)
		if (!submit(&test->items[i], 0))
			atomic_or(ORDER, &test->errors);
}

static int system_queues(struct kobox_linux_workqueue_report *report)
{
	struct workqueue_struct *queues[] = {
		system_wq, system_highpri_wq, system_bh_wq,
		system_bh_highpri_wq, system_unbound_wq, system_dfl_wq,
	};
	unsigned int cpu, q, i;
	struct wq_case *test;

	for_each_online_cpu(cpu) {
		for (q = 0; q < ARRAY_SIZE(queues); q++) {
			if (!queues[q])
				return -EINVAL;
			test = kzalloc(sizeof(*test), GFP_KERNEL);
			if (!test)
				return -ENOMEM;
			test->report = report;
			test->kind = min_t(unsigned int, q, UNBOUND);
			test->cpu = cpu;
			test->any_cpu = test->kind == UNBOUND;
			test->ordered_check = is_bh(test);
			test->wq = queues[q];
			strscpy(report->queue, queue_names[test->kind], sizeof(report->queue));
			report->cpu = cpu;
			begin_case(test, "system-queue-dispatch");
			if (smp_call_function_single(cpu, system_submit_batch, test, 1))
				return fail(test, __LINE__);
			/* Never flush a shared system queue: wait only for our items. */
			for (i = 0; i < ITEM_COUNT; i++) {
				flush_work(&test->items[i].work);
				if (atomic_read(&test->items[i].runs) != 1)
					return fail(test, __LINE__);
			}
			if ((is_bh(test) && atomic_read(&test->sequence) != ITEM_COUNT) ||
			    verify_items(test))
				return fail(test, __LINE__);
			kfree(test);
		}
	}
	return 0;
}

struct priority_case {
	struct work_struct normal;
	struct work_struct high;
	unsigned int cpu;
	atomic_t sequence;
	atomic_t errors;
};

static void priority_observe(struct priority_case *test, unsigned int expected)
{
	if (!in_serving_softirq() || in_hardirq() || irqs_disabled() ||
	    raw_smp_processor_id() != test->cpu ||
	    atomic_fetch_inc(&test->sequence) != expected)
		atomic_inc(&test->errors);
}

static void normal_bh_callback(struct work_struct *work)
{
	priority_observe(container_of(work, struct priority_case, normal), 1);
}

static void high_bh_callback(struct work_struct *work)
{
	priority_observe(container_of(work, struct priority_case, high), 0);
}

static void priority_submit(void *argument)
{
	struct priority_case *test = argument;

	/* Both become pending before IRQ enable; submit low priority first. */
	if (!irqs_disabled() ||
	    !queue_work_on(test->cpu, system_bh_wq, &test->normal) ||
	    !queue_work_on(test->cpu, system_bh_highpri_wq, &test->high))
		atomic_inc(&test->errors);
}

static int bh_priority(struct kobox_linux_workqueue_report *report)
{
	struct priority_case *test;
	cpumask_t saved;
	unsigned int cpu;

	cpumask_copy(&saved, current->cpus_ptr);
	strscpy(report->queue, "bh-highpri", sizeof(report->queue));
	strscpy(report->scenario, "bh-priority-dispatch", sizeof(report->scenario));
	for_each_online_cpu(cpu) {
		test = kzalloc(sizeof(*test), GFP_KERNEL);
		if (!test)
			return -ENOMEM;
		test->cpu = cpu;
		report->cpu = cpu;
		INIT_WORK(&test->normal, normal_bh_callback);
		INIT_WORK(&test->high, high_bh_callback);
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
			return -EINVAL;
		/* Priority applies at dispatch, not to an already executing BH.
		 * Submit in task context with BH disabled to establish that boundary.
		 */
		local_bh_disable();
		local_irq_disable();
		priority_submit(test);
		local_irq_enable();
		local_bh_enable();
		flush_work(&test->normal);
		flush_work(&test->high);
		if (atomic_read(&test->errors) || atomic_read(&test->sequence) != 2) {
			report->line = __LINE__;
			report->errors = atomic_read(&test->errors);
			return -EINVAL;
		}
		report->cases++;
		report->callbacks += 2;
		kfree(test);
	}
	return set_cpus_allowed_ptr(current, &saved);
}

struct pressure_case;

struct pressure_work {
	struct work_struct work;
	struct pressure_case *test;
	struct list_head pages;
	unsigned int nr_pages;
};

struct pressure_case {
	struct workqueue_struct *wq;
	struct pressure_work works[ITEM_COUNT];
	struct completion released;
	struct list_head pages;
	unsigned int nr_pages;
	unsigned int cpu;
	atomic_t rescued;
	atomic_t callbacks;
	atomic_t errors;
};

static void free_pressure_pages(struct list_head *pages)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, pages, lru) {
		list_del(&page->lru);
		__free_page(page);
	}
}

static unsigned int fill_pressure_pages(struct list_head *pages)
{
	unsigned int count = 0;
	struct page *page;

	while ((page = alloc_page(GFP_NOWAIT | __GFP_NOWARN))) {
		list_add(&page->lru, pages);
		count++;
	}
	return count;
}

static void pressure_callback(struct work_struct *work)
{
	struct pressure_work *item = container_of(work, struct pressure_work, work);
	struct pressure_case *test = item->test;
	unsigned int i;

	/* All members of this attributes pool and its rescuer are pinned to
	 * one CPU. Non-sleeping page operations retain that execution domain,
	 * so the rescuer cannot free a list while a worker is filling it.
	 */
	preempt_disable();
	if (current_work() != work || in_interrupt() ||
	    raw_smp_processor_id() != test->cpu) {
		atomic_or(CONTEXT, &test->errors);
		preempt_enable();
		return;
	}
	if (current_is_workqueue_rescuer()) {
		if (atomic_inc_return(&test->rescued) == 1) {
			/* No regular worker or controller frees the held RAM. */
			free_pressure_pages(&test->pages);
			for (i = 0; i < ITEM_COUNT; i++)
				free_pressure_pages(&test->works[i].pages);
			complete_all(&test->released);
		}
	} else if (!completion_done(&test->released)) {
		/* Prior tests' deferred cleanup may return more pages after the
		 * first exhaustion. Keep the dependency on rescue real until it runs.
		 */
		item->nr_pages = fill_pressure_pages(&item->pages);
	}
	preempt_enable();
	if (!wait_for_completion_timeout(&test->released, GATE_WAIT))
		atomic_or(TIMEOUT, &test->errors);
	atomic_inc(&test->callbacks);
}

static int rescuer_pressure(struct kobox_linux_workqueue_report *report,
			    unsigned int cpu)
{
	struct pressure_case *test;
	struct workqueue_attrs *attrs;
	struct page *page;
	unsigned int i;
	bool completed;

	strscpy(report->queue, "mem-reclaim", sizeof(report->queue));
	strscpy(report->scenario, "real-ram-pressure-rescue", sizeof(report->scenario));
	report->cpu = cpu;
	test = kzalloc(sizeof(*test), GFP_KERNEL);
	attrs = alloc_workqueue_attrs();
	if (!test || !attrs)
		return -ENOMEM;
	INIT_LIST_HEAD(&test->pages);
	test->cpu = cpu;
	init_completion(&test->released);
	test->wq = alloc_workqueue("wq-gate-rescue", WQ_UNBOUND | WQ_MEM_RECLAIM,
				   ITEM_COUNT);
	if (!test->wq)
		return -ENOMEM;
	/* A private attributes pool avoids borrowing existing system workers. */
	attrs->nice = 7;
	cpumask_copy(attrs->cpumask, cpumask_of(cpu));
	if (apply_workqueue_attrs(test->wq, attrs))
		return -EINVAL;
	free_workqueue_attrs(attrs);
	for (i = 0; i < ITEM_COUNT; i++) {
		test->works[i].test = test;
		INIT_LIST_HEAD(&test->works[i].pages);
		INIT_WORK(&test->works[i].work, pressure_callback);
	}
	/*
	 * Deliberately exhaust real Linux RAM, not host memory or a private
	 * allocator. Serializing against the OOM killer keeps this bounded
	 * reclaim test from killing PID 1. Allocator/worker/mayday logic stays
	 * upstream; allocation failure and retry are not injected or replaced.
	 * No reclaiming allocation is made by this task while it owns oom_lock.
	 */
	mutex_lock(&oom_lock);
	test->nr_pages = fill_pressure_pages(&test->pages);
	if (test->nr_pages < 4096) {
		free_pressure_pages(&test->pages);
		mutex_unlock(&oom_lock);
		return -EINVAL;
	}
	report->pressure_pages += test->nr_pages;
	for (i = 0; i < ITEM_COUNT; i++) {
		if (!queue_work_on(cpu, test->wq, &test->works[i].work))
			atomic_or(ORDER, &test->errors);
	}
	completed = wait_for_completion_timeout(&test->released, GATE_WAIT);
	/* Failure still owns its storage; the launcher immediately exits. */
	if (!completed) {
		report->line = __LINE__;
		return -ETIMEDOUT;
	}
	mutex_unlock(&oom_lock);
	__flush_workqueue(test->wq);
	for (i = 0; i < ITEM_COUNT; i++) {
		report->pressure_pages += test->works[i].nr_pages;
		if (!list_empty(&test->works[i].pages))
			atomic_or(ORDER, &test->errors);
	}
	if (!atomic_read(&test->rescued) || atomic_read(&test->errors) ||
	    atomic_read(&test->callbacks) != ITEM_COUNT || !list_empty(&test->pages)) {
		report->line = __LINE__;
		report->errors = atomic_read(&test->errors);
		return -EINVAL;
	}
	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	__free_page(page);
	report->rescued += atomic_read(&test->rescued);
	report->callbacks += atomic_read(&test->callbacks);
	destroy_workqueue(test->wq);
	kfree(test);
	report->cases++;
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_workqueue_verify(struct kobox_linux_workqueue_report *report)
{
	cpumask_t saved;
	struct wq_case *test;
	unsigned int cpu;
	enum queue_kind kind;
	int status;

	if (!report || report->size != sizeof(*report) ||
	    system_state != SYSTEM_RUNNING || num_online_cpus() != 2 ||
	    kobox_linux_exception_warnings())
		return -EINVAL;
	cpumask_copy(&saved, current->cpus_ptr);
	report->phase = 1;
	for_each_online_cpu(cpu) {
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu ^ 1)))
			return -EINVAL;
		for (kind = NORMAL; kind < KINDS; kind++) {
			test = new_case(report, kind, cpu);
			if (!test)
				return -ENOMEM;
			status = pending_cancel(test) ?: running_wait(test, FLUSH_ONE, false) ?:
				running_wait(test, CANCEL_SYNC, false) ?:
				running_wait(test, CANCEL_SYNC, true) ?: flush_snapshot(test) ?:
				self_requeue(test, false) ?: self_requeue(test, true) ?:
				drain_requeue(test) ?: cancel_running_pending(test) ?:
				delayed_paths(test) ?: running_wait(test, FLUSH_DELAYED, false) ?:
				running_wait(test, CANCEL_DELAYED_SYNC, false) ?:
				running_wait(test, CANCEL_DELAYED_SYNC, true);
			if (status)
				return status;
			destroy_workqueue(test->wq);
			kfree(test);
		}
	}
	report->phase = 2;
	status = ordered_fifo(report) ?: unbound_attributes(report) ?:
		system_queues(report) ?: bh_priority(report);
	if (status)
		return status;
	report->phase = 3;
	for_each_online_cpu(cpu) {
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu ^ 1)))
			return -EINVAL;
		status = rescuer_pressure(report, cpu);
		if (status)
			return status;
	}
	report->warnings = kobox_linux_exception_warnings();
	if (report->warnings)
		return -EINVAL;
	report->phase = 4;
	return set_cpus_allowed_ptr(current, &saved);
}
