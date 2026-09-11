// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "core_lifecycle.h"

#include "../host/posix/memory_resource.h"

#include <kobox2/closure_layout.h>
#include <kobox2/memory_arena_layout.h>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TEST_ARENA_SIZE (16u * 1024u * 1024u)
#define TEST_GENERATION UINT64_C(41)
#define TEST_OBJECT_ID UINT64_C(73)

#define CHECK(expression)                                                     \
	do {                                                                    \
		if (!(expression)) {                                              \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,    \
				__LINE__, #expression);                              \
			return -1;                                                  \
		}                                                               \
	} while (0)

struct cache_tracker {
	size_t constructors;
	size_t destructors;
};

struct cpu_thread_case {
	const struct kb2_core_cpu_operations *operations;
	void *binding_object;
	int status;
};

enum sync_thread_action {
	SYNC_THREAD_MUTEX_LOCK = 1,
	SYNC_THREAD_MUTEX_UNLOCK,
	SYNC_THREAD_EVENT_WAIT,
	SYNC_THREAD_COMPLETION_WAIT,
};

struct sync_thread_case {
	const struct kb2_core_sync_operations *operations;
	void *binding_object;
	void *object;
	uint32_t action;
	uint32_t started;
	kb2_core_status_t status;
};

enum thread_case_action {
	THREAD_CASE_RETURN = 1,
	THREAD_CASE_PARK,
	THREAD_CASE_DELAYED_PARK,
	THREAD_CASE_SYNC_WAIT,
	THREAD_CASE_DETACH_WAIT,
	THREAD_CASE_TIME_SLEEP,
};

struct thread_case {
	const struct kb2_core_thread_operations *operations;
	void *binding_object;
	const struct kb2_core_sync_operations *sync_operations;
	void *sync_binding_object;
	kb2_core_sync_event_t event;
	const struct kb2_core_time_operations *time_operations;
	void *time_binding_object;
	uint64_t time_deadline_ns;
	uint32_t time_wait_flags;
	kb2_core_thread_t observed_thread;
	uint32_t action;
	uint32_t phase;
	uint32_t release;
	uint32_t current_matches;
	uint32_t observed;
	kb2_core_status_t status;
};

struct time_callback_case {
	const struct kb2_core_time_operations *operations;
	void *binding_object;
	const struct kb2_core_cpu_operations *cpu_operations;
	void *cpu_binding_object;
	const struct kb2_core_thread_operations *thread_operations;
	void *thread_binding_object;
	uint64_t expiration_count;
	uint32_t calls;
	uint32_t context_class;
	uint32_t has_current_thread;
	kb2_core_status_t self_cancel_status;
};

struct work_callback_case {
	const struct kb2_core_workqueue_operations *operations;
	void *binding_object;
	const struct kb2_core_thread_operations *thread_operations;
	void *thread_binding_object;
	const struct kb2_core_cpu_operations *cpu_operations;
	void *cpu_binding_object;
	kb2_core_workqueue_queue_t queue;
	uint32_t calls;
	uint32_t phase;
	uint32_t release;
	uint32_t block_first;
	uint32_t has_current_thread;
	uint32_t context_class;
	uint32_t cpu_id;
	kb2_core_status_t self_cancel_status;
	kb2_core_status_t self_flush_status;
	kb2_core_status_t self_queue_flush_status;
};

struct work_order_case {
	uint32_t count;
	uint32_t order[2];
};

struct work_order_argument {
	struct work_order_case *test;
	uint32_t value;
};

struct rcu_callback_case {
	const struct kb2_core_rcu_operations *operations;
	void *binding_object;
	kb2_core_rcu_domain_t domain;
	const struct kb2_core_thread_operations *thread_operations;
	void *thread_binding_object;
	const struct kb2_core_cpu_operations *cpu_operations;
	void *cpu_binding_object;
	uint32_t calls;
	uint32_t order[2];
	uint32_t has_current_thread;
	uint32_t context_class;
	kb2_core_status_t self_barrier_status;
	kb2_core_status_t self_synchronize_status;
};

struct rcu_callback_argument {
	struct rcu_callback_case *test;
	uint32_t value;
};

struct rcu_wait_case {
	const struct kb2_core_rcu_operations *operations;
	void *binding_object;
	kb2_core_rcu_domain_t domain;
	kb2_core_rcu_read_token_t token;
	uint32_t action;
	uint32_t started;
	uint32_t finished;
	kb2_core_status_t status;
};

struct rcu_quiesce_reader_case {
	const struct kb2_core_rcu_operations *operations;
	void *binding_object;
	kb2_core_rcu_domain_t domain;
	uint32_t started;
	kb2_core_status_t status;
};

enum rcu_wait_action {
	RCU_WAIT_SYNCHRONIZE = 1,
	RCU_WAIT_UNLOCK,
	RCU_WAIT_DESTROY,
};

static void *memory_object;
static const struct kobox_resource_interface_operations *memory_operations;

