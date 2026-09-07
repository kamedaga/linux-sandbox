// SPDX-License-Identifier: GPL-2.0-only

#include "wait_gate.h"
#include "host.h"

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/wait.h>

#define WATCHDOG (5 * HZ)
#define SHORT_MS 80U
#define EARLY_MS 1000U

enum wait_api {
	SCHED_TIMEOUT,
	SCHED_UNINTERRUPTIBLE,
	SCHED_INTERRUPTIBLE,
	SCHED_KILLABLE,
	SCHED_IDLE_TIMEOUT,
	SCHED_IO,
	WAIT_TIMEOUT,
	WAIT_INTERRUPTIBLE,
	WAIT_KILLABLE,
	COMP_TIMEOUT,
	COMP_INTERRUPTIBLE,
	COMP_KILLABLE,
	COMP_IO,
	HR_RELATIVE,
	HR_ABSOLUTE,
	HR_INTERRUPTIBLE,
	HR_RANGE_CLOCK,
	WAIT_HR,
	WAIT_HR_INTERRUPTIBLE,
	SLEEP_MS,
	SLEEP_MS_INTERRUPTIBLE,
	SLEEP_US,
	API_COUNT,
};

enum wait_scenario {
	EXPIRE,
	EARLY_WAKE,
	READY_BEFORE_WAIT,
	WAKE_BEFORE_SCHEDULE,
	ZERO_TIMEOUT,
	READY_ZERO_TIMEOUT,
	SIGNAL_ASLEEP,
	SIGNAL_PENDING,
	NONFATAL_SIGNAL,
	IRQ_ONLY,
	SPURIOUS_WAKE,
	REPEATED_WAKE,
	INFINITE_WAKE,
};

enum wait_property {
	QUEUE = BIT(0),
	COMPLETION = BIT(1),
	HIGH_RES = BIT(2),
	INTERRUPTIBLE = BIT(3),
	KILLABLE = BIT(4),
	SLEEP_LOOP = BIT(5),
	MILLISECONDS = BIT(6),
};

struct api_description {
	const char *name;
	unsigned int state;
	unsigned int properties;
};

static const struct api_description apis[API_COUNT] = {
	[SCHED_TIMEOUT] = { "schedule_timeout", TASK_UNINTERRUPTIBLE, 0 },
	[SCHED_UNINTERRUPTIBLE] = { "schedule_timeout_uninterruptible",
		TASK_UNINTERRUPTIBLE, 0 },
	[SCHED_INTERRUPTIBLE] = { "schedule_timeout_interruptible",
		TASK_INTERRUPTIBLE, INTERRUPTIBLE },
	[SCHED_KILLABLE] = { "schedule_timeout_killable",
		TASK_KILLABLE, KILLABLE },
	[SCHED_IDLE_TIMEOUT] = { "schedule_timeout_idle", TASK_IDLE, 0 },
	[SCHED_IO] = { "io_schedule_timeout", TASK_UNINTERRUPTIBLE, 0 },
	[WAIT_TIMEOUT] = { "wait_event_timeout", TASK_UNINTERRUPTIBLE, QUEUE },
	[WAIT_INTERRUPTIBLE] = { "wait_event_interruptible_timeout",
		TASK_INTERRUPTIBLE, QUEUE | INTERRUPTIBLE },
	[WAIT_KILLABLE] = { "wait_event_killable_timeout",
		TASK_KILLABLE, QUEUE | KILLABLE },
	[COMP_TIMEOUT] = { "wait_for_completion_timeout",
		TASK_UNINTERRUPTIBLE, COMPLETION },
	[COMP_INTERRUPTIBLE] = { "wait_for_completion_interruptible_timeout",
		TASK_INTERRUPTIBLE, COMPLETION | INTERRUPTIBLE },
	[COMP_KILLABLE] = { "wait_for_completion_killable_timeout",
		TASK_KILLABLE, COMPLETION | KILLABLE },
	[COMP_IO] = { "wait_for_completion_io_timeout",
		TASK_UNINTERRUPTIBLE, COMPLETION },
	[HR_RELATIVE] = { "schedule_hrtimeout/relative",
		TASK_UNINTERRUPTIBLE, HIGH_RES },
	[HR_ABSOLUTE] = { "schedule_hrtimeout/absolute",
		TASK_UNINTERRUPTIBLE, HIGH_RES },
	[HR_INTERRUPTIBLE] = { "schedule_hrtimeout/interruptible",
		TASK_INTERRUPTIBLE, HIGH_RES | INTERRUPTIBLE },
	[HR_RANGE_CLOCK] = { "schedule_hrtimeout_range_clock",
		TASK_UNINTERRUPTIBLE, HIGH_RES },
	[WAIT_HR] = { "wait_event_hrtimeout",
		TASK_UNINTERRUPTIBLE, QUEUE | HIGH_RES },
	[WAIT_HR_INTERRUPTIBLE] = { "wait_event_interruptible_hrtimeout",
		TASK_INTERRUPTIBLE, QUEUE | HIGH_RES | INTERRUPTIBLE },
	[SLEEP_MS] = { "msleep", TASK_UNINTERRUPTIBLE, SLEEP_LOOP },
	[SLEEP_MS_INTERRUPTIBLE] = { "msleep_interruptible",
		TASK_INTERRUPTIBLE, SLEEP_LOOP | INTERRUPTIBLE | MILLISECONDS },
	[SLEEP_US] = { "usleep_range", TASK_UNINTERRUPTIBLE,
		SLEEP_LOOP | HIGH_RES },
};

