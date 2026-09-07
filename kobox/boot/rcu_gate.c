// SPDX-License-Identifier: GPL-2.0-only

#include "rcu_gate.h"
#include "host.h"

#include <linux/completion.h>
#include <linux/context_tracking_state.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/timekeeping.h>

#include "../../kernel/rcu/rcu.h"

#define GATE_WAIT (5 * HZ)
#define GATE_NS (5LL * NSEC_PER_SEC)
#define PAYLOAD_MAGIC 0x72ca91835bbad610ULL
#define PAYLOADS 3U

enum rcu_flavor {
	TREE,
	SRCU_DYNAMIC,
	SRCU_STATIC,
	FLAVOR_COUNT,
};

enum reader_scenario {
	BUSY_READER,
	NESTED_READER,
	PREEMPTED_READER,
	MIGRATED_IDLE,
	SLEEPING_IDLE,
	SCENARIO_COUNT,
};

enum gate_error {
	EARLY_RECLAIM = BIT(0),
	BAD_PAYLOAD = BIT(1),
	BAD_CONTEXT = BIT(2),
	EARLY_BARRIER = BIT(3),
	CALLBACK_TIMEOUT = BIT(4),
	READER_TIMEOUT = BIT(5),
	BAD_ORDER = BIT(6),
};

static const char * const flavor_names[] = {
	[TREE] = "tree-rcu",
	[SRCU_DYNAMIC] = "srcu-dynamic",
	[SRCU_STATIC] = "srcu-static",
};

static const char * const scenario_names[] = {
	[BUSY_READER] = "busy-reader",
	[NESTED_READER] = "nested-reader",
	[PREEMPTED_READER] = "preempted-reader",
	[MIGRATED_IDLE] = "migration-idle",
	[SLEEPING_IDLE] = "sleeping-idle",
};

DEFINE_STATIC_SRCU(kobox_gate_static_srcu);

struct rcu_case;

struct rcu_payload {
	struct rcu_head head;
	struct rcu_case *test;
	u64 magic;
	unsigned int id;
};

struct gate_thread {
	struct task_struct *task;
	struct rcu_case *test;
	struct completion done;
	bool started;
	bool finished;
};

struct rcu_case {
	struct kobox_linux_rcu_report *report;
	struct srcu_struct dynamic_srcu;
	struct srcu_struct *srcu;
	struct kmem_cache *cache;
	struct rcu_payload __rcu *published[PAYLOADS];
	struct rcu_payload *retired[PAYLOADS];
	struct gate_thread reader;
	struct gate_thread writer;
	struct gate_thread barrier;
	struct gate_thread witness;
	struct gate_thread preemptor;
	struct completion reader_ready;
	struct completion reader_release;
	struct completion callback_entered;
	struct rcu_gp_oldstate tree_cookie;
	unsigned long srcu_cookie;
	unsigned long srcu_exp_before;
	unsigned long reader_initial_switches;
	atomic_t active;
	atomic_t errors;
	atomic_t callbacks;
	atomic_t reclaimed;
	atomic_t barrier_probes;
	enum rcu_flavor flavor;
	enum reader_scenario scenario;
	unsigned int reader_cpu;
	unsigned int seen_cpu;
	bool expedited;
	bool drop_inner;
	bool inner_dropped;
	bool release_reader;
	bool release_preemptor;
	bool barrier_witnessed;
	u64 reader_stamp;
};

struct callback_probe {
	struct hrtimer timer;
	struct rcu_case *test;
	ktime_t deadline;
	unsigned int cpu;
	bool released;
};

static void record_error(struct rcu_case *test, enum gate_error error)
{
	atomic_or(error, &test->errors);
}

static int fail(struct rcu_case *test, unsigned int line)
{
	test->report->failure_line = line;
	test->report->errors = atomic_read(&test->errors);
	test->report->warnings = kobox_linux_exception_warnings();
	return -EINVAL;
}

static int await_flag(bool *flag)
{
	ktime_t deadline = ktime_get() + GATE_NS;

	/* Acquire each worker's publication of the preceding test phase. */
	while (!smp_load_acquire(flag)) {
		if (ktime_get() >= deadline)
			return -ETIMEDOUT;
		usleep_range(500, 1000);
	}
	return 0;
}

