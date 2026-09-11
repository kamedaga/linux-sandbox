// SPDX-License-Identifier: GPL-2.0-only

#include "time_port.h"
#include "../boot/diagnostic.h"

#include <linux/completion.h>
#include <linux/context_tracking.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/smp.h>
#include <linux/tick.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>

#include "../../kernel/time/tick-sched.h"

#define GATE_TIMEOUT (5 * HZ)
/* Abort this fixture process before any failed test's stack timers can escape. */
#define CHECK(test) do { \
	if (!(test)) { \
		kobox_linux_boot_diagnostic( \
			"kobox time: fail function=%s line=%u cpu=%u check=%s\n", \
			__func__, __LINE__, raw_smp_processor_id(), #test); \
		__builtin_trap(); \
	} \
} while (0)

struct timer_case {
	struct hrtimer high;
	struct timer_list wheel;
	struct completion done;
	ktime_t deadline;
	ktime_t fired_at;
	unsigned long wheel_deadline;
	unsigned int cpu;
	unsigned int fires;
	bool valid;
	bool cancel;
	bool rearm;
};

static bool irq_identity(unsigned int cpu)
{
	return raw_smp_processor_id() == cpu && task_cpu(current) == cpu &&
		current == raw_cpu_read(current_task) && rcu_is_watching_curr_cpu();
}

static enum hrtimer_restart high_callback(struct hrtimer *timer)
{
	struct timer_case *test = container_of(timer, typeof(*test), high);

	test->fired_at = ktime_get();
	test->valid &= in_hardirq() && irq_identity(test->cpu) &&
		test->fired_at >= test->deadline && hrtimer_is_hres_active(timer);
	if (!test->valid)
		kobox_linux_boot_diagnostic(
			"kobox time: high target=%u cpu=%u task_cpu=%u hardirq=%u identity=%u fired=%lld deadline=%lld hres=%u\n",
			test->cpu, raw_smp_processor_id(), task_cpu(current),
			!!in_hardirq(), irq_identity(test->cpu), test->fired_at,
			test->deadline, hrtimer_is_hres_active(timer));
	test->fires++;
	complete(&test->done);
	return HRTIMER_NORESTART;
}

static void wheel_callback(struct timer_list *timer)
{
	struct timer_case *test = timer_container_of(test, timer, wheel);

	test->valid &= in_serving_softirq() && irq_identity(test->cpu) &&
		time_after_eq(jiffies, test->wheel_deadline);
	if (!test->valid)
		kobox_linux_boot_diagnostic(
			"kobox time: wheel target=%u cpu=%u softirq=%u identity=%u now=%lu deadline=%lu\n",
			test->cpu, raw_smp_processor_id(),
			!!in_serving_softirq(), irq_identity(test->cpu),
			jiffies, test->wheel_deadline);
	test->fires++;
	complete(&test->done);
}

static void setup_timers(void *argument)
{
	struct timer_case *test = argument;

	hrtimer_setup_on_stack(&test->high, high_callback, CLOCK_MONOTONIC,
				HRTIMER_MODE_ABS_PINNED_HARD);
	timer_setup_on_stack(&test->wheel, wheel_callback, TIMER_PINNED);
	test->wheel_deadline = jiffies + msecs_to_jiffies(20);
	mod_timer(&test->wheel, test->wheel_deadline);
}

static void arm_high(void *argument)
{
	struct timer_case *test = argument;

	/* Below the 4 ms jiffy interval; hres mode is checked in the callback. */
	test->deadline = ktime_get() + 500 * NSEC_PER_USEC;
	hrtimer_start(&test->high, test->deadline, HRTIMER_MODE_ABS_PINNED_HARD);
}

static void delay_delivery(void *argument)
{
	struct timer_case *test = argument;
	unsigned long flags;
	ktime_t old_deadline;

	local_irq_save(flags);
	old_deadline = ktime_get() + NSEC_PER_MSEC;
	test->deadline = old_deadline;
	hrtimer_start(&test->high, old_deadline, HRTIMER_MODE_ABS_PINNED_HARD);
	/* The host interrupt becomes pending while Linux delivery is masked. */
	while (ktime_get() < old_deadline + 2 * NSEC_PER_MSEC)
		cpu_relax();
	test->valid &= test->fires == 0;
	if (!test->valid)
		kobox_linux_boot_diagnostic(
			"kobox time: masked target=%u fires=%u cancel=%u rearm=%u\n",
			test->cpu, test->fires, test->cancel, test->rearm);
	if (test->cancel)
		test->valid &= hrtimer_cancel(&test->high) == 1;
	if (!test->valid)
		kobox_linux_boot_diagnostic(
			"kobox time: cancel target=%u valid=%u cancel=%u rearm=%u\n",
			test->cpu, test->valid, test->cancel, test->rearm);
	if (test->rearm) {
		test->deadline = ktime_get() + 10 * NSEC_PER_MSEC;
		hrtimer_start(&test->high, test->deadline,
			      HRTIMER_MODE_ABS_PINNED_HARD);
	}
	/* Also inject a stale/spurious device notification deterministically. */
	if (kobox_task_host()->cpu_notify(test->cpu, KOBOX_LINUX_TASK_CLOCKEVENT)) {
		kobox_linux_boot_diagnostic(
			"kobox time: stale notification failed target=%u\n",
			test->cpu);
		__builtin_trap();
	}
	local_irq_restore(flags);
}