static int resource_count(const struct kobox_module_context *context,
			  uint32_t slot_id, uint32_t *state_out,
			  size_t *count_out)
{
	(void)context;
	if (slot_id != KOBOX_LINUX_CORE_MEMORY_SLOT_ID || !state_out ||
	    !count_out)
		return KOBOX_MODULE_RESOURCE_NOT_VISIBLE;
	*state_out = KOBOX_MODULE_RESOURCE_PRESENT_STATE;
	*count_out = 1;
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_acquire(
	const struct kobox_module_context *context, uint32_t slot_id,
	size_t object_index, uint64_t required_rights,
	struct kobox_module_resource_handle *handle_out)
{
	if (!context || slot_id != KOBOX_LINUX_CORE_MEMORY_SLOT_ID ||
	    object_index || required_rights != KB2_MEMORY_ARENA_REQUIRED_RIGHTS ||
	    !handle_out)
		return KOBOX_MODULE_RESOURCE_INVALID_ARGUMENT;
	*handle_out = (struct kobox_module_resource_handle){
		.generation = context->generation,
		.object_id = TEST_OBJECT_ID,
	};
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_bind(
	const struct kobox_module_context *context,
	struct kobox_module_resource_handle handle,
	const uint8_t expected_digest[KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE],
	struct kobox_module_resource_binding *binding_out)
{
	static const uint8_t digest[KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE] =
		KB2_MEMORY_ARENA_SCHEMA_SHA256_BYTES;

	if (!context || handle.generation != context->generation ||
	    handle.object_id != TEST_OBJECT_ID || !expected_digest ||
	    memcmp(expected_digest, digest, sizeof(digest)) || !binding_out)
		return KOBOX_MODULE_RESOURCE_INTERFACE;
	binding_out->operations = memory_operations;
	binding_out->object = memory_object;
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_info(const struct kobox_module_context *context,
			 struct kobox_module_resource_handle handle,
			 struct kobox_module_resource_info *info_out)
{
	if (!context || handle.generation != context->generation ||
	    handle.object_id != TEST_OBJECT_ID || !info_out)
		return KOBOX_MODULE_RESOURCE_STALE;
	*info_out = (struct kobox_module_resource_info){
		.resource_type = KB2_CLOSURE_RESOURCE_MEMORY,
		.granted_rights = KB2_MEMORY_ARENA_REQUIRED_RIGHTS,
	};
	return KOBOX_MODULE_RESOURCE_OK;
}

static kb2_core_status_t cache_constructor(void *allocation,
					    size_t object_size,
					    void *argument)
{
	struct cache_tracker *tracker = argument;
	unsigned char *bytes = allocation;
	size_t index;

	if (!tracker || object_size != 96)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	for (index = 0; index < object_size; index++) {
		if (bytes[index])
			return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	bytes[0] = 0xa5;
	tracker->constructors++;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static void cache_destructor(void *allocation, size_t object_size,
			     void *argument)
{
	struct cache_tracker *tracker = argument;
	unsigned char *bytes = allocation;

	if (tracker && object_size == 96 && bytes[0] == 0xa5)
		tracker->destructors++;
}

static void *cpu_thread_worker(void *argument)
{
	struct cpu_thread_case *test = argument;
	uint32_t count;

	test->status = -1;
	if (test->operations->preempt_count(test->binding_object, &count) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    count ||
	    test->operations->preempt_disable(test->binding_object) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    test->operations->preempt_count(test->binding_object, &count) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    count != 1 ||
	    test->operations->preempt_enable(test->binding_object) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    test->operations->preempt_count(test->binding_object, &count) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    count)
		return NULL;
	test->status = 0;
	return NULL;
}

static void *sync_thread_worker(void *argument)
{
	struct sync_thread_case *test = argument;

	__atomic_store_n(&test->started, 1, __ATOMIC_RELEASE);
	switch (test->action) {
	case SYNC_THREAD_MUTEX_LOCK:
		test->status = test->operations->mutex_lock(
			test->binding_object, test->object, 0);
		if (test->status == KB2_CORE_RUNTIME_STATUS_OK)
			test->status = test->operations->mutex_unlock(
				test->binding_object, test->object);
		break;
	case SYNC_THREAD_MUTEX_UNLOCK:
		test->status = test->operations->mutex_unlock(
			test->binding_object, test->object);
		break;
	case SYNC_THREAD_EVENT_WAIT:
		test->status = test->operations->event_wait(
			test->binding_object, test->object, 0);
		break;
	case SYNC_THREAD_COMPLETION_WAIT:
		test->status = test->operations->completion_wait(
			test->binding_object, test->object, 0);
		break;
	default:
		test->status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
		break;
	}
	return NULL;
}

static uint64_t monotonic_now_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
	       (uint64_t)now.tv_nsec;
}

static int32_t thread_case_entry(void *argument)
{
	struct thread_case *test = argument;
	kb2_core_thread_t current = NULL;
	uint32_t value;

	test->status = test->operations->current(test->binding_object,
						 &current);
	if (test->status != KB2_CORE_RUNTIME_STATUS_OK)
		return -1;
	test->observed_thread = current;
	test->current_matches = 1;
	__atomic_store_n(&test->phase, 1, __ATOMIC_RELEASE);
	switch (test->action) {
	case THREAD_CASE_RETURN:
		break;
	case THREAD_CASE_PARK:
		test->status = test->operations->park(test->binding_object,
						 current, 0);
		break;
	case THREAD_CASE_DELAYED_PARK:
		while (!__atomic_load_n(&test->release, __ATOMIC_ACQUIRE))
			(void)test->operations->yield(test->binding_object);
		__atomic_store_n(&test->phase, 2, __ATOMIC_RELEASE);
		test->status = test->operations->park(test->binding_object,
						 current, 0);
		break;
	case THREAD_CASE_SYNC_WAIT:
		test->status = test->sync_operations->event_wait(
			test->sync_binding_object, test->event,
			KB2_CORE_RUNTIME_SYNC_WAIT_FLAG_INTERRUPTIBLE);
		break;
	case THREAD_CASE_DETACH_WAIT:
		while (!__atomic_load_n(&test->release, __ATOMIC_ACQUIRE))
			(void)test->operations->yield(test->binding_object);
		break;
	case THREAD_CASE_TIME_SLEEP:
		test->status = test->time_operations->sleep_until(
			test->time_binding_object,
			KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC,
			test->time_deadline_ns, test->time_wait_flags);
		__atomic_store_n(&test->phase, 3, __ATOMIC_RELEASE);
		return 123;
	default:
		test->status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
		return -1;
	}
	if (test->status == KB2_CORE_RUNTIME_STATUS_INTERRUPTED) {
		if (test->operations->is_interrupted(
			    test->binding_object, current, &value) !=
			    KB2_CORE_RUNTIME_STATUS_OK || !value ||
		    test->operations->clear_interrupt(
			    test->binding_object, current) !=
			    KB2_CORE_RUNTIME_STATUS_OK)
			return -1;
		test->observed = value;
	} else if (test->status == KB2_CORE_RUNTIME_STATUS_CANCELED) {
		if (test->operations->stop_requested(
			    test->binding_object, current, &value) !=
			    KB2_CORE_RUNTIME_STATUS_OK || !value)
			return -1;
		test->observed = value;
	}
	__atomic_store_n(&test->phase, 3, __ATOMIC_RELEASE);
	return 123;
}

static int wait_for_thread_phase(struct thread_case *test, uint32_t phase)
{
	uint32_t iteration;

	for (iteration = 0; iteration < 1000000; iteration++) {
		if (__atomic_load_n(&test->phase, __ATOMIC_ACQUIRE) >= phase)
			return 0;
		sched_yield();
	}
	return -1;
}

static void time_test_callback(kb2_core_time_timer_t timer, void *argument,
			       uint64_t expiration_count)
{
	struct time_callback_case *test = argument;
	kb2_core_thread_t current = NULL;
	uint32_t canceled;

	test->expiration_count += expiration_count;
	if (test->cpu_operations->context_class(
		    test->cpu_binding_object, &test->context_class) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		test->context_class = 0;
	if (test->thread_operations &&
	    test->thread_operations->current(
		    test->thread_binding_object, &current) ==
		    KB2_CORE_RUNTIME_STATUS_OK && current)
		test->has_current_thread = 1;
	test->self_cancel_status = test->operations->timer_cancel_sync(
		test->binding_object, timer, &canceled);
	__atomic_add_fetch(&test->calls, 1, __ATOMIC_RELEASE);
}

static int wait_for_time_calls(struct time_callback_case *test,
			       uint32_t calls)
{
	uint32_t iteration;

	for (iteration = 0; iteration < 1000000; iteration++) {
		if (__atomic_load_n(&test->calls, __ATOMIC_ACQUIRE) >= calls)
			return 0;
		sched_yield();
	}
	return -1;
}

static void work_test_callback(kb2_core_workqueue_work_t work, void *argument)
{
	struct work_callback_case *test = argument;
	kb2_core_thread_t current = NULL;
	uint32_t canceled;
	uint32_t call = __atomic_add_fetch(&test->calls, 1,
					   __ATOMIC_ACQ_REL);

	if (test->thread_operations->current(
		    test->thread_binding_object, &current) ==
		    KB2_CORE_RUNTIME_STATUS_OK && current)
		test->has_current_thread = 1;
	if (test->cpu_operations->context_class(
		    test->cpu_binding_object, &test->context_class) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		test->context_class = 0;
	if (test->cpu_operations->current(
		    test->cpu_binding_object, &test->cpu_id) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		test->cpu_id = UINT32_MAX;
	if (call == 1) {
		test->self_cancel_status = test->operations->cancel_sync(
			test->binding_object, work, &canceled);
		test->self_flush_status = test->operations->flush_work(
			test->binding_object, work);
		test->self_queue_flush_status = test->operations->flush_queue(
			test->binding_object, test->queue);
	}
	__atomic_store_n(&test->phase, call, __ATOMIC_RELEASE);
	if (test->block_first && call == 1) {
		while (!__atomic_load_n(&test->release, __ATOMIC_ACQUIRE))
			(void)test->thread_operations->yield(
				test->thread_binding_object);
	}
}

static int wait_for_work_phase(struct work_callback_case *test,
			       uint32_t phase)
{
	uint32_t iteration;

	for (iteration = 0; iteration < 1000000; iteration++) {
		if (__atomic_load_n(&test->phase, __ATOMIC_ACQUIRE) >= phase)
			return 0;
		sched_yield();
	}
	return -1;
}

static void work_order_callback(kb2_core_workqueue_work_t work,
				void *argument)
{
	struct work_order_argument *item = argument;
	uint32_t index = __atomic_fetch_add(&item->test->count, 1,
					    __ATOMIC_ACQ_REL);

	(void)work;
	if (index < sizeof(item->test->order) / sizeof(item->test->order[0]))
		item->test->order[index] = item->value;
}

static void rcu_test_callback(void *argument)
{
	struct rcu_callback_argument *item = argument;
	struct rcu_callback_case *test = item->test;
	kb2_core_thread_t current = NULL;
	uint32_t index = __atomic_fetch_add(&test->calls, 1,
					    __ATOMIC_ACQ_REL);

	if (index < sizeof(test->order) / sizeof(test->order[0]))
		test->order[index] = item->value;
	if (test->thread_operations->current(
		    test->thread_binding_object, &current) ==
		    KB2_CORE_RUNTIME_STATUS_OK && current)
		test->has_current_thread = 1;
	if (test->cpu_operations->context_class(
		    test->cpu_binding_object, &test->context_class) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		test->context_class = 0;
	if (!index)
		test->self_barrier_status = test->operations->barrier(
			test->binding_object, test->domain);
	if (!index)
		test->self_synchronize_status = test->operations->synchronize(
			test->binding_object, test->domain, 0);
}

static void *rcu_wait_worker(void *argument)
{
	struct rcu_wait_case *test = argument;

	__atomic_store_n(&test->started, 1, __ATOMIC_RELEASE);
	if (test->action == RCU_WAIT_SYNCHRONIZE)
		test->status = test->operations->synchronize(
			test->binding_object, test->domain,
			KB2_CORE_RUNTIME_RCU_SYNC_FLAG_EXPEDITED);
	else if (test->action == RCU_WAIT_UNLOCK)
		test->status = test->operations->read_unlock(
			test->binding_object, test->domain, test->token);
	else
		test->status = test->operations->domain_destroy(
			test->binding_object, test->domain);
	__atomic_store_n(&test->finished, 1, __ATOMIC_RELEASE);
	return NULL;
}

static void *rcu_quiesce_reader_worker(void *argument)
{
	struct rcu_quiesce_reader_case *test = argument;
	kb2_core_rcu_read_token_t token;

	test->status = test->operations->read_lock(
		test->binding_object, test->domain, &token);
	__atomic_store_n(&test->started, 1, __ATOMIC_RELEASE);
	if (test->status != KB2_CORE_RUNTIME_STATUS_OK)
		return NULL;
	for (;;) {
		kb2_core_rcu_domain_t observed = NULL;
		kb2_core_status_t status = test->operations->default_domain(
			test->binding_object, &observed);

		if (status == KB2_CORE_RUNTIME_STATUS_INVALID_STATE)
			break;
		if (status != KB2_CORE_RUNTIME_STATUS_OK ||
		    observed != test->domain) {
			if (status != KB2_CORE_RUNTIME_STATUS_OK)
				test->status = status;
			else
				test->status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
			(void)test->operations->read_unlock(
				test->binding_object, test->domain, token);
			return NULL;
		}
		sched_yield();
	}
	test->status = test->operations->read_unlock(
		test->binding_object, test->domain, token);
	return NULL;
}

static int test_time_operations(
	const struct kb2_core_time_operations *operations,
	struct kb2_core_binding *binding,
	const struct kb2_core_cpu_operations *cpu_operations,
	struct kb2_core_binding *cpu_binding,
	const struct kb2_core_thread_operations *thread_operations,
	struct kb2_core_binding *thread_binding)
{
	struct time_callback_case atomic_case = { 0 };
	struct time_callback_case thread_callback_case = { 0 };
	struct thread_case sleep_case;
	kb2_core_thread_t sleeper;
	int32_t exit_status;
	kb2_core_time_timer_t atomic_timer;
	kb2_core_time_timer_t thread_timer;
	uint64_t monotonic;
	uint64_t boottime;
	uint64_t realtime;
	uint64_t remaining;
	uint32_t pending;
	uint32_t canceled;

	CHECK(operations &&
	      operations->header.size == sizeof(*operations) &&
	      operations->header.interface_id == KB2_CORE_RUNTIME_INTERFACE_TIME);
	CHECK(operations->monotonic_ns(binding->object, &monotonic) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->boottime_ns(binding->object, &boottime) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->realtime_ns(binding->object, &realtime) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      monotonic && boottime >= monotonic && realtime > boottime);
	CHECK(operations->sleep_until(binding->object,
				      KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC,
				      monotonic - 1, 0) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->busy_wait_until(
		      binding->object, KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC,
		      monotonic + UINT64_C(100000)) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->monotonic_ns(binding->object, &monotonic) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->sleep_until(
		      binding->object, KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC,
		      monotonic + UINT64_C(1000000), 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->sleep_until(binding->object, 0, monotonic, 0) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT &&
	      operations->sleep_until(
		      binding->object, KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC,
		      monotonic, 2) == KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);
	sleep_case = (struct thread_case){
		.operations = thread_operations,
		.binding_object = thread_binding->object,
		.time_operations = operations,
		.time_binding_object = binding->object,
		.time_deadline_ns = monotonic + UINT64_C(10000000000),
		.time_wait_flags =
			KB2_CORE_RUNTIME_TIME_WAIT_FLAG_INTERRUPTIBLE,
		.action = THREAD_CASE_TIME_SLEEP,
	};
	CHECK(thread_operations->create(
		      thread_binding->object, thread_case_entry, &sleep_case,
		      "time-sleep", sizeof("time-sleep") - 1, 0, NULL, 0, 0,
		      &sleeper) == KB2_CORE_RUNTIME_STATUS_OK &&
	      !wait_for_thread_phase(&sleep_case, 1) &&
	      thread_operations->interrupt(thread_binding->object, sleeper) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      thread_operations->join(thread_binding->object, sleeper, 0,
				      &exit_status) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      sleep_case.status == KB2_CORE_RUNTIME_STATUS_INTERRUPTED);

	atomic_case = (struct time_callback_case){
		.operations = operations,
		.binding_object = binding->object,
		.cpu_operations = cpu_operations,
		.cpu_binding_object = cpu_binding->object,
	};
	CHECK(operations->timer_create(
		      binding->object, KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC,
		      KB2_CORE_RUNTIME_TIME_TIMER_CONTEXT_ATOMIC,
		      time_test_callback, &atomic_case,
		      KB2_CORE_RUNTIME_TIME_TIMER_FLAG_PINNED,
		      &atomic_timer) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->monotonic_ns(binding->object, &monotonic) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->timer_arm(binding->object, atomic_timer,
				    monotonic + UINT64_C(2000000), 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->timer_is_pending(binding->object, atomic_timer,
					   &pending) ==
		      KB2_CORE_RUNTIME_STATUS_OK && pending &&
	      operations->timer_remaining(binding->object, atomic_timer,
					  &remaining) ==
		      KB2_CORE_RUNTIME_STATUS_OK && remaining &&
	      !wait_for_time_calls(&atomic_case, 1));
	CHECK(atomic_case.expiration_count == 1 &&
	      atomic_case.context_class ==
		      KB2_CORE_RUNTIME_CPU_CONTEXT_SOFTIRQ &&
	      atomic_case.self_cancel_status == KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->timer_is_pending(binding->object, atomic_timer,
					   &pending) ==
		      KB2_CORE_RUNTIME_STATUS_OK && !pending &&
	      operations->timer_destroy(binding->object, atomic_timer) ==
		      KB2_CORE_RUNTIME_STATUS_OK);

	thread_callback_case = (struct time_callback_case){
		.operations = operations,
		.binding_object = binding->object,
		.cpu_operations = cpu_operations,
		.cpu_binding_object = cpu_binding->object,
		.thread_operations = thread_operations,
		.thread_binding_object = thread_binding->object,
	};
	CHECK(operations->timer_create(
		      binding->object, KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC,
		      KB2_CORE_RUNTIME_TIME_TIMER_CONTEXT_THREAD,
		      time_test_callback, &thread_callback_case, 0,
		      &thread_timer) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->monotonic_ns(binding->object, &monotonic) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->timer_arm(binding->object, thread_timer,
				    monotonic + UINT64_C(2000000),
				    UINT64_C(1000000)) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !wait_for_time_calls(&thread_callback_case, 1));
	CHECK(thread_callback_case.expiration_count >= 1 &&
	      thread_callback_case.context_class ==
		      KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD &&
	      thread_callback_case.has_current_thread &&
	      thread_callback_case.self_cancel_status ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->timer_cancel_sync(binding->object, thread_timer,
					    &canceled) ==
		      KB2_CORE_RUNTIME_STATUS_OK && canceled &&
	      operations->timer_destroy(binding->object, thread_timer) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	return 0;
}

static int test_workqueue_operations(
	const struct kb2_core_workqueue_operations *operations,
	struct kb2_core_binding *binding,
	const struct kb2_core_thread_operations *thread_operations,
	struct kb2_core_binding *thread_binding,
	const struct kb2_core_cpu_operations *cpu_operations,
	struct kb2_core_binding *cpu_binding,
	const struct kb2_core_time_operations *time_operations,
	struct kb2_core_binding *time_binding)
{
	struct work_callback_case blocking_case = { 0 };
	struct work_callback_case delayed_case = { 0 };
	struct work_callback_case reclaim_blocked_case = { 0 };
	struct work_callback_case reclaim_progress_case = { 0 };
	struct work_order_case order_case = { 0 };
	struct work_order_argument order_arguments[2] = {
		{ .test = &order_case, .value = 1 },
		{ .test = &order_case, .value = 2 },
	};
	kb2_core_workqueue_queue_t queue;
	kb2_core_workqueue_queue_t ordered_queue;
	kb2_core_workqueue_queue_t reclaim_queue;
	kb2_core_workqueue_work_t blocking_work;
	kb2_core_workqueue_work_t delayed_work;
	kb2_core_workqueue_work_t reclaim_blocked_work;
	kb2_core_workqueue_work_t reclaim_progress_work;
	kb2_core_workqueue_work_t order_work[2];
	uint64_t now;
	uint32_t result;

	CHECK(operations && operations->header.size == sizeof(*operations) &&
	      operations->header.interface_id ==
		      KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE);
	CHECK(operations->queue_create(
		      binding->object, "bad-ordered", sizeof("bad-ordered") - 1,
		      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_ORDERED, 1, &queue) ==
	      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);
	CHECK(operations->queue_create(
		      binding->object, "bad-rescuer", sizeof("bad-rescuer") - 1,
		      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND |
			      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_ORDERED |
			      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_MEMORY_RECLAIM,
		      1, &queue) == KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);
	CHECK(operations->queue_create(
		      binding->object, "ordered", sizeof("ordered") - 1,
		      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND |
			      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_ORDERED,
		      1, &ordered_queue) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->queue_create(
		      binding->object, "bound", sizeof("bound") - 1, 0, 1,
		      &queue) == KB2_CORE_RUNTIME_STATUS_OK);

	blocking_case = (struct work_callback_case){
		.operations = operations,
		.binding_object = binding->object,
		.thread_operations = thread_operations,
		.thread_binding_object = thread_binding->object,
		.cpu_operations = cpu_operations,
		.cpu_binding_object = cpu_binding->object,
		.queue = queue,
		.block_first = 1,
	};
	CHECK(operations->work_create(binding->object, work_test_callback,
				      &blocking_case, &blocking_work) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->submit(binding->object, queue, blocking_work, 2,
				 &result) ==
	      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);
	CHECK(operations->submit(binding->object, ordered_queue, blocking_work,
				 0, &result) ==
	      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);
	CHECK(operations->submit(binding->object, queue, blocking_work, 1,
				 &result) == KB2_CORE_RUNTIME_STATUS_OK &&
	      result && !wait_for_work_phase(&blocking_case, 1));
	CHECK(blocking_case.has_current_thread &&
	      blocking_case.context_class == KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD &&
	      blocking_case.cpu_id == 1 &&
	      blocking_case.self_cancel_status ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      blocking_case.self_flush_status ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      blocking_case.self_queue_flush_status ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK);
	CHECK(operations->submit(binding->object, queue, blocking_work, 1,
				 &result) == KB2_CORE_RUNTIME_STATUS_OK && result);
	CHECK(operations->submit(binding->object, queue, blocking_work, 1,
				 &result) == KB2_CORE_RUNTIME_STATUS_OK && !result);
	__atomic_store_n(&blocking_case.release, 1, __ATOMIC_RELEASE);
	CHECK(operations->flush_work(binding->object, blocking_work) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      blocking_case.calls == 2 &&
	      operations->work_destroy(binding->object, blocking_work) ==
		      KB2_CORE_RUNTIME_STATUS_OK);

	delayed_case = (struct work_callback_case){
		.operations = operations,
		.binding_object = binding->object,
		.thread_operations = thread_operations,
		.thread_binding_object = thread_binding->object,
		.cpu_operations = cpu_operations,
		.cpu_binding_object = cpu_binding->object,
		.queue = queue,
	};
	CHECK(operations->work_create(binding->object, work_test_callback,
				      &delayed_case, &delayed_work) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(time_operations->monotonic_ns(time_binding->object, &now) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->submit_at(
		      binding->object, queue, delayed_work, 0,
		      now + UINT64_C(1000000000), &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result &&
	      operations->is_pending(binding->object, delayed_work, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK && result);
	CHECK(operations->reschedule_at(
		      binding->object, queue, delayed_work, 0,
		      now + UINT64_C(2000000), &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK && result);
	CHECK(operations->flush_queue(binding->object, queue) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      delayed_case.calls == 1);
	CHECK(time_operations->monotonic_ns(time_binding->object, &now) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->submit_at(
		      binding->object, queue, delayed_work, 0,
		      now + UINT64_C(1000000000), &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK && result &&
	      operations->cancel(binding->object, delayed_work, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK && result &&
	      operations->cancel_sync(binding->object, delayed_work, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK && !result &&
	      operations->is_pending(binding->object, delayed_work, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK && !result);
	CHECK(operations->work_destroy(binding->object, delayed_work) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->queue_destroy(binding->object, queue) ==
		      KB2_CORE_RUNTIME_STATUS_OK);

	CHECK(operations->queue_create(
		      binding->object, "reclaim", sizeof("reclaim") - 1,
		      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_MEMORY_RECLAIM, 1,
		      &reclaim_queue) == KB2_CORE_RUNTIME_STATUS_OK);
	reclaim_blocked_case = (struct work_callback_case){
		.operations = operations,
		.binding_object = binding->object,
		.thread_operations = thread_operations,
		.thread_binding_object = thread_binding->object,
		.cpu_operations = cpu_operations,
		.cpu_binding_object = cpu_binding->object,
		.queue = reclaim_queue,
		.block_first = 1,
	};
	reclaim_progress_case = reclaim_blocked_case;
	reclaim_progress_case.block_first = 0;
	CHECK(operations->work_create(
		      binding->object, work_test_callback, &reclaim_blocked_case,
		      &reclaim_blocked_work) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->work_create(
		      binding->object, work_test_callback, &reclaim_progress_case,
		      &reclaim_progress_work) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->submit(binding->object, reclaim_queue,
				 reclaim_blocked_work, 0, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result && !wait_for_work_phase(&reclaim_blocked_case, 1));
	CHECK(operations->submit(binding->object, reclaim_queue,
				 reclaim_progress_work, 0, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result && !wait_for_work_phase(&reclaim_progress_case, 1));
	__atomic_store_n(&reclaim_blocked_case.release, 1, __ATOMIC_RELEASE);
	CHECK(operations->flush_queue(binding->object, reclaim_queue) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->work_destroy(binding->object,
				       reclaim_progress_work) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->work_destroy(binding->object,
				       reclaim_blocked_work) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->queue_destroy(binding->object, reclaim_queue) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->work_create(
		      binding->object, work_order_callback, &order_arguments[0],
		      &order_work[0]) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->work_create(
		      binding->object, work_order_callback, &order_arguments[1],
		      &order_work[1]) == KB2_CORE_RUNTIME_STATUS_OK &&
	      time_operations->monotonic_ns(time_binding->object, &now) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->submit_at(
		      binding->object, ordered_queue, order_work[0],
		      KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY,
		      now + UINT64_C(4000000), &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result &&
	      operations->submit_at(
		      binding->object, ordered_queue, order_work[1],
		      KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY,
		      now + UINT64_C(2000000), &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result &&
	      operations->flush_queue(binding->object, ordered_queue) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      order_case.count == 2 && order_case.order[0] == 2 &&
	      order_case.order[1] == 1 &&
	      operations->work_destroy(binding->object, order_work[1]) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->work_destroy(binding->object, order_work[0]) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->queue_destroy(binding->object, ordered_queue) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	return 0;
}

static int test_rcu_operations(
	const struct kb2_core_rcu_operations *operations,
	struct kb2_core_binding *binding,
	const struct kb2_core_thread_operations *thread_operations,
	struct kb2_core_binding *thread_binding,
	const struct kb2_core_cpu_operations *cpu_operations,
	struct kb2_core_binding *cpu_binding)
{
	struct rcu_callback_case callback_case = { 0 };
	struct rcu_callback_argument callback_arguments[2] = {
		{ .test = &callback_case, .value = 1 },
		{ .test = &callback_case, .value = 2 },
	};
	struct rcu_wait_case wait_case;
	kb2_core_rcu_domain_t default_domain;
	kb2_core_rcu_domain_t classic_domain;
	kb2_core_rcu_domain_t srcu_domain;
	kb2_core_rcu_read_token_t old_token;
	kb2_core_rcu_read_token_t later_token;
	pthread_t waiter;
	uint32_t iteration;

	CHECK(operations && operations->header.size == sizeof(*operations) &&
	      operations->header.interface_id == KB2_CORE_RUNTIME_INTERFACE_RCU);
	CHECK(operations->default_domain(binding->object, &default_domain) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->domain_destroy(binding->object, default_domain) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_STATE);
	CHECK(operations->domain_create(
		      binding->object, 0, &classic_domain) ==
	      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);
	CHECK(operations->domain_create(
		      binding->object, KB2_CORE_RUNTIME_RCU_DOMAIN_CLASSIC,
		      &classic_domain) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->domain_create(
		      binding->object, KB2_CORE_RUNTIME_RCU_DOMAIN_SRCU,
		      &srcu_domain) == KB2_CORE_RUNTIME_STATUS_OK);

	CHECK(operations->read_lock(binding->object, classic_domain,
				    &old_token) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->synchronize(binding->object, classic_domain, 0) ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->quiescent_state(binding->object, classic_domain) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_STATE &&
	      operations->read_unlock(binding->object, srcu_domain, old_token) ==
		      KB2_CORE_RUNTIME_STATUS_OWNER &&
	      operations->read_unlock(binding->object, classic_domain,
				      old_token) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->quiescent_state(binding->object, classic_domain) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->quiescent_state(binding->object, srcu_domain) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT &&
	      operations->synchronize(binding->object, classic_domain, 2) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);

	CHECK(operations->read_lock(binding->object, classic_domain,
				    &old_token) == KB2_CORE_RUNTIME_STATUS_OK);
	wait_case = (struct rcu_wait_case){
		.operations = operations,
		.binding_object = binding->object,
		.domain = classic_domain,
		.action = RCU_WAIT_SYNCHRONIZE,
	};
	CHECK(!pthread_create(&waiter, NULL, rcu_wait_worker, &wait_case));
	while (!__atomic_load_n(&wait_case.started, __ATOMIC_ACQUIRE))
		sched_yield();
	for (iteration = 0; iteration < 10000; iteration++)
		sched_yield();
	CHECK(!__atomic_load_n(&wait_case.finished, __ATOMIC_ACQUIRE) &&
	      operations->read_unlock(binding->object, classic_domain,
				      old_token) == KB2_CORE_RUNTIME_STATUS_OK &&
	      !pthread_join(waiter, NULL) &&
	      wait_case.status == KB2_CORE_RUNTIME_STATUS_OK);

	callback_case = (struct rcu_callback_case){
		.operations = operations,
		.binding_object = binding->object,
		.domain = classic_domain,
		.thread_operations = thread_operations,
		.thread_binding_object = thread_binding->object,
		.cpu_operations = cpu_operations,
		.cpu_binding_object = cpu_binding->object,
	};
	CHECK(operations->read_lock(binding->object, classic_domain,
				    &old_token) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->call(binding->object, classic_domain,
			       rcu_test_callback, &callback_arguments[0]) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->call(binding->object, classic_domain,
			       rcu_test_callback, &callback_arguments[1]) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->barrier(binding->object, classic_domain) ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->read_lock(binding->object, classic_domain,
				    &later_token) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->read_unlock(binding->object, classic_domain,
				      old_token) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->barrier(binding->object, classic_domain) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      callback_case.calls == 2 && callback_case.order[0] == 1 &&
	      callback_case.order[1] == 2 && callback_case.has_current_thread &&
	      callback_case.context_class == KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD &&
	      callback_case.self_barrier_status ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      callback_case.self_synchronize_status ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->read_unlock(binding->object, classic_domain,
				      later_token) == KB2_CORE_RUNTIME_STATUS_OK);

	CHECK(operations->read_lock(binding->object, srcu_domain, &old_token) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	wait_case = (struct rcu_wait_case){
		.operations = operations,
		.binding_object = binding->object,
		.domain = srcu_domain,
		.token = old_token,
		.action = RCU_WAIT_UNLOCK,
	};
	CHECK(!pthread_create(&waiter, NULL, rcu_wait_worker, &wait_case) &&
	      !pthread_join(waiter, NULL) &&
	      wait_case.status == KB2_CORE_RUNTIME_STATUS_OWNER);
	wait_case = (struct rcu_wait_case){
		.operations = operations,
		.binding_object = binding->object,
		.domain = srcu_domain,
		.action = RCU_WAIT_DESTROY,
	};
	CHECK(!pthread_create(&waiter, NULL, rcu_wait_worker, &wait_case));
	while (!__atomic_load_n(&wait_case.started, __ATOMIC_ACQUIRE))
		sched_yield();
	for (iteration = 0; iteration < 10000; iteration++)
		sched_yield();
	CHECK(!__atomic_load_n(&wait_case.finished, __ATOMIC_ACQUIRE) &&
	      operations->read_unlock(binding->object, srcu_domain, old_token) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !pthread_join(waiter, NULL) &&
	      wait_case.status == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->domain_destroy(binding->object, classic_domain) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	return 0;
}

static int wait_until_started(struct sync_thread_case *test)
{
	unsigned int iteration;

	for (iteration = 0; iteration < 100000; iteration++) {
		if (__atomic_load_n(&test->started, __ATOMIC_ACQUIRE))
			return 0;
		sched_yield();
	}
	return -1;
}

static int test_sync_operations(
	const struct kb2_core_sync_operations *operations,
	struct kb2_core_binding *binding,
	struct kb2_core_binding *other_binding)
{
	kb2_core_sync_spin_t spin;
	kb2_core_sync_mutex_t mutex;
	kb2_core_sync_rwlock_t rwlock;
	kb2_core_sync_semaphore_t semaphore;
	kb2_core_sync_event_t event;
	kb2_core_sync_event_t manual_event;
	kb2_core_sync_completion_t completion;
	struct sync_thread_case thread_case;
	pthread_t thread;
	uint64_t deadline = monotonic_now_ns();
	uint32_t result;
	unsigned int iteration;

	CHECK(deadline && operations &&
	      operations->header.size == sizeof(*operations) &&
	      operations->header.interface_id == KB2_CORE_RUNTIME_INTERFACE_SYNC);

	CHECK(operations->spin_create(binding->object, &spin) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->spin_lock(binding->object, spin) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->spin_try_lock(binding->object, spin, &result) ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->spin_destroy(binding->object, spin) ==
		      KB2_CORE_RUNTIME_STATUS_BUSY &&
	      operations->spin_unlock(binding->object, spin) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->spin_destroy(other_binding->object, spin) ==
		      KB2_CORE_RUNTIME_STATUS_OWNER &&
	      operations->spin_destroy(binding->object, spin) ==
		      KB2_CORE_RUNTIME_STATUS_OK);

	CHECK(operations->mutex_create(binding->object, &mutex) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->mutex_try_lock(binding->object, mutex, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 1 &&
	      operations->mutex_lock(binding->object, mutex, 0) ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK);
	thread_case = (struct sync_thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.object = mutex,
		.action = SYNC_THREAD_MUTEX_UNLOCK,
	};
	CHECK(!pthread_create(&thread, NULL, sync_thread_worker, &thread_case) &&
	      !pthread_join(thread, NULL) &&
	      thread_case.status == KB2_CORE_RUNTIME_STATUS_OWNER);
	thread_case = (struct sync_thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.object = mutex,
		.action = SYNC_THREAD_MUTEX_LOCK,
	};
	CHECK(!pthread_create(&thread, NULL, sync_thread_worker, &thread_case) &&
	      !wait_until_started(&thread_case) &&
	      operations->mutex_destroy(binding->object, mutex) ==
		      KB2_CORE_RUNTIME_STATUS_BUSY &&
	      operations->mutex_unlock(binding->object, mutex) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !pthread_join(thread, NULL) &&
	      thread_case.status == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->mutex_lock(binding->object, mutex, 2) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT &&
	      operations->mutex_lock(binding->object, mutex, 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->mutex_lock_until(
		      other_binding->object, mutex, deadline - 1, 0) ==
		      KB2_CORE_RUNTIME_STATUS_OWNER &&
	      operations->mutex_unlock(binding->object, mutex) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->mutex_destroy(binding->object, mutex) ==
		      KB2_CORE_RUNTIME_STATUS_OK);

	CHECK(operations->rwlock_create(binding->object, &rwlock) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->rwlock_read_try_lock(
		      binding->object, rwlock, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 1 &&
	      operations->rwlock_read_lock(binding->object, rwlock, 0) ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->rwlock_write_try_lock(
		      binding->object, rwlock, &result) ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->rwlock_read_unlock(binding->object, rwlock) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->rwlock_write_lock(binding->object, rwlock, 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->rwlock_read_try_lock(
		      binding->object, rwlock, &result) ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->rwlock_write_lock_until(
		      binding->object, rwlock, deadline - 1, 0) ==
		      KB2_CORE_RUNTIME_STATUS_DEADLOCK &&
	      operations->rwlock_write_unlock(binding->object, rwlock) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->rwlock_read_lock_until(
		      binding->object, rwlock, 0, 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->rwlock_read_unlock(binding->object, rwlock) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->rwlock_destroy(binding->object, rwlock) ==
		      KB2_CORE_RUNTIME_STATUS_OK);

	CHECK(operations->semaphore_create(binding->object, 0, 2,
					   &semaphore) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->semaphore_try_down(
		      binding->object, semaphore, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 0 &&
	      operations->semaphore_up(binding->object, semaphore, 0) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT &&
	      operations->semaphore_up(binding->object, semaphore, 3) ==
		      KB2_CORE_RUNTIME_STATUS_EXHAUSTED &&
	      operations->semaphore_up(binding->object, semaphore, 2) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->semaphore_down(binding->object, semaphore, 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->semaphore_down_until(
		      binding->object, semaphore, 0, 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->semaphore_down_until(
		      binding->object, semaphore, deadline - 1, 0) ==
		      KB2_CORE_RUNTIME_STATUS_TIMED_OUT &&
	      operations->semaphore_destroy(binding->object, semaphore) ==
		      KB2_CORE_RUNTIME_STATUS_OK);

	CHECK(operations->event_create(
		      binding->object,
		      KB2_CORE_RUNTIME_SYNC_EVENT_FLAG_INITIAL_SIGNALED,
		      &event) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->event_try_wait(binding->object, event, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 1 &&
	      operations->event_try_wait(binding->object, event, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 0 &&
	      operations->event_signal(binding->object, event) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->event_signal(binding->object, event) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->event_wait(binding->object, event, 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->event_wait_until(
		      binding->object, event, deadline - 1, 0) ==
		      KB2_CORE_RUNTIME_STATUS_TIMED_OUT &&
	      operations->event_reset(binding->object, event) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->event_destroy(binding->object, event) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->event_create(
		      binding->object,
		      KB2_CORE_RUNTIME_SYNC_EVENT_FLAG_MANUAL_RESET,
		      &manual_event) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->event_signal(binding->object, manual_event) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->event_try_wait(
		      binding->object, manual_event, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 1 &&
	      operations->event_try_wait(
		      binding->object, manual_event, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 1 &&
	      operations->event_reset(binding->object, manual_event) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->event_destroy(binding->object, manual_event) ==
		      KB2_CORE_RUNTIME_STATUS_OK);

	CHECK(operations->completion_create(binding->object, &completion) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->completion_complete(binding->object, completion) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->completion_complete(binding->object, completion) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->completion_wait(binding->object, completion, 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->completion_try_wait(
		      binding->object, completion, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 1 &&
	      operations->completion_try_wait(
		      binding->object, completion, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 0);
	thread_case = (struct sync_thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.object = completion,
		.action = SYNC_THREAD_COMPLETION_WAIT,
	};
	CHECK(!pthread_create(&thread, NULL, sync_thread_worker, &thread_case) &&
	      !wait_until_started(&thread_case));
	for (iteration = 0; iteration < 100000; iteration++) {
		if (operations->completion_reinit(
			    binding->object, completion) ==
		    KB2_CORE_RUNTIME_STATUS_BUSY)
			break;
		sched_yield();
	}
	CHECK(iteration != 100000 &&
	      operations->completion_complete(binding->object, completion) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !pthread_join(thread, NULL) &&
	      thread_case.status == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->completion_complete_all(
		      binding->object, completion) == KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->completion_wait_until(
		      binding->object, completion, deadline - 1, 0) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->completion_reinit(binding->object, completion) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->completion_try_wait(
		      binding->object, completion, &result) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      result == 0 &&
	      operations->completion_destroy(binding->object, completion) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	return 0;
}

static int test_sync_quiesce(struct kobox_module_context *context,
			     struct kb2_core_binding *sync_binding)
{
	const struct kb2_core_sync_operations *operations;
	kb2_core_sync_completion_t completion;
	struct sync_thread_case thread_case;
	pthread_t thread;
	unsigned int iteration;

	operations = sync_binding->operations;
	CHECK(operations->completion_create(sync_binding->object, &completion) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	thread_case = (struct sync_thread_case){
		.operations = operations,
		.binding_object = sync_binding->object,
		.object = completion,
		.action = SYNC_THREAD_COMPLETION_WAIT,
	};
	CHECK(!pthread_create(&thread, NULL, sync_thread_worker, &thread_case) &&
	      !wait_until_started(&thread_case));
	for (iteration = 0; iteration < 100000; iteration++) {
		if (operations->completion_reinit(
			    sync_binding->object, completion) ==
		    KB2_CORE_RUNTIME_STATUS_BUSY)
			break;
		sched_yield();
	}
	CHECK(iteration != 100000 && !kobox_linux_core_quiesce(context) &&
	      !pthread_join(thread, NULL) &&
	      thread_case.status == KB2_CORE_RUNTIME_STATUS_CANCELED);
	CHECK(operations->completion_wait(
		      sync_binding->object, completion, 0) ==
		      KB2_CORE_RUNTIME_STATUS_CANCELED &&
	      operations->completion_destroy(
		      sync_binding->object, completion) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	return 0;
}

static int test_thread_operations(
	const struct kb2_core_thread_operations *operations,
	struct kb2_core_binding *binding,
	const struct kb2_core_sync_operations *sync_operations,
	struct kb2_core_binding *sync_binding)
{
	struct thread_case test;
	kb2_core_thread_t root;
	kb2_core_thread_t thread;
	kb2_core_sync_event_t event;
	uint64_t cpu_mask = UINT64_C(1);
	uint64_t invalid_cpu_mask = UINT64_C(4);
	uint32_t value;
	int32_t exit_status;

	CHECK(operations &&
	      operations->header.size == sizeof(*operations) &&
	      operations->header.interface_id ==
		      KB2_CORE_RUNTIME_INTERFACE_THREAD &&
	      operations->current(binding->object, &root) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      root);
	CHECK(operations->set_name(binding->object, root, "kobox-root",
				   sizeof("kobox-root") - 1) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->set_affinity(binding->object, root, &cpu_mask, 1) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->set_priority(
		      binding->object, root,
		      KB2_CORE_RUNTIME_THREAD_PRIORITY_DEFAULT) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->interrupt(binding->object, root) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->is_interrupted(binding->object, root, &value) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      value &&
	      operations->clear_interrupt(binding->object, root) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->is_interrupted(binding->object, root, &value) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !value);
	CHECK(operations->yield(binding->object) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->create(
		      binding->object, thread_case_entry, &test, "bad-stack",
		      sizeof("bad-stack") - 1,
		      KB2_CORE_RUNTIME_THREAD_STACK_MINIMUM_BYTES - 1, NULL, 0, 0,
		      &thread) == KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);
	CHECK(operations->create(
		      binding->object, thread_case_entry, &test, "bad-mask",
		      sizeof("bad-mask") - 1, 0, &invalid_cpu_mask, 1, 0,
		      &thread) == KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);

	test = (struct thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.action = THREAD_CASE_RETURN,
		.status = KB2_CORE_RUNTIME_STATUS_OK,
	};
	CHECK(operations->create(
		      binding->object, thread_case_entry, &test, "return",
		      sizeof("return") - 1, 0, NULL, 0, 0, &thread) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->join(binding->object, thread, 0, &exit_status) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      exit_status == 123 && test.observed_thread == thread &&
	      test.status == KB2_CORE_RUNTIME_STATUS_OK);

	test = (struct thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.action = THREAD_CASE_RETURN,
		.status = KB2_CORE_RUNTIME_STATUS_OK,
	};
	CHECK(operations->create(
		      binding->object, thread_case_entry, &test, "start-parked",
		      sizeof("start-parked") - 1, 0, NULL, 0,
		      KB2_CORE_RUNTIME_THREAD_FLAG_START_PARKED, &thread) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(!__atomic_load_n(&test.phase, __ATOMIC_ACQUIRE) &&
	      operations->unpark(binding->object, thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->join(binding->object, thread, 0, &exit_status) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      exit_status == 123 && test.observed_thread == thread);

	test = (struct thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.action = THREAD_CASE_DELAYED_PARK,
		.status = KB2_CORE_RUNTIME_STATUS_OK,
	};
	CHECK(operations->create(
		      binding->object, thread_case_entry, &test, "wake",
		      sizeof("wake") - 1, 0, NULL, 0, 0, &thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !wait_for_thread_phase(&test, 1));
	CHECK(operations->wake(binding->object, thread) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	__atomic_store_n(&test.release, 1, __ATOMIC_RELEASE);
	CHECK(!wait_for_thread_phase(&test, 2));
	CHECK(operations->join(binding->object, thread,
			       monotonic_now_ns() + UINT64_C(1000000),
			       &exit_status) == KB2_CORE_RUNTIME_STATUS_TIMED_OUT);
	CHECK(operations->unpark(binding->object, thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->join(binding->object, thread, 0, &exit_status) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      test.status == KB2_CORE_RUNTIME_STATUS_OK);

	test = (struct thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.action = THREAD_CASE_PARK,
		.status = KB2_CORE_RUNTIME_STATUS_OK,
	};
	CHECK(operations->create(
		      binding->object, thread_case_entry, &test, "interrupt",
		      sizeof("interrupt") - 1, 0, NULL, 0, 0, &thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !wait_for_thread_phase(&test, 1));
	CHECK(operations->interrupt(binding->object, thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->join(binding->object, thread, 0, &exit_status) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      test.status == KB2_CORE_RUNTIME_STATUS_INTERRUPTED &&
	      test.observed);

	test = (struct thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.action = THREAD_CASE_PARK,
		.status = KB2_CORE_RUNTIME_STATUS_OK,
	};
	CHECK(operations->create(
		      binding->object, thread_case_entry, &test, "stop",
		      sizeof("stop") - 1, 0, NULL, 0, 0, &thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !wait_for_thread_phase(&test, 1));
	CHECK(operations->request_stop(binding->object, thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->stop_requested(binding->object, thread, &value) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      value &&
	      operations->join(binding->object, thread, 0, &exit_status) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      test.status == KB2_CORE_RUNTIME_STATUS_CANCELED &&
	      test.observed);

	CHECK(sync_operations->event_create(sync_binding->object, 0, &event) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	test = (struct thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.sync_operations = sync_operations,
		.sync_binding_object = sync_binding->object,
		.event = event,
		.action = THREAD_CASE_SYNC_WAIT,
		.status = KB2_CORE_RUNTIME_STATUS_OK,
	};
	CHECK(operations->create(
		      binding->object, thread_case_entry, &test, "sync-interrupt",
		      sizeof("sync-interrupt") - 1, 0, NULL, 0, 0, &thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !wait_for_thread_phase(&test, 1));
	CHECK(operations->interrupt(binding->object, thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      operations->join(binding->object, thread, 0, &exit_status) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      test.status == KB2_CORE_RUNTIME_STATUS_INTERRUPTED &&
	      test.observed &&
	      sync_operations->event_destroy(sync_binding->object, event) ==
		      KB2_CORE_RUNTIME_STATUS_OK);

	test = (struct thread_case){
		.operations = operations,
		.binding_object = binding->object,
		.action = THREAD_CASE_DETACH_WAIT,
		.status = KB2_CORE_RUNTIME_STATUS_OK,
	};
	CHECK(operations->create(
		      binding->object, thread_case_entry, &test, "detached",
		      sizeof("detached") - 1, 0, NULL, 0, 0, &thread) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !wait_for_thread_phase(&test, 1));
	CHECK(operations->detach(binding->object, thread) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	__atomic_store_n(&test.release, 1, __ATOMIC_RELEASE);
	CHECK(!wait_for_thread_phase(&test, 3));
	CHECK(test.current_matches && test.observed_thread == thread);
	return 0;
}

static int test_core_lifecycle(void)
{
	static const uint8_t arena_digest[KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE] =
		KB2_MEMORY_ARENA_SCHEMA_SHA256_BYTES;
	static const uint8_t core_digest[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE] =
		KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES;
	const int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW;
	const struct kobox_module_runtime_operations runtime_operations = {
		.size = sizeof(runtime_operations),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
		.resource_count = resource_count,
		.resource_acquire = resource_acquire,
		.resource_bind = resource_bind,
		.resource_info = resource_info,
	};
	struct kobox_module_context context = {
		.size = sizeof(context),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
		.generation = TEST_GENERATION,
		.node_id = 1,
		.resource_view = &context,
		.runtime_operations = &runtime_operations,
		.core_operations = &kobox_linux_core_directory,
		.logical_cpu_count = 2,
	};
	struct kobox_module_context other_context = context;
	struct kobox_module_context stale_context = context;
	struct kobox_module_context invalid_context = context;
	struct kobox_resource_native_handle handle;
	kb2_resource_grant_object_t object = {
		.slot_id = KOBOX_LINUX_CORE_MEMORY_SLOT_ID,
		.object_id = TEST_OBJECT_ID,
		.granted_rights = KB2_MEMORY_ARENA_REQUIRED_RIGHTS,
		.handle_count = 1,
	};
	kb2_resource_grant_slot_t slot = {
		.slot_id = KOBOX_LINUX_CORE_MEMORY_SLOT_ID,
		.resource_type = KB2_CLOSURE_RESOURCE_MEMORY,
		.state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
		.object_count = 1,
	};
	struct cache_tracker tracker = { 0 };
	struct kb2_core_binding binding = { 0 };
	struct kb2_core_binding other_binding = { 0 };
	struct kb2_core_binding cpu_binding = { 0 };
	struct kb2_core_binding other_cpu_binding = { 0 };
	struct kb2_core_binding sync_binding = { 0 };
	struct kb2_core_binding other_sync_binding = { 0 };
	struct kb2_core_binding thread_binding = { 0 };
	struct kb2_core_binding time_binding = { 0 };
	struct kb2_core_binding work_binding = { 0 };
	struct kb2_core_binding rcu_binding = { 0 };
	struct kb2_core_binding rebound_cpu_binding = { 0 };
	struct kb2_core_binding stale_binding;
	struct kb2_core_binding stale_cpu_binding;
	struct kb2_core_binding rejected = { 0 };
	const struct kb2_core_memory_operations *operations;
	const struct kb2_core_cpu_operations *cpu_operations;
	const struct kb2_core_sync_operations *sync_operations;
	const struct kb2_core_thread_operations *thread_operations;
	const struct kb2_core_time_operations *time_operations;
	const struct kb2_core_workqueue_operations *work_operations;
	const struct kb2_core_rcu_operations *rcu_operations;
	struct cpu_thread_case cpu_thread_case;
	struct thread_case quiesce_thread_case;
	struct thread_case time_quiesce_thread_case;
	struct work_callback_case work_quiesce_case;
	struct rcu_callback_case rcu_quiesce_case;
	struct rcu_callback_argument rcu_quiesce_argument;
	struct rcu_quiesce_reader_case rcu_quiesce_reader_case;
	kb2_core_thread_t quiesce_thread;
	kb2_core_thread_t time_quiesce_thread;
	kb2_core_workqueue_queue_t quiesce_queue;
	kb2_core_workqueue_work_t quiesce_work;
	kb2_core_rcu_domain_t quiesce_rcu_domain;
	kb2_core_cpu_percpu_allocation_t percpu_allocation;
	kb2_core_memory_cache_t cache;
	unsigned char wrong_digest[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE];
	unsigned char *pages;
	unsigned char *allocation;
	unsigned char *replacement;
	unsigned char *cache_object;
	unsigned char *cpu_zero;
	unsigned char *cpu_one;
	void *opaque;
	size_t total_pages;
	size_t free_pages;
	size_t usable_size;
	size_t index;
	uint32_t largest_order;
	uint32_t count;
	uint32_t online;
	uint32_t cpu_id;
	uint32_t context_class;
	uint64_t irq_state;
	uint64_t nested_irq_state;
	uint64_t quiesce_deadline;
	uint32_t work_pending;
	pthread_t cpu_thread;
	pthread_t rcu_quiesce_reader;
	int descriptor;

	memcpy(slot.interface_schema_digest, arena_digest,
	       sizeof(arena_digest));
	descriptor = memfd_create("kobox-core-arena",
				  MFD_CLOEXEC | MFD_ALLOW_SEALING);
	CHECK(descriptor >= 0 && !ftruncate(descriptor, TEST_ARENA_SIZE) &&
	      !fcntl(descriptor, F_ADD_SEALS, seals));
	handle = (struct kobox_resource_native_handle){
		.role = KB2_MEMORY_ARENA_NATIVE_HANDLE_ROLE_MEMORY,
		.handle = descriptor,
	};
	CHECK(!kobox_linux_memory_resource_import(
		      NULL, &slot, &object, &handle, 1, &memory_object,
		      &memory_operations));
	invalid_context.logical_cpu_count = 0;
	CHECK(kobox_linux_core_init(&invalid_context));
	CHECK(!kobox_linux_core_init(&context));

	memcpy(wrong_digest, core_digest, sizeof(wrong_digest));
	wrong_digest[0] ^= 1;
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_MEMORY, wrong_digest,
		      &rejected) == KB2_CORE_RUNTIME_STATUS_INTERFACE &&
	      !rejected.operations && !rejected.object);
	stale_context.generation--;
	CHECK(kobox_linux_core_directory.bind(
		      &stale_context, KB2_CORE_RUNTIME_INTERFACE_MEMORY,
		      core_digest, &rejected) == KB2_CORE_RUNTIME_STATUS_STALE);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_CPU, core_digest,
		      &rejected) == KB2_CORE_RUNTIME_STATUS_INVALID_STATE);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_MEMORY, core_digest,
		      &binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_MEMORY, core_digest,
		      &rejected) == KB2_CORE_RUNTIME_STATUS_INVALID_STATE);
	other_context.node_id = 2;
	CHECK(kobox_linux_core_directory.bind(
		      &other_context, KB2_CORE_RUNTIME_INTERFACE_MEMORY,
		      core_digest, &other_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_CPU, core_digest,
		      &cpu_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_CPU, core_digest,
		      &rejected) == KB2_CORE_RUNTIME_STATUS_INVALID_STATE);
	CHECK(kobox_linux_core_directory.bind(
		      &other_context, KB2_CORE_RUNTIME_INTERFACE_CPU, core_digest,
		      &other_cpu_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_SYNC, core_digest,
		      &sync_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &other_context, KB2_CORE_RUNTIME_INTERFACE_SYNC, core_digest,
		      &other_sync_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_THREAD, core_digest,
		      &thread_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_TIME, core_digest,
		      &time_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE, core_digest,
		      &work_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_RCU, core_digest,
		      &rcu_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.unbind(&other_context, &other_binding) ==
	      KB2_CORE_RUNTIME_STATUS_BUSY);
	sync_operations = sync_binding.operations;
	CHECK(!test_sync_operations(sync_operations, &sync_binding,
				    &other_sync_binding));
	thread_operations = thread_binding.operations;
	CHECK(!test_thread_operations(thread_operations, &thread_binding,
				      sync_operations, &sync_binding));
	time_operations = time_binding.operations;
	CHECK(!test_time_operations(time_operations, &time_binding,
				    cpu_binding.operations, &cpu_binding,
				    thread_operations, &thread_binding));
	work_operations = work_binding.operations;
	CHECK(!test_workqueue_operations(
		work_operations, &work_binding, thread_operations,
		&thread_binding, cpu_binding.operations, &cpu_binding,
		time_operations, &time_binding));
	rcu_operations = rcu_binding.operations;
	CHECK(!test_rcu_operations(
		rcu_operations, &rcu_binding, thread_operations, &thread_binding,
		cpu_binding.operations, &cpu_binding));

	cpu_operations = cpu_binding.operations;
	CHECK(cpu_operations &&
	      cpu_operations->header.size == sizeof(*cpu_operations) &&
	      cpu_operations->header.interface_id ==
		      KB2_CORE_RUNTIME_INTERFACE_CPU &&
	      !memcmp(cpu_operations->header.schema_digest, core_digest,
		      sizeof(core_digest)));
	CHECK(cpu_operations->possible_count(cpu_binding.object, &count) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      count == 2);
	CHECK(cpu_operations->online_count(cpu_binding.object, &count) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      count == 2);
	CHECK(cpu_operations->current(cpu_binding.object, &cpu_id) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_id == 0);
	CHECK(cpu_operations->is_online(cpu_binding.object, 1, &online) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      online == 1);
	CHECK(cpu_operations->is_online(cpu_binding.object, 2, &online) ==
	      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);
	CHECK(cpu_operations->context_class(cpu_binding.object, &context_class) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      context_class == KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD);

	CHECK(cpu_operations->preempt_count(cpu_binding.object, &count) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !count);
	CHECK(cpu_operations->preempt_enable(cpu_binding.object) ==
	      KB2_CORE_RUNTIME_STATUS_INVALID_STATE);
	CHECK(cpu_operations->preempt_disable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->preempt_disable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->preempt_count(cpu_binding.object, &count) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      count == 2);
	CHECK(kobox_linux_core_directory.unbind(&context, &cpu_binding) ==
	      KB2_CORE_RUNTIME_STATUS_BUSY);
	CHECK(cpu_operations->preempt_enable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->preempt_enable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(cpu_operations->migrate_disable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->migrate_count(cpu_binding.object, &count) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      count == 1 &&
	      cpu_operations->migrate_enable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(cpu_operations->local_irq_disable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->local_irq_enable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->local_irq_enable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_STATE);
	CHECK(cpu_operations->local_irq_save(cpu_binding.object, &irq_state) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->local_irq_save(
		      cpu_binding.object, &nested_irq_state) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(cpu_operations->local_irq_restore(cpu_binding.object, irq_state) ==
	      KB2_CORE_RUNTIME_STATUS_INVALID_STATE);
	CHECK(cpu_operations->local_irq_restore(
		      other_cpu_binding.object, nested_irq_state) ==
	      KB2_CORE_RUNTIME_STATUS_INVALID_STATE);
	CHECK(cpu_operations->local_irq_restore(
		      cpu_binding.object, nested_irq_state) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->local_irq_restore(cpu_binding.object, irq_state) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(cpu_operations->bottom_half_disable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->bottom_half_enable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      cpu_operations->bottom_half_enable(cpu_binding.object) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_STATE);

	cpu_thread_case = (struct cpu_thread_case){
		.operations = cpu_operations,
		.binding_object = cpu_binding.object,
	};
	CHECK(!pthread_create(&cpu_thread, NULL, cpu_thread_worker,
		      &cpu_thread_case));
	CHECK(!pthread_join(cpu_thread, NULL) && !cpu_thread_case.status);

	CHECK(cpu_operations->percpu_allocate(
		      cpu_binding.object, 33, 64,
		      KB2_CORE_RUNTIME_CPU_PERCPU_FLAG_ZERO,
		      &percpu_allocation) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(cpu_operations->percpu_address(
		      cpu_binding.object, percpu_allocation, 0, &opaque) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	cpu_zero = opaque;
	CHECK(cpu_operations->percpu_address(
		      cpu_binding.object, percpu_allocation, 1, &opaque) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	cpu_one = opaque;
	CHECK(cpu_zero != cpu_one && !((uintptr_t)cpu_zero & 63) &&
	      !((uintptr_t)cpu_one & 63));
	for (index = 0; index < 33; index++)
		CHECK(!cpu_zero[index] && !cpu_one[index]);
	cpu_zero[0] = 0x12;
	cpu_one[0] = 0x34;
	CHECK(cpu_zero[0] == 0x12 && cpu_one[0] == 0x34);
	CHECK(cpu_operations->percpu_address(
		      cpu_binding.object, percpu_allocation, 2, &opaque) ==
	      KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT);
	CHECK(cpu_operations->percpu_address(
		      other_cpu_binding.object, percpu_allocation, 0, &opaque) ==
	      KB2_CORE_RUNTIME_STATUS_OWNER);
	CHECK(kobox_linux_core_directory.unbind(&context, &cpu_binding) ==
	      KB2_CORE_RUNTIME_STATUS_BUSY);

	operations = binding.operations;
	CHECK(operations && operations->header.size == sizeof(*operations) &&
	      operations->header.interface_id ==
		      KB2_CORE_RUNTIME_INTERFACE_MEMORY &&
	      !memcmp(operations->header.schema_digest, core_digest,
		      sizeof(core_digest)) &&
	      operations->statistics(binding.object, &total_pages, &free_pages,
				     &largest_order) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      total_pages > 32 && free_pages < total_pages && largest_order > 3);

	CHECK(operations->page_allocate(
		      binding.object, 3, KB2_CORE_RUNTIME_MEMORY_FLAG_ZERO,
		      &opaque) == KB2_CORE_RUNTIME_STATUS_OK);
	pages = opaque;
	for (index = 0; index < 8 * 4096; index++)
		CHECK(!pages[index]);
	CHECK(operations->page_release(other_binding.object, pages, 3) ==
	      KB2_CORE_RUNTIME_STATUS_OWNER);
	CHECK(kobox_linux_core_directory.unbind(&context, &binding) ==
	      KB2_CORE_RUNTIME_STATUS_BUSY);
	CHECK(kobox_linux_core_directory.unbind(&other_context, &binding) ==
	      KB2_CORE_RUNTIME_STATUS_OWNER);

	CHECK(operations->allocate(
		      binding.object, 257, 64, KB2_CORE_RUNTIME_MEMORY_FLAG_ZERO,
		      &opaque) == KB2_CORE_RUNTIME_STATUS_OK);
	allocation = opaque;
	CHECK(!((uintptr_t)allocation & 63) &&
	      operations->usable_size(binding.object, allocation, &usable_size) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      usable_size >= 257);
	for (index = 0; index < 257; index++) {
		CHECK(!allocation[index]);
		allocation[index] = (unsigned char)(index ^ 0x5a);
	}
	CHECK(operations->reallocate(
		      binding.object, allocation, 1025, 128,
		      KB2_CORE_RUNTIME_MEMORY_FLAG_ZERO,
		      &opaque) == KB2_CORE_RUNTIME_STATUS_OK);
	replacement = opaque;
	CHECK(!((uintptr_t)replacement & 127));
	for (index = 0; index < 257; index++)
		CHECK(replacement[index] == (unsigned char)(index ^ 0x5a));
	for (index = 257; index < 1025; index++)
		CHECK(!replacement[index]);

	CHECK(operations->cache_create(
		      binding.object, "test-cache", sizeof("test-cache") - 1, 96,
		      32, KB2_CORE_RUNTIME_MEMORY_FLAG_ZERO, cache_constructor,
		      cache_destructor, &tracker, &cache) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->cache_allocate(binding.object, cache, 0, &opaque) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	cache_object = opaque;
	CHECK(cache_object[0] == 0xa5 && tracker.constructors == 1 &&
	      operations->cache_destroy(binding.object, cache) ==
		      KB2_CORE_RUNTIME_STATUS_BUSY);
	CHECK(operations->cache_release(binding.object, cache, cache_object) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(tracker.destructors == 1 &&
	      operations->cache_destroy(binding.object, cache) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->release(binding.object, replacement) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->page_release(binding.object, pages, 3) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(cpu_operations->percpu_release(cpu_binding.object,
				     percpu_allocation) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.unbind(&context, &rcu_binding) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      kobox_linux_core_directory.unbind(&context, &work_binding) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      kobox_linux_core_directory.unbind(&context, &time_binding) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	for (index = 0; index < 1000000; index++) {
		kb2_core_status_t unbind_status =
			kobox_linux_core_directory.unbind(&context,
						  &thread_binding);

		if (unbind_status == KB2_CORE_RUNTIME_STATUS_OK)
			break;
		CHECK(unbind_status == KB2_CORE_RUNTIME_STATUS_BUSY);
		sched_yield();
	}
	CHECK(index != 1000000);
	CHECK(kobox_linux_core_directory.unbind(&context, &sync_binding) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.unbind(
		      &other_context, &other_sync_binding) ==
	      KB2_CORE_RUNTIME_STATUS_OK);

	stale_cpu_binding = cpu_binding;
	CHECK(kobox_linux_core_directory.unbind(&context, &cpu_binding) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(cpu_operations->possible_count(
		      stale_cpu_binding.object, &count) ==
	      KB2_CORE_RUNTIME_STATUS_STALE);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_CPU, core_digest,
		      &rebound_cpu_binding) == KB2_CORE_RUNTIME_STATUS_OK &&
	      rebound_cpu_binding.object != stale_cpu_binding.object);
	CHECK(cpu_operations->possible_count(
		      stale_cpu_binding.object, &count) ==
	      KB2_CORE_RUNTIME_STATUS_STALE);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_SYNC, core_digest,
		      &sync_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_THREAD, core_digest,
		      &thread_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	thread_operations = thread_binding.operations;
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_TIME, core_digest,
		      &time_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	time_operations = time_binding.operations;
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE, core_digest,
		      &work_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	work_operations = work_binding.operations;
	CHECK(kobox_linux_core_directory.bind(
		      &context, KB2_CORE_RUNTIME_INTERFACE_RCU, core_digest,
		      &rcu_binding) == KB2_CORE_RUNTIME_STATUS_OK);
	rcu_operations = rcu_binding.operations;
	{
		uint64_t time_now;

		CHECK(time_operations->monotonic_ns(time_binding.object,
						    &time_now) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
		time_quiesce_thread_case = (struct thread_case){
			.operations = thread_operations,
			.binding_object = thread_binding.object,
			.time_operations = time_operations,
			.time_binding_object = time_binding.object,
			.time_deadline_ns = time_now + UINT64_C(10000000000),
			.action = THREAD_CASE_TIME_SLEEP,
		};
		quiesce_deadline = time_now + UINT64_C(10000000000);
	}
	CHECK(work_operations->queue_create(
		      work_binding.object, "freezable", sizeof("freezable") - 1,
		      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND |
			      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_FREEZABLE,
		      1, &quiesce_queue) == KB2_CORE_RUNTIME_STATUS_OK);
	work_quiesce_case = (struct work_callback_case){
		.operations = work_operations,
		.binding_object = work_binding.object,
		.thread_operations = thread_operations,
		.thread_binding_object = thread_binding.object,
		.cpu_operations = rebound_cpu_binding.operations,
		.cpu_binding_object = rebound_cpu_binding.object,
		.queue = quiesce_queue,
	};
	CHECK(work_operations->work_create(
		      work_binding.object, work_test_callback, &work_quiesce_case,
		      &quiesce_work) == KB2_CORE_RUNTIME_STATUS_OK &&
	      work_operations->submit_at(
		      work_binding.object, quiesce_queue, quiesce_work,
		      KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY, quiesce_deadline,
		      &work_pending) == KB2_CORE_RUNTIME_STATUS_OK &&
	      work_pending);
	rcu_quiesce_case = (struct rcu_callback_case){
		.operations = rcu_operations,
		.binding_object = rcu_binding.object,
		.thread_operations = thread_operations,
		.thread_binding_object = thread_binding.object,
		.cpu_operations = rebound_cpu_binding.operations,
		.cpu_binding_object = rebound_cpu_binding.object,
	};
	rcu_quiesce_argument = (struct rcu_callback_argument){
		.test = &rcu_quiesce_case,
		.value = 1,
	};
	CHECK(rcu_operations->default_domain(
		      rcu_binding.object, &quiesce_rcu_domain) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	rcu_quiesce_case.domain = quiesce_rcu_domain;
	rcu_quiesce_reader_case = (struct rcu_quiesce_reader_case){
		.operations = rcu_operations,
		.binding_object = rcu_binding.object,
		.domain = quiesce_rcu_domain,
	};
	CHECK(!pthread_create(&rcu_quiesce_reader, NULL,
			      rcu_quiesce_reader_worker,
			      &rcu_quiesce_reader_case));
	while (!__atomic_load_n(&rcu_quiesce_reader_case.started,
				__ATOMIC_ACQUIRE))
		sched_yield();
	CHECK(rcu_quiesce_reader_case.status == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(rcu_operations->call(
		      rcu_binding.object, quiesce_rcu_domain, rcu_test_callback,
		      &rcu_quiesce_argument) == KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(thread_operations->create(
		      thread_binding.object, thread_case_entry,
		      &time_quiesce_thread_case, "time-quiesce",
		      sizeof("time-quiesce") - 1, 0, NULL, 0, 0,
		      &time_quiesce_thread) == KB2_CORE_RUNTIME_STATUS_OK &&
	      !wait_for_thread_phase(&time_quiesce_thread_case, 1));
	quiesce_thread_case = (struct thread_case){
		.operations = thread_operations,
		.binding_object = thread_binding.object,
		.action = THREAD_CASE_PARK,
		.status = KB2_CORE_RUNTIME_STATUS_OK,
	};
	CHECK(thread_operations->create(
		      thread_binding.object, thread_case_entry,
		      &quiesce_thread_case, "quiesce",
		      sizeof("quiesce") - 1, 0, NULL, 0, 0,
		      &quiesce_thread) == KB2_CORE_RUNTIME_STATUS_OK &&
	      !wait_for_thread_phase(&quiesce_thread_case, 1));
	CHECK(!test_sync_quiesce(&context, &sync_binding));
	CHECK(quiesce_thread_case.observed_thread == quiesce_thread &&
	      quiesce_thread_case.status == KB2_CORE_RUNTIME_STATUS_CANCELED &&
	      quiesce_thread_case.observed &&
	      time_quiesce_thread_case.observed_thread == time_quiesce_thread &&
	      time_quiesce_thread_case.status ==
		      KB2_CORE_RUNTIME_STATUS_CANCELED &&
	      __atomic_load_n(&time_quiesce_thread_case.phase,
			      __ATOMIC_ACQUIRE) == 3);
	CHECK(!work_quiesce_case.calls &&
	      work_operations->is_pending(work_binding.object, quiesce_work,
					  &work_pending) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      !work_pending &&
	      work_operations->work_destroy(work_binding.object, quiesce_work) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      work_operations->queue_destroy(work_binding.object,
					     quiesce_queue) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(!pthread_join(rcu_quiesce_reader, NULL) &&
	      rcu_quiesce_reader_case.status == KB2_CORE_RUNTIME_STATUS_OK &&
	      rcu_quiesce_case.calls == 1 &&
	      rcu_quiesce_case.order[0] == 1 &&
	      rcu_operations->default_domain(
		      rcu_binding.object, &quiesce_rcu_domain) ==
		      KB2_CORE_RUNTIME_STATUS_INVALID_STATE &&
	      kobox_linux_core_directory.unbind(&context, &rcu_binding) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      kobox_linux_core_directory.unbind(&context, &work_binding) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      kobox_linux_core_directory.unbind(&context, &time_binding) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      kobox_linux_core_directory.unbind(&context, &thread_binding) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      kobox_linux_core_directory.unbind(&context, &sync_binding) ==
		      KB2_CORE_RUNTIME_STATUS_OK &&
	      kobox_linux_core_directory.unbind(
		      &context, &rebound_cpu_binding) ==
		      KB2_CORE_RUNTIME_STATUS_OK);
	stale_binding = binding;
	CHECK(kobox_linux_core_directory.unbind(&context, &binding) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(operations->statistics(stale_binding.object, &total_pages,
				     &free_pages, &largest_order) ==
	      KB2_CORE_RUNTIME_STATUS_STALE);
	CHECK(kobox_linux_core_directory.unbind(
		      &other_context, &other_cpu_binding) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(kobox_linux_core_directory.unbind(&other_context, &other_binding) ==
	      KB2_CORE_RUNTIME_STATUS_OK);
	CHECK(!kobox_linux_core_cleanup(&context));

	kobox_linux_memory_resource_release(NULL, memory_object);
	memory_object = NULL;
	memory_operations = NULL;
	close(descriptor);
	return 0;
}

int main(void)
{
	return test_core_lifecycle() ? EXIT_FAILURE : EXIT_SUCCESS;
}
