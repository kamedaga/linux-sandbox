// SPDX-License-Identifier: GPL-2.0-only

#include "time_port.h"

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
	if (!(test)) \
		__builtin_trap(); \
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
	test->fires++;
	complete(&test->done);
	return HRTIMER_NORESTART;
}

static void wheel_callback(struct timer_list *timer)
{
	struct timer_case *test = timer_container_of(test, timer, wheel);

	test->valid &= in_serving_softirq() && irq_identity(test->cpu) &&
		time_after_eq(jiffies, test->wheel_deadline);
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
	if (test->cancel)
		test->valid &= hrtimer_cancel(&test->high) == 1;
	if (test->rearm) {
		test->deadline = ktime_get() + 10 * NSEC_PER_MSEC;
		hrtimer_start(&test->high, test->deadline,
			      HRTIMER_MODE_ABS_PINNED_HARD);
	}
	/* Also inject a stale/spurious device notification deterministically. */
	if (kobox_task_host()->cpu_notify(test->cpu, KOBOX_LINUX_TASK_CLOCKEVENT))
		__builtin_trap();
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
};

static int busy_worker(void *argument)
{
	struct busy_case *test = argument;
	unsigned long switches;
	unsigned long ticks;
	u64 until;
	u64 now;

	wait_for_completion(test->start);
	preempt_disable();
	switches = current->nivcsw;
	ticks = tick_get_tick_sched(test->cpu)->last_tick_jiffies;
	until = sched_clock() + 20 * NSEC_PER_MSEC;
	while (sched_clock() < until)
		cpu_relax();
	test->valid = irq_identity(test->cpu) && need_resched() &&
		current->nivcsw == switches &&
		tick_get_tick_sched(test->cpu)->last_tick_jiffies != ticks;
	preempt_enable();
	switches = current->nivcsw;
	complete(test->running);
	if (kobox_task_host()->monotonic_ns(&until))
		return -EIO;
	until += 5 * NSEC_PER_SEC;
	/* No yield or schedule call: only an actual timer can time-slice this. */
	while (!kthread_should_stop()) {
		WRITE_ONCE(test->iterations, test->iterations + 1);
		cpu_relax();
		if (!(test->iterations & 0xffff)) {
			if (kobox_task_host()->monotonic_ns(&now) || now >= until)
				return -ETIMEDOUT;
		}
	}
	test->valid &= irq_identity(test->cpu) && current->nivcsw >= switches + 2;
	return test->valid ? 0 : -EINVAL;
}

static int test_tick_preemption(struct kobox_linux_task_report *report)
{
	struct busy_case tests[4] = {};
	DECLARE_COMPLETION_ONSTACK(start);
	DECLARE_COMPLETION_ONSTACK(running);
	unsigned long tick_before[2];
	unsigned int i;
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
		if (!wait_for_completion_timeout(&running, GATE_TIMEOUT))
			status = -ETIMEDOUT;
	}
	msleep(100);
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		if (!READ_ONCE(tests[i].iterations))
			status = -EINVAL;
		if (kthread_stop(tests[i].task))
			status = -EINVAL;
		put_task_struct(tests[i].task);
	}
	for (i = 0; i < 2; i++) {
		report->tick_progress[i] =
			READ_ONCE(tick_get_tick_sched(i)->last_tick_jiffies) - tick_before[i];
		if (!report->tick_progress[i])
			status = -EINVAL;
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