static int test_timers(unsigned int cpu)
{
	struct timer_case test = {.cpu = cpu, .valid = true};
	unsigned int round;

	init_completion(&test.done);
	CHECK(!smp_call_function_single(cpu, setup_timers, &test, 1));
	CHECK(wait_for_completion_timeout(&test.done, GATE_TIMEOUT));
	CHECK(test.valid && test.fires == 1);
	timer_shutdown_sync(&test.wheel);
	timer_destroy_on_stack(&test.wheel);
	CHECK(hrtimer_is_hres_active(&test.high));
	for (round = 0; round < 8; round++) {
		test.fires = 0;
		reinit_completion(&test.done);
		CHECK(!smp_call_function_single(cpu, arm_high, &test, 1));
		CHECK(wait_for_completion_timeout(&test.done, GATE_TIMEOUT));
		hrtimer_cancel(&test.high);
		CHECK(test.valid && test.fires == 1);
	}

	test.fires = 0;
	reinit_completion(&test.done);
	CHECK(!smp_call_function_single(cpu, delay_delivery, &test, 1));
	CHECK(wait_for_completion_timeout(&test.done, GATE_TIMEOUT));
	hrtimer_cancel(&test.high);
	CHECK(test.valid && test.fires == 1);
	msleep(20);
	CHECK(test.fires == 1);

	test.fires = 0;
	test.cancel = true;
	reinit_completion(&test.done);
	CHECK(!smp_call_function_single(cpu, delay_delivery, &test, 1));
	msleep(20);
	CHECK(test.valid && test.fires == 0 && !completion_done(&test.done));
	test.rearm = true;
	CHECK(!smp_call_function_single(cpu, delay_delivery, &test, 1));
	CHECK(wait_for_completion_timeout(&test.done, GATE_TIMEOUT));
	hrtimer_cancel(&test.high);
	CHECK(test.valid && test.fires == 1);
	destroy_hrtimer_on_stack(&test.high);
	return 0;
}

struct busy_case {
	struct task_struct *task;
	struct completion *start;
	struct completion *running;
	unsigned int cpu;
	u64 iterations;
	bool valid;
	bool preempted;
};

static int busy_worker(void *argument)
{
	struct busy_case *test = argument;
	unsigned long switches;
	unsigned long ticks;
	u64 started;
	u64 elapsed;
	u64 until;
	u64 now;

	wait_for_completion(test->start);
	preempt_disable();
	switches = current->nivcsw;
	ticks = READ_ONCE(tick_get_tick_sched(test->cpu)->last_tick_jiffies);
	started = sched_clock();
	/* Require real tick delivery, not a hard real-time host latency bound. */
	do {
		cpu_relax();
		elapsed = sched_clock() - started;
		if (elapsed >= 20 * NSEC_PER_MSEC && need_resched() &&
		    READ_ONCE(tick_get_tick_sched(test->cpu)->last_tick_jiffies) != ticks)
			break;
	} while (elapsed < 5 * NSEC_PER_SEC);
	test->valid = irq_identity(test->cpu) && need_resched() &&
		elapsed < 5 * NSEC_PER_SEC &&
		current->nivcsw == switches &&
		READ_ONCE(tick_get_tick_sched(test->cpu)->last_tick_jiffies) != ticks;
	if (!test->valid)
		kobox_linux_boot_diagnostic(
			"kobox time: busy masked target=%u cpu=%u identity=%u resched=%u switches=%lu before=%lu ticks=%lu before=%lu\n",
			test->cpu, raw_smp_processor_id(), irq_identity(test->cpu),
			!!need_resched(), current->nivcsw, switches,
			READ_ONCE(tick_get_tick_sched(test->cpu)->last_tick_jiffies), ticks);
	preempt_enable();
	if (elapsed > 20 * NSEC_PER_MSEC)
		kobox_linux_boot_diagnostic(
			"kobox time: busy masked elapsed target=%u ns=%llu valid=%u\n",
			test->cpu, elapsed, test->valid);
	switches = current->nivcsw;
	complete(test->running);
	if (kobox_task_host()->monotonic_ns(&until)) {
		kobox_linux_boot_diagnostic("kobox time: busy clock failed target=%u\n", test->cpu);
		return -EIO;
	}
	until += 5 * NSEC_PER_SEC;
	/* No yield or schedule call: only an actual timer can time-slice this. */
	while (!kthread_should_stop()) {
		WRITE_ONCE(test->iterations, test->iterations + 1);
		if (!READ_ONCE(test->preempted) &&
		    READ_ONCE(current->nivcsw) >= switches + 2)
			WRITE_ONCE(test->preempted, true);
		cpu_relax();
		if (!(test->iterations & 0xffff)) {
			if (kobox_task_host()->monotonic_ns(&now) || now >= until) {
				kobox_linux_boot_diagnostic(
					"kobox time: busy timeout/clock target=%u iterations=%llu\n",
					test->cpu, test->iterations);
				return -ETIMEDOUT;
			}
		}
	}
	test->valid &= irq_identity(test->cpu) && current->nivcsw >= switches + 2;
	if (!test->valid)
		kobox_linux_boot_diagnostic(
			"kobox time: busy final target=%u cpu=%u identity=%u switches=%lu before=%lu iterations=%llu\n",
			test->cpu, raw_smp_processor_id(), irq_identity(test->cpu),
			current->nivcsw, switches, test->iterations);
	return test->valid ? 0 : -EINVAL;
}