static const char * const scenarios[] = {
	[EXPIRE] = "expiry",
	[EARLY_WAKE] = "early-wake",
	[READY_BEFORE_WAIT] = "ready-before-wait",
	[WAKE_BEFORE_SCHEDULE] = "wake-before-schedule",
	[ZERO_TIMEOUT] = "zero-timeout",
	[READY_ZERO_TIMEOUT] = "ready-zero-timeout",
	[SIGNAL_ASLEEP] = "signal-asleep",
	[SIGNAL_PENDING] = "signal-pending",
	[NONFATAL_SIGNAL] = "nonfatal-signal",
	[IRQ_ONLY] = "unrelated-irq",
	[SPURIOUS_WAKE] = "spurious-wake",
	[REPEATED_WAKE] = "repeated-wake-budget",
	[INFINITE_WAKE] = "infinite-wake",
};

struct wait_case {
	struct kobox_linux_wait_report *report;
	struct task_struct *task;
	wait_queue_head_t queue;
	struct completion event;
	struct completion finished;
	struct completion irq_done;
	struct hrtimer irq_timer;
	enum wait_api api;
	enum wait_scenario scenario;
	unsigned int cpu;
	unsigned int milliseconds;
	unsigned long timeout;
	unsigned long start;
	unsigned long asleep;
	unsigned long wake;
	unsigned long end;
	ktime_t start_ns;
	ktime_t end_ns;
	ktime_t irq_deadline;
	long result;
	unsigned int invalid;
	bool condition;
	bool begun;
	bool armed;
	bool released;
	bool returned;
	bool pending_signal;
};

static unsigned int properties(const struct wait_case *test)
{
	return apis[test->api].properties;
}

static bool raw_wait(const struct wait_case *test)
{
	return !(properties(test) & (QUEUE | COMPLETION | SLEEP_LOOP));
}