static int await_blocked(struct gate_thread *thread)
{
	ktime_t deadline = ktime_get() + GATE_NS;

	if (await_flag(&thread->started))
		return -ETIMEDOUT;
	do {
		/* Do not mistake the final join wait for a wait inside the RCU API. */
		if (smp_load_acquire(&thread->finished))
			return -EINVAL;
		if (wait_task_inactive(thread->task, TASK_NORMAL))
			return 0;
		usleep_range(500, 1000);
	} while (ktime_get() < deadline);
	return -ETIMEDOUT;
}

static int finish_thread(struct gate_thread *thread)
{
	/* Publish the result before the controller checks it or joins the task. */
	smp_store_release(&thread->finished, true);
	complete(&thread->done);
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int gate_start_thread(struct rcu_case *test, struct gate_thread *thread,
			int (*function)(void *), unsigned int cpu, bool fifo)
{
	thread->test = test;
	init_completion(&thread->done);
	thread->task = kthread_create(function, thread, "rcu-gate/%u", cpu);
	if (IS_ERR(thread->task))
		return PTR_ERR(thread->task);
	kthread_bind(thread->task, cpu);
	if (fifo)
		sched_set_fifo(thread->task);
	wake_up_process(thread->task);
	return 0;
}

static int join_thread(struct gate_thread *thread)
{
	if (!wait_for_completion_timeout(&thread->done, GATE_WAIT))
		return -ETIMEDOUT;
	return kthread_stop(thread->task);
}

static int reader_lock(struct rcu_case *test)
{
	if (test->srcu)
		return srcu_read_lock(test->srcu);
	rcu_read_lock();
	return 0;
}

static void reader_unlock(struct rcu_case *test, int index)
{
	if (test->srcu)
		srcu_read_unlock(test->srcu, index);
	else
		rcu_read_unlock();
}

static void inspect_payloads(struct rcu_case *test,
			     struct rcu_payload **payloads)
{
	unsigned int i;

	for (i = 0; i < PAYLOADS; i++)
		if (!payloads[i] || READ_ONCE(payloads[i]->magic) !=
		    (PAYLOAD_MAGIC ^ i))
			record_error(test, BAD_PAYLOAD);
	preempt_disable();
	if (current != raw_cpu_read(current_task) || !rcu_is_watching())
		record_error(test, BAD_CONTEXT);
	WRITE_ONCE(test->seen_cpu, raw_smp_processor_id());
	preempt_enable();
}

static int reader_main(void *argument)
{
	struct gate_thread *thread = argument;
	struct rcu_case *test = thread->test;
	struct rcu_payload *payloads[PAYLOADS];
	ktime_t deadline = ktime_get() + GATE_NS;
	unsigned int i;
	int outer, inner = 0;

	outer = reader_lock(test);
	test->reader_initial_switches = current->nivcsw;
	if (test->scenario == NESTED_READER)
		inner = reader_lock(test);
	for (i = 0; i < PAYLOADS; i++)
		payloads[i] = test->srcu ?
			srcu_dereference(test->published[i], test->srcu) :
			rcu_dereference(test->published[i]);
	atomic_inc(&test->active);
	inspect_payloads(test, payloads);
	/* Publish a live read-side section, not just a runnable kthread. */
	smp_store_release(&thread->started, true);
	complete(&test->reader_ready);
	if (test->scenario == SLEEPING_IDLE) {
		if (!wait_for_completion_timeout(&test->reader_release, GATE_WAIT))
			record_error(test, READER_TIMEOUT);
	} else {
		while (!READ_ONCE(test->release_reader)) {
			inspect_payloads(test, payloads);
			if (test->scenario == NESTED_READER &&
			    READ_ONCE(test->drop_inner) && !test->inner_dropped) {
				reader_unlock(test, inner);
				/* Publish inner unlock while the outer reader stays live. */
				smp_store_release(&test->inner_dropped, true);
			}
			if (ktime_get() >= deadline) {
				record_error(test, READER_TIMEOUT);
				break;
			}
			cpu_relax();
		}
	}
	inspect_payloads(test, payloads);
	if (test->scenario == NESTED_READER && !test->inner_dropped)
		reader_unlock(test, inner);
	/* No payload access follows this marker. On a broken GP the reclaimers
	 * report failure and retain the allocation while this count is nonzero.
	 */
	WRITE_ONCE(test->reader_stamp, PAYLOAD_MAGIC);
	atomic_dec(&test->active);
	reader_unlock(test, outer);
	return finish_thread(thread);
}

static bool gp_complete(struct rcu_case *test)
{
	if (test->srcu)
		return poll_state_synchronize_srcu(test->srcu, test->srcu_cookie);
	return poll_state_synchronize_rcu_full(&test->tree_cookie);
}

static bool can_reclaim(struct rcu_case *test)
{
	if (atomic_read(&test->active) || !READ_ONCE(test->release_reader)) {
		record_error(test, EARLY_RECLAIM);
		return false;
	}
	if (READ_ONCE(test->reader_stamp) != PAYLOAD_MAGIC) {
		record_error(test, BAD_ORDER);
		return false;
	}
	return true;
}

static void reclaim_payload(struct rcu_payload *payload)
{
	struct rcu_case *test = payload->test;

	if (!can_reclaim(test))
		return;
	kmem_cache_free(test->cache, payload);
	atomic_inc(&test->reclaimed);
}

static enum hrtimer_restart probe_barrier(struct hrtimer *timer)
{
	struct callback_probe *probe = container_of(timer, struct callback_probe,
						  timer);
	struct rcu_case *test = probe->test;

	if (!in_hardirq() || raw_smp_processor_id() != probe->cpu ||
	    ktime_get() < probe->deadline)
		record_error(test, BAD_CONTEXT);
	/* Acquire a possible barrier return while this callback is still active. */
	if (smp_load_acquire(&test->barrier.finished))
		record_error(test, EARLY_BARRIER);
	atomic_inc(&test->barrier_probes);
	WRITE_ONCE(probe->released, true);
	return HRTIMER_NORESTART;
}

static void reclaim_callback(struct rcu_head *head)
{
	struct rcu_payload *payload = container_of(head, struct rcu_payload, head);
	struct rcu_case *test = payload->test;
	struct callback_probe probe = { .test = test };
	ktime_t watchdog = ktime_get() + GATE_NS;

	if (!can_reclaim(test))
		return;
	if (payload->id != 1) {
		reclaim_payload(payload);
		atomic_inc(&test->callbacks);
		return;
	}
	if (irqs_disabled() || !rcu_is_watching()) {
		record_error(test, BAD_CONTEXT);
		return;
	}
	/*
	 * This callback was queued on CPU 0. Both native RCU callback queues and
	 * SRCU's rcu_gp per-CPU workqueue preserve that placement in this profile.
	 * CPU 1 remains available to execute the barrier and its witness task.
	 */
	preempt_disable();
	probe.cpu = raw_smp_processor_id();
	if (probe.cpu != 0)
		record_error(test, BAD_CONTEXT);
	complete(&test->callback_entered);
	hrtimer_setup_on_stack(&probe.timer, probe_barrier, CLOCK_MONOTONIC,
			       HRTIMER_MODE_ABS_PINNED_HARD);
	probe.deadline = ktime_get() + 10 * NSEC_PER_MSEC;
	hrtimer_start(&probe.timer, probe.deadline, HRTIMER_MODE_ABS_PINNED_HARD);
	/* Acquire proof that the barrier actually ran and blocked again. */
	while ((!smp_load_acquire(&test->barrier_witnessed) ||
		!READ_ONCE(probe.released)) &&
	       ktime_get() < watchdog)
		cpu_relax();
	if (!probe.released || !test->barrier_witnessed)
		record_error(test, CALLBACK_TIMEOUT);
	hrtimer_cancel(&probe.timer);
	destroy_hrtimer_on_stack(&probe.timer);
	preempt_enable();
	reclaim_payload(payload);
	atomic_inc(&test->callbacks);
}

static void enqueue_callback(void *argument)
{
	struct rcu_payload *payload = argument;
	struct rcu_case *test = payload->test;

	if (test->srcu)
		call_srcu(test->srcu, &payload->head, reclaim_callback);
	else
		call_rcu(&payload->head, reclaim_callback);
}

static int writer_main(void *argument)
{
	struct gate_thread *thread = argument;
	struct rcu_case *test = thread->test;

	/* Only the selected public GP API can block after this publication. */
	smp_store_release(&thread->started, true);
	if (test->srcu) {
		if (test->expedited)
			synchronize_srcu_expedited(test->srcu);
		else
			synchronize_srcu(test->srcu);
	} else if (test->expedited) {
		synchronize_rcu_expedited();
	} else {
		synchronize_rcu();
	}
	reclaim_payload(test->retired[0]);
	return finish_thread(thread);
}

static int barrier_main(void *argument)
{
	struct gate_thread *thread = argument;
	struct rcu_case *test = thread->test;

	/* Callbacks were enqueued on both CPUs before this thread was created. */
	smp_store_release(&thread->started, true);
	if (test->srcu)
		srcu_barrier(test->srcu);
	else
		rcu_barrier();
	if (atomic_read(&test->callbacks) != PAYLOADS - 1)
		record_error(test, EARLY_BARRIER);
	return finish_thread(thread);
}

static int witness_main(void *argument)
{
	struct gate_thread *thread = argument;
	struct rcu_case *test = thread->test;
	unsigned long switches;

	if (!wait_for_completion_timeout(&test->callback_entered, GATE_WAIT)) {
		record_error(test, CALLBACK_TIMEOUT);
		return finish_thread(thread);
	}
	switches = READ_ONCE(test->barrier.task->nvcsw);
	/* A barrier that merely waits for a GP must not pass because callback
	 * execution starved its task. Force a real recheck on the other CPU.
	 */
	if (READ_ONCE(test->barrier.finished) ||
	    !wake_up_process(test->barrier.task) || await_blocked(&test->barrier) ||
	    READ_ONCE(test->barrier.task->nvcsw) <= switches ||
	    atomic_read(&test->callbacks) >= PAYLOADS - 1)
		record_error(test, EARLY_BARRIER);
	/* Publish the witness results before allowing the callback to finish. */
	smp_store_release(&test->barrier_witnessed, true);
	return finish_thread(thread);
}

static int preemptor_main(void *argument)
{
	struct gate_thread *thread = argument;
	struct rcu_case *test = thread->test;
	ktime_t deadline = ktime_get() + GATE_NS;

	/* Higher-priority Linux task; it never directly switches the reader. */
	smp_store_release(&thread->started, true);
	while (!READ_ONCE(test->release_preemptor) && ktime_get() < deadline)
		cpu_relax();
	if (!test->release_preemptor)
		record_error(test, READER_TIMEOUT);
	return finish_thread(thread);
}

static int check_held(struct rcu_case *test)
{
	if (atomic_read(&test->active) != 1 || gp_complete(test) ||
	    READ_ONCE(test->writer.finished) || READ_ONCE(test->barrier.finished) ||
	    atomic_read(&test->callbacks) || atomic_read(&test->reclaimed) ||
	    atomic_read(&test->errors))
		return fail(test, __LINE__);
	return 0;
}

static int observe_idle(struct rcu_case *test, unsigned int cpu)
{
	ktime_t deadline = ktime_get() + GATE_NS;
	int state;

	do {
		state = atomic_read_acquire(&per_cpu(context_tracking, cpu).state);
		if ((state & CT_STATE_MASK) == CT_STATE_IDLE &&
		    !(state & CT_RCU_WATCHING)) {
			test->report->idle_observations++;
			return check_held(test);
		}
		usleep_range(500, 1000);
	} while (ktime_get() < deadline);
	return fail(test, __LINE__);
}

static int exercise_reader(struct rcu_case *test)
{
	unsigned long switches = test->reader_initial_switches;
	ktime_t deadline = ktime_get() + GATE_NS;
	union rcu_special special;
	int status;

	switch (test->scenario) {
	case NESTED_READER:
		WRITE_ONCE(test->drop_inner, true);
		if (await_flag(&test->inner_dropped))
			return fail(test, __LINE__);
		break;
	case PREEMPTED_READER:
		status = gate_start_thread(test, &test->preemptor, preemptor_main,
				      test->reader_cpu, true);
		if (status)
			return status;
		if (await_flag(&test->preemptor.started))
			return fail(test, __LINE__);
		if (READ_ONCE(test->reader.task->on_cpu) ||
		    READ_ONCE(test->reader.task->nivcsw) <= switches)
			return fail(test, __LINE__);
		if (!test->srcu) {
			special.s = READ_ONCE(test->reader.task->rcu_read_unlock_special.s);
			if (!special.b.blocked ||
			    READ_ONCE(test->reader.task->rcu_read_lock_nesting) != 1)
				return fail(test, __LINE__);
		}
		test->report->preemptions++;
		status = check_held(test);
		WRITE_ONCE(test->release_preemptor, true);
		if (status || join_thread(&test->preemptor))
			return fail(test, __LINE__);
		break;
	case MIGRATED_IDLE:
		status = set_cpus_allowed_ptr(test->reader.task,
					      cpumask_of(test->reader_cpu ^ 1));
		if (status)
			return status;
		while (READ_ONCE(test->seen_cpu) != (test->reader_cpu ^ 1)) {
			if (ktime_get() >= deadline)
				return fail(test, __LINE__);
			usleep_range(500, 1000);
		}
		test->report->migrations++;
		return observe_idle(test, test->reader_cpu);
	case SLEEPING_IDLE:
		if (await_blocked(&test->reader))
			return fail(test, __LINE__);
		return observe_idle(test, test->reader_cpu);
	default:
		break;
	}
	/* Give GP/callback/barrier workers execution opportunities while held. */
	msleep(20);
	return check_held(test);
}

static int prepare_case(struct rcu_case *test)
{
	unsigned int i;
	int status;

	if (test->flavor == SRCU_DYNAMIC) {
		test->srcu = &test->dynamic_srcu;
		status = init_srcu_struct(test->srcu);
		if (status)
			return status;
	} else if (test->flavor == SRCU_STATIC) {
		test->srcu = &kobox_gate_static_srcu;
	}
	init_completion(&test->reader_ready);
	init_completion(&test->reader_release);
	init_completion(&test->callback_entered);
	test->cache = kmem_cache_create("kobox-rcu-gate",
				      sizeof(struct rcu_payload), 0,
				      SLAB_NO_MERGE, NULL);
	if (!test->cache)
		return -ENOMEM;
	for (i = 0; i < PAYLOADS; i++) {
		test->retired[i] = kmem_cache_zalloc(test->cache, GFP_KERNEL);
		if (!test->retired[i])
			return -ENOMEM;
		test->retired[i]->test = test;
		test->retired[i]->id = i;
		test->retired[i]->magic = PAYLOAD_MAGIC ^ i;
		rcu_assign_pointer(test->published[i], test->retired[i]);
	}
	return 0;
}

static int start_reclaimers(struct rcu_case *test)
{
	unsigned int i;
	int status;

	if (test->srcu)
		test->srcu_cookie = get_state_synchronize_srcu(test->srcu);
	else
		get_state_synchronize_rcu_full(&test->tree_cookie);
	for (i = 0; i < PAYLOADS; i++)
		rcu_assign_pointer(test->published[i], NULL);
	for (i = 1; i < PAYLOADS; i++) {
		status = smp_call_function_single(i - 1, enqueue_callback,
						 test->retired[i], 1);
		if (status)
			return status;
	}
	if (test->srcu)
		test->srcu_exp_before =
			READ_ONCE(test->srcu->srcu_sup->srcu_gp_seq_needed_exp);
	status = gate_start_thread(test, &test->writer, writer_main,
			      test->reader_cpu ^ 1, false);
	if (status || await_blocked(&test->writer))
		return fail(test, __LINE__);
	status = gate_start_thread(test, &test->barrier, barrier_main,
			      1, false);
	if (status || await_blocked(&test->barrier))
		return fail(test, __LINE__);
	status = gate_start_thread(test, &test->witness, witness_main, 1, false);
	if (status)
		return status;
	if (test->srcu) {
		unsigned long requested =
			READ_ONCE(test->srcu->srcu_sup->srcu_gp_seq_needed_exp);

		/* Pending normal callbacks prevent SRCU's legitimate idle auto-expedite.
		 * Observe the request; do not force or rewrite the SRCU state machine.
		 */
		if (test->expedited != (requested != test->srcu_exp_before))
			return fail(test, __LINE__);
	}
	return check_held(test);
}

static int run_case(struct kobox_linux_rcu_report *report, enum rcu_flavor flavor,
		    enum reader_scenario scenario, bool expedited, unsigned int cpu)
{
	struct rcu_case *test;
	int status;

	strscpy(report->flavor, flavor_names[flavor], sizeof(report->flavor));
	strscpy(report->scenario, scenario_names[scenario], sizeof(report->scenario));
	report->cpu = cpu;
	report->expedited = expedited;
	report->phase = 1;
	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->report = report;
	test->flavor = flavor;
	test->scenario = scenario;
	test->expedited = expedited;
	test->reader_cpu = cpu;
	test->seen_cpu = cpu;
	status = prepare_case(test);
	if (status)
		return status;
	report->phase = 2;
	status = gate_start_thread(test, &test->reader, reader_main, cpu, false);
	if (status || !wait_for_completion_timeout(&test->reader_ready, GATE_WAIT))
		return fail(test, __LINE__);
	report->phase = 3;
	status = start_reclaimers(test);
	if (status)
		return status;
	report->phase = 4;
	status = exercise_reader(test);
	if (status)
		return status;
	report->phase = 5;
	WRITE_ONCE(test->release_reader, true);
	if (scenario == SLEEPING_IDLE)
		complete(&test->reader_release);
	if (join_thread(&test->reader) || join_thread(&test->writer) ||
	    join_thread(&test->barrier) || join_thread(&test->witness))
		return fail(test, __LINE__);
	if (!gp_complete(test) || atomic_read(&test->errors) ||
	    atomic_read(&test->active) || atomic_read(&test->reclaimed) != PAYLOADS ||
	    atomic_read(&test->callbacks) != PAYLOADS - 1 ||
	    atomic_read(&test->barrier_probes) != 1 || !test->barrier_witnessed)
		return fail(test, __LINE__);
	report->phase = 6;
	if (flavor == SRCU_DYNAMIC)
		cleanup_srcu_struct(test->srcu);
	/* A private, nonmerged SLUB cache must now be empty and destroy cleanly. */
	if (kmem_cache_shrink(test->cache))
		return fail(test, __LINE__);
	kmem_cache_destroy(test->cache);
	report->warnings = kobox_linux_exception_warnings();
	if (report->warnings)
		return fail(test, __LINE__);
	report->callbacks += atomic_read(&test->callbacks);
	report->barrier_probes += atomic_read(&test->barrier_probes);
	report->reclaimed += atomic_read(&test->reclaimed);
	if (expedited)
		report->expedited_gps++;
	else
		report->normal_gps++;
	report->passed++;
	kfree(test);
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_rcu_verify(struct kobox_linux_rcu_report *report)
{
	cpumask_t saved_affinity;
	enum rcu_flavor flavor;
	enum reader_scenario scenario;
	unsigned int cpu, expedited;
	int status;

	if (!report || report->size != sizeof(*report) ||
	    system_state != SYSTEM_RUNNING || num_online_cpus() != 2 ||
	    !IS_ENABLED(CONFIG_PREEMPT_RCU) || !IS_ENABLED(CONFIG_TREE_SRCU) ||
	    !rcu_inkernel_boot_has_ended() || rcu_gp_is_normal() ||
	    rcu_gp_is_expedited() || kobox_linux_exception_warnings())
		return -EINVAL;
	cpumask_copy(&saved_affinity, current->cpus_ptr);
	for_each_online_cpu(cpu) {
		status = set_cpus_allowed_ptr(current, cpumask_of(cpu ^ 1));
		if (status)
			return status;
		for (flavor = TREE; flavor < FLAVOR_COUNT; flavor++) {
			for (expedited = 0; expedited < 2; expedited++) {
				for (scenario = BUSY_READER; scenario < SCENARIO_COUNT;
				     scenario++) {
					if (flavor == TREE && scenario == SLEEPING_IDLE)
						continue;
					status = run_case(report, flavor, scenario,
							  expedited, cpu);
					if (status)
						return status;
				}
			}
		}
	}
	report->phase = 7;
	return set_cpus_allowed_ptr(current, &saved_affinity);
}