static int test_tick_preemption(struct kobox_linux_task_report *report)
{
	struct busy_case tests[4] = {};
	DECLARE_COMPLETION_ONSTACK(start);
	DECLARE_COMPLETION_ONSTACK(running);
	unsigned long tick_before[2];
	unsigned int i;
	u64 started, now;
	bool preempted;
	int status = 0;

	for (i = 0; i < 2; i++)
		tick_before[i] = READ_ONCE(tick_get_tick_sched(i)->last_tick_jiffies);
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		tests[i].cpu = i / 2;
		tests[i].start = &start;
		tests[i].running = &running;
		tests[i].task = kthread_create(busy_worker, &tests[i], "tick-busy/%u", i);
		CHECK(!IS_ERR(tests[i].task));
		get_task_struct(tests[i].task);
		kthread_bind(tests[i].task, tests[i].cpu);
		wake_up_process(tests[i].task);
	}
	complete_all(&start);
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		if (!wait_for_completion_timeout(&running, GATE_TIMEOUT)) {
			kobox_linux_boot_diagnostic(
				"kobox time: running completion timeout index=%u\n", i);
			status = -ETIMEDOUT;
		}
	}
	if (kobox_task_host()->monotonic_ns(&started)) {
		status = -EIO;
		goto stop_workers;
	}
	/* Keep the original observation interval, but do not assume that the
	 * host delivers two involuntary switches within exactly 100 ms.
	 * Only the workers publish proof; this observer never drives an IRQ.
	 */
	do {
		msleep(100);
		if (kobox_task_host()->monotonic_ns(&now)) {
			status = -EIO;
			goto stop_workers;
		}
		preempted = true;
		for (i = 0; i < ARRAY_SIZE(tests); i++)
			preempted &= READ_ONCE(tests[i].preempted);
		if (now - started >= 5 * NSEC_PER_SEC) {
			kobox_linux_boot_diagnostic(
				"kobox time: busy observation timeout preempted=%u\n",
				preempted);
			status = -ETIMEDOUT;
			goto stop_workers;
		}
	} while (now - started < 100 * NSEC_PER_MSEC || !preempted);
	if (now - started > 100 * NSEC_PER_MSEC)
		kobox_linux_boot_diagnostic(
			"kobox time: busy observation elapsed ns=%llu\n", now - started);
stop_workers:
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		int stopped;

		if (!READ_ONCE(tests[i].iterations)) {
			kobox_linux_boot_diagnostic(
				"kobox time: zero iterations index=%u cpu=%u\n",
				i, tests[i].cpu);
			status = -EINVAL;
		}
		stopped = kthread_stop(tests[i].task);
		if (stopped) {
			kobox_linux_boot_diagnostic(
				"kobox time: worker stop index=%u cpu=%u result=%d\n",
				i, tests[i].cpu, stopped);
			status = -EINVAL;
		}
		put_task_struct(tests[i].task);
	}
	for (i = 0; i < 2; i++) {
		report->tick_progress[i] =
			READ_ONCE(tick_get_tick_sched(i)->last_tick_jiffies) - tick_before[i];
		if (!report->tick_progress[i]) {
			kobox_linux_boot_diagnostic("kobox time: no tick progress cpu=%u\n", i);
			status = -EINVAL;
		}
	}
	return status;
}

int kobox_task_time_gate(struct kobox_linux_task_report *report)
{
	unsigned int cpu;
	int status;

	for (cpu = 0; cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS; cpu++) {
		status = test_timers(cpu);
		if (status)
			return status;
		report->high_resolution_ready[cpu] = 1;
	}
	status = test_tick_preemption(report);
	if (status)
		return status;
	for (cpu = 0; cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS; cpu++) {
		CHECK(READ_ONCE(report->hardirq_entries[cpu]));
		CHECK(READ_ONCE(report->idle_irq_entries[cpu]));
		CHECK(READ_ONCE(report->idle_exits[cpu]));
	}
	return 0;
}