static long invoke_wait(struct wait_case *test)
{
	ktime_t duration = (ktime_t)test->milliseconds * NSEC_PER_MSEC;
	ktime_t expires = test->api == HR_ABSOLUTE && duration ?
		ktime_add(ktime_get(), duration) : duration;
	ktime_t *limit = test->scenario == INFINITE_WAKE ? NULL : &expires;
	unsigned long timeout = test->timeout;

	switch (test->api) {
	case SCHED_TIMEOUT:
		return schedule_timeout(timeout);
	case SCHED_UNINTERRUPTIBLE:
		return schedule_timeout_uninterruptible(timeout);
	case SCHED_INTERRUPTIBLE:
		return schedule_timeout_interruptible(timeout);
	case SCHED_KILLABLE:
		return schedule_timeout_killable(timeout);
	case SCHED_IDLE_TIMEOUT:
		return schedule_timeout_idle(timeout);
	case SCHED_IO:
		return io_schedule_timeout(timeout);
	case WAIT_TIMEOUT:
		return wait_event_timeout(test->queue,
			READ_ONCE(test->condition), timeout);
	case WAIT_INTERRUPTIBLE:
		return wait_event_interruptible_timeout(test->queue,
			READ_ONCE(test->condition), timeout);
	case WAIT_KILLABLE:
		return wait_event_killable_timeout(test->queue,
			READ_ONCE(test->condition), timeout);
	case COMP_TIMEOUT:
		return wait_for_completion_timeout(&test->event, timeout);
	case COMP_INTERRUPTIBLE:
		return wait_for_completion_interruptible_timeout(&test->event,
							 timeout);
	case COMP_KILLABLE:
		return wait_for_completion_killable_timeout(&test->event, timeout);
	case COMP_IO:
		return wait_for_completion_io_timeout(&test->event, timeout);
	case HR_RELATIVE:
	case HR_INTERRUPTIBLE:
		return schedule_hrtimeout(limit, HRTIMER_MODE_REL);
	case HR_ABSOLUTE:
		return schedule_hrtimeout(limit, HRTIMER_MODE_ABS);
	case HR_RANGE_CLOCK:
		return schedule_hrtimeout_range_clock(limit, NSEC_PER_MSEC,
				HRTIMER_MODE_REL, CLOCK_MONOTONIC);
	case WAIT_HR:
		return wait_event_hrtimeout(test->queue,
			READ_ONCE(test->condition), duration);
	case WAIT_HR_INTERRUPTIBLE:
		return wait_event_interruptible_hrtimeout(test->queue,
			READ_ONCE(test->condition), duration);
	case SLEEP_MS:
		msleep(test->milliseconds);
		return 0;
	case SLEEP_MS_INTERRUPTIBLE:
		return msleep_interruptible(test->milliseconds);
	case SLEEP_US:
		usleep_range(test->milliseconds * 1000,
			     test->milliseconds * 1000 + 1000);
		return 0;
	default:
		return -EINVAL;
	}
}

static int waiter(void *argument)
{
	struct wait_case *test = argument;
	ktime_t deadline;

	allow_signal(SIGUSR1);
	/* kthreadd's ignored dispositions include SIGKILL. Opt in explicitly. */
	allow_signal(SIGKILL);
	if (test->scenario == SIGNAL_PENDING)
		test->invalid = send_sig(properties(test) & KILLABLE ?
					 SIGKILL : SIGUSR1, current, 1) != 0;
	if (test->scenario == WAKE_BEFORE_SCHEDULE) {
		/*
		 * No upstream hook: hold this task before its public schedule call.
		 * The other CPU must perform a real TTWU before it can continue.
		 * armed/released publish the state transition and wake acknowledgement.
		 */
		preempt_disable();
		set_current_state(apis[test->api].state);
		deadline = ktime_get() + NSEC_PER_SEC;
		/* Publish the sleep state; acquire the remote wake acknowledgement. */
		smp_store_release(&test->armed, true);
		/* Paired with the controller's release after wake_up_process(). */
		while (!smp_load_acquire(&test->released) &&
		       ktime_get() < deadline)
			cpu_relax();
		if (!test->released || READ_ONCE(current->__state) != TASK_RUNNING)
			test->invalid = true;
		__set_current_state(TASK_RUNNING);
		preempt_enable();
	} else if (raw_wait(test)) {
		set_current_state(apis[test->api].state);
	}
	test->start = jiffies;
	test->start_ns = ktime_get();
	/* Distinguish the tested wait from any sleep in upstream kthread startup. */
	smp_store_release(&test->begun, true);
	test->result = invoke_wait(test);
	test->end_ns = ktime_get();
	test->end = jiffies;
	test->pending_signal = signal_pending(current);
	test->invalid |= READ_ONCE(current->__state) != TASK_RUNNING ||
		raw_smp_processor_id() != test->cpu ||
		current != raw_cpu_read(current_task) || current->in_iowait;
	flush_signals(current);
	/* Publish result and state checks before the controller observes return. */
	smp_store_release(&test->returned, true);
	complete(&test->finished);
	/* Keep the task reference alive until the controller joins it. */
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static enum hrtimer_restart unrelated_irq(struct hrtimer *timer)
{
	struct wait_case *test = container_of(timer, struct wait_case, irq_timer);
	unsigned int errors = 0;

	if (!in_hardirq())
		errors |= BIT(0);
	if (raw_smp_processor_id() != test->cpu)
		errors |= BIT(1);
	if (current != raw_cpu_read(current_task))
		errors |= BIT(2);
	if (current == test->task)
		errors |= BIT(3);
	if (!hrtimer_is_hres_active(timer) || ktime_get() < test->irq_deadline)
		errors |= BIT(4);
	if (errors)
		WRITE_ONCE(test->invalid, errors);
	test->report->irq_callbacks++;
	complete(&test->irq_done);
	return HRTIMER_NORESTART;
}

static void arm_unrelated_irq(void *argument)
{
	struct wait_case *test = argument;

	/* Remote irq_work may be flushed by idle in task context. A pinned hard
	 * timer exercises actual clockevent IRQ entry regardless of idle polling.
	 */
	hrtimer_setup(&test->irq_timer, unrelated_irq, CLOCK_MONOTONIC,
		      HRTIMER_MODE_ABS_PINNED_HARD);
	test->irq_deadline = ktime_get() + NSEC_PER_MSEC;
	hrtimer_start(&test->irq_timer, test->irq_deadline,
		      HRTIMER_MODE_ABS_PINNED_HARD);
}

static int fail(struct wait_case *test, unsigned int line)
{
	test->report->failure_line = line;
	test->report->invalid = READ_ONCE(test->invalid);
	test->report->result = READ_ONCE(test->result);
	test->report->elapsed_ns = ktime_get() - READ_ONCE(test->start_ns);
	test->report->warnings = kobox_linux_exception_warnings();
	return -EINVAL;
}

static unsigned long await_sleep(struct wait_case *test)
{
	unsigned long deadline = jiffies + WATCHDOG;
	unsigned long switches;

	do {
		/* Acquire the tested call's start, not kthread's initial parked state. */
		if (!smp_load_acquire(&test->begun)) {
			cond_resched();
			continue;
		}
		/* Pairs with waiter's publication before its final stop/join wait. */
		if (smp_load_acquire(&test->returned))
			return 0;
		switches = wait_task_inactive(test->task, apis[test->api].state);
		if (switches) {
			test->asleep = jiffies;
			return switches;
		}
		cond_resched();
	} while (time_before(jiffies, deadline));
	return 0;
}

static void deliver_event(struct wait_case *test)
{
	test->wake = jiffies;
	if (properties(test) & QUEUE) {
		WRITE_ONCE(test->condition, true);
		wake_up_all(&test->queue);
	} else if (properties(test) & COMPLETION) {
		complete(&test->event);
	} else {
		wake_up_process(test->task);
	}
}

static int inject_noise(struct wait_case *test, unsigned long switches)
{
	unsigned long after;
	unsigned int i;

	for (i = 0; i < 3; i++) {
		reinit_completion(&test->irq_done);
		if (smp_call_function_single(test->cpu, arm_unrelated_irq, test, 1) ||
		    !wait_for_completion_timeout(&test->irq_done, WATCHDOG))
			return fail(test, __LINE__);
		hrtimer_cancel(&test->irq_timer);
		after = await_sleep(test);
		test->report->expected_switches = switches & LONG_MAX;
		test->report->observed_switches = after & LONG_MAX;
		if (after != switches || READ_ONCE(test->invalid))
			return fail(test, __LINE__);
	}
	return 0;
}

static int stimulate(struct wait_case *test)
{
	unsigned long switches;
	unsigned long deadline;
	unsigned long first_asleep;
	unsigned int i;
	int signal;

	if (test->scenario == WAKE_BEFORE_SCHEDULE) {
		deadline = jiffies + WATCHDOG;
		/* Acquire the task state published before the pre-schedule pause. */
		while (!smp_load_acquire(&test->armed)) {
			if (time_after_eq(jiffies, deadline))
				return fail(test, __LINE__);
			cond_resched();
		}
		if (wake_up_process(test->task) != 1)
			return fail(test, __LINE__);
		/* Allow the waiter to continue only after successful upstream TTWU. */
		smp_store_release(&test->released, true);
		return 0;
	}
	if (test->scenario != EARLY_WAKE && test->scenario != SIGNAL_ASLEEP &&
	    test->scenario != NONFATAL_SIGNAL && test->scenario != IRQ_ONLY &&
	    test->scenario != SPURIOUS_WAKE && test->scenario != REPEATED_WAKE &&
	    test->scenario != INFINITE_WAKE)
		return 0;
	switches = await_sleep(test);
	if (!switches)
		return fail(test, __LINE__);
	if (test->scenario == IRQ_ONLY)
		return inject_noise(test, switches);
	/*
	 * Advance real jiffies before waking, so an unchanged timeout cannot pass.
	 * Keep the initial asleep bound for the remaining-time oracle.
	 */
	msleep(20);
	if (test->scenario == REPEATED_WAKE) {
		first_asleep = test->asleep;
		for (i = 0; i < 3; i++) {
			if (!wake_up_process(test->task) || !await_sleep(test) ||
			    READ_ONCE(test->task->nvcsw) == (switches & LONG_MAX))
				return fail(test, __LINE__);
			switches = READ_ONCE(test->task->nvcsw) | LONG_MIN;
			msleep(20);
		}
		test->asleep = first_asleep;
		if (test->api != SLEEP_MS_INTERRUPTIBLE) {
			deliver_event(test);
			return 0;
		}
	}
	if (test->scenario == EARLY_WAKE || test->scenario == INFINITE_WAKE) {
		deliver_event(test);
		return 0;
	}
	if (test->scenario == SPURIOUS_WAKE) {
		if (!wake_up_process(test->task) || !await_sleep(test) ||
		    READ_ONCE(test->task->nvcsw) == (switches & LONG_MAX))
			return fail(test, __LINE__);
		return 0;
	}
	signal = properties(test) & KILLABLE &&
		test->scenario == SIGNAL_ASLEEP ? SIGKILL : SIGUSR1;
	test->wake = jiffies;
	if (send_sig(signal, test->task, 1))
		return fail(test, __LINE__);
	return 0;
}

static bool returns_remaining(const struct wait_case *test)
{
	return !(properties(test) & HIGH_RES) &&
		(!(properties(test) & SLEEP_LOOP) ||
		 properties(test) & MILLISECONDS);
}

static bool expires_normally(const struct wait_case *test)
{
	return test->scenario == EXPIRE || test->scenario == IRQ_ONLY ||
		test->scenario == SPURIOUS_WAKE ||
		test->scenario == NONFATAL_SIGNAL ||
		(test->scenario == SIGNAL_ASLEEP &&
		 !(properties(test) & (INTERRUPTIBLE | KILLABLE)));
}

static int check_result(struct wait_case *test)
{
	unsigned int flags = properties(test);
	long expected = 0;
	long low, high;

	test->report->result = test->result;
	test->report->elapsed_ns = test->end_ns - test->start_ns;
	if (test->invalid || test->end_ns < test->start_ns)
		return fail(test, __LINE__);
	if (expires_normally(test) || test->scenario == ZERO_TIMEOUT) {
		if (flags & HIGH_RES && flags & QUEUE)
			expected = -ETIME;
		if (test->result != expected)
			return fail(test, __LINE__);
		if (flags & HIGH_RES) {
			if (test->end_ns - test->start_ns <
			    (ktime_t)test->milliseconds * NSEC_PER_MSEC)
				return fail(test, __LINE__);
		} else if (test->end - test->start < test->timeout) {
			return fail(test, __LINE__);
		}
		if ((test->scenario == SIGNAL_ASLEEP ||
		     test->scenario == NONFATAL_SIGNAL) && !test->pending_signal)
			return fail(test, __LINE__);
		return 0;
	}
	if (test->scenario == SIGNAL_ASLEEP || test->scenario == SIGNAL_PENDING ||
	    (test->scenario == REPEATED_WAKE && flags & MILLISECONDS)) {
		if (!test->pending_signal)
			return fail(test, __LINE__);
		if (flags & (QUEUE | COMPLETION))
			expected = -ERESTARTSYS;
		else if (flags & HIGH_RES)
			expected = -EINTR;
		if (expected)
			return test->result == expected ? 0 : fail(test, __LINE__);
	}
	if (test->scenario == READY_BEFORE_WAIT ||
	    test->scenario == READY_ZERO_TIMEOUT) {
		expected = flags & HIGH_RES ? 0 : max(test->timeout, 1UL);
		return test->result == expected ? 0 : fail(test, __LINE__);
	}
	if (test->scenario == INFINITE_WAKE) {
		expected = flags & HIGH_RES ? -EINTR : MAX_SCHEDULE_TIMEOUT;
		return test->result == expected ? 0 : fail(test, __LINE__);
	}
	if (flags & HIGH_RES) {
		expected = flags & QUEUE ? 0 : -EINTR;
		return test->result == expected ? 0 : fail(test, __LINE__);
	}
	if (!returns_remaining(test) || test->result <= 0)
		return fail(test, __LINE__);
	/*
	 * Bound the internal jiffy samples by observations, not wall-clock slack:
	 * start <= internal start <= inactive; wake <= return sample <= end.
	 */
	low = max_t(long, 0, test->timeout - (test->end - test->start));
	high = test->timeout;
	if (test->scenario == EARLY_WAKE || test->scenario == SIGNAL_ASLEEP ||
	    test->scenario == REPEATED_WAKE)
		high -= test->wake - test->asleep;
	if (flags & (QUEUE | COMPLETION)) {
		low = max(low, 1L);
		high = max(high, 1L);
	}
	if (flags & MILLISECONDS) {
		low = jiffies_to_msecs(low);
		high = jiffies_to_msecs(high);
	}
	return test->result >= low && test->result <= high ?
		0 : fail(test, __LINE__);
}

static int run_case(struct kobox_linux_wait_report *report, enum wait_api api,
		    enum wait_scenario scenario, unsigned int cpu)
{
	struct wait_case *test;
	int status;

	strscpy(report->api, apis[api].name, sizeof(report->api));
	strscpy(report->scenario, scenarios[scenario], sizeof(report->scenario));
	report->cpu = cpu;
	report->expected_switches = 0;
	report->observed_switches = 0;
	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->report = report;
	test->api = api;
	test->scenario = scenario;
	test->cpu = cpu;
	test->milliseconds = SHORT_MS;
	if (scenario == EARLY_WAKE || scenario == SIGNAL_PENDING ||
	    scenario == REPEATED_WAKE ||
	    scenario == WAKE_BEFORE_SCHEDULE || scenario == READY_BEFORE_WAIT ||
	    (scenario == SIGNAL_ASLEEP &&
	     properties(test) & (INTERRUPTIBLE | KILLABLE)))
		test->milliseconds = EARLY_MS;
	if (scenario == ZERO_TIMEOUT || scenario == READY_ZERO_TIMEOUT)
		test->milliseconds = 0;
	test->timeout = msecs_to_jiffies(test->milliseconds);
	if (scenario == INFINITE_WAKE)
		test->timeout = MAX_SCHEDULE_TIMEOUT;
	init_waitqueue_head(&test->queue);
	init_completion(&test->event);
	init_completion(&test->finished);
	init_completion(&test->irq_done);
	if (scenario == READY_BEFORE_WAIT || scenario == READY_ZERO_TIMEOUT) {
		test->condition = true;
		complete(&test->event);
	}
	test->task = kthread_create(waiter, test, "wait-gate/%u", cpu);
	if (IS_ERR(test->task)) {
		status = PTR_ERR(test->task);
		kfree(test);
		return status;
	}
	kthread_bind(test->task, cpu);
	wake_up_process(test->task);
	status = stimulate(test);
	if (status)
		return status;
	if (!wait_for_completion_timeout(&test->finished, WATCHDOG))
		return fail(test, __LINE__);
	status = kthread_stop(test->task);
	if (status)
		return status;
	status = check_result(test);
	if (status)
		return status;
	kfree(test);
	report->passed++;
	return 0;
}

static bool applicable(enum wait_api api, enum wait_scenario scenario)
{
	unsigned int flags = apis[api].properties;

	switch (scenario) {
	case EARLY_WAKE:
		return !(flags & SLEEP_LOOP);
	case READY_BEFORE_WAIT:
	case READY_ZERO_TIMEOUT:
		return flags & (QUEUE | COMPLETION);
	case WAKE_BEFORE_SCHEDULE:
		/* State-setting wrappers intentionally replace TASK_RUNNING. */
		return api == SCHED_TIMEOUT || api == SCHED_IO ||
			api == HR_RELATIVE || api == HR_ABSOLUTE ||
			api == HR_INTERRUPTIBLE || api == HR_RANGE_CLOCK;
	case SIGNAL_PENDING:
		return flags & (INTERRUPTIBLE | KILLABLE);
	case NONFATAL_SIGNAL:
		return flags & KILLABLE;
	case SPURIOUS_WAKE:
		return flags & (QUEUE | COMPLETION | SLEEP_LOOP);
	case REPEATED_WAKE:
		return flags & (QUEUE | COMPLETION) || api == SLEEP_MS_INTERRUPTIBLE;
	case INFINITE_WAKE:
		return api <= SCHED_IO ||
			(api >= HR_RELATIVE && api <= HR_RANGE_CLOCK);
	default:
		return true;
	}
}

__attribute__((visibility("default")))
int kobox_linux_wait_verify(struct kobox_linux_wait_report *report)
{
	cpumask_t saved_affinity;
	unsigned int cpu;
	enum wait_api api;
	enum wait_scenario scenario;
	int status;

	if (!report || report->size != sizeof(*report) ||
	    system_state != SYSTEM_RUNNING || num_online_cpus() != 2)
		return -EINVAL;
	cpumask_copy(&saved_affinity, current->cpus_ptr);
	for_each_online_cpu(cpu) {
		status = set_cpus_allowed_ptr(current, cpumask_of(cpu ^ 1));
		if (status)
			return status;
		for (api = 0; api < API_COUNT; api++) {
			for (scenario = EXPIRE; scenario <= INFINITE_WAKE; scenario++) {
				if (!applicable(api, scenario))
					continue;
				status = run_case(report, api, scenario, cpu);
				if (status)
					return status;
			}
		}
	}
	status = set_cpus_allowed_ptr(current, &saved_affinity);
	report->warnings = kobox_linux_exception_warnings();
	if (report->warnings)
		return -EINVAL;
	return status;
}
