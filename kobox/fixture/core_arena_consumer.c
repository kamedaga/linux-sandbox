// SPDX-License-Identifier: GPL-2.0-only

#include "../provider/core_lifecycle.h"

#include <linux/compiler_attributes.h>

#include <stddef.h>
#include <stdint.h>

#define ARENA_FIXTURE_NODE_ID 2u
#define ARENA_FIXTURE_ORDER 3u
#define ARENA_FIXTURE_SIZE ((size_t)4096u << ARENA_FIXTURE_ORDER)
#define ARENA_FIXTURE_RESULT UINT64_C(0x6b62326172656e61)

static const char arena_fixture_license[]
	__section(".modinfo") __used __aligned(1) = "license=GPL";
static const char arena_fixture_name[]
	__section(".modinfo") __used __aligned(1) =
		"name=core_arena_consumer";
static const char arena_fixture_dependency[]
	__section(".modinfo") __used __aligned(1) = "depends=";
static const char arena_fixture_interface[]
	__section(".modinfo") __used __aligned(1) = "kobox_interface=dev";

static const struct kobox_module_context *fixture_context;
static const struct kb2_core_memory_operations *fixture_memory_operations;
static const struct kb2_core_cpu_operations *fixture_cpu_operations;
static const struct kb2_core_sync_operations *fixture_sync_operations;
static const struct kb2_core_thread_operations *fixture_thread_operations;
static const struct kb2_core_time_operations *fixture_time_operations;
static const struct kb2_core_workqueue_operations *fixture_work_operations;
static const struct kb2_core_rcu_operations *fixture_rcu_operations;
static struct kb2_core_binding fixture_memory_binding;
static struct kb2_core_binding fixture_cpu_binding;
static struct kb2_core_binding fixture_sync_binding;
static struct kb2_core_binding fixture_thread_binding;
static struct kb2_core_binding fixture_time_binding;
static struct kb2_core_binding fixture_work_binding;
static struct kb2_core_binding fixture_rcu_binding;
static kb2_core_cpu_percpu_allocation_t fixture_percpu_allocation;
static unsigned char *fixture_allocation;
static unsigned char *fixture_cpu_zero;
static unsigned char *fixture_cpu_one;
static uint32_t fixture_thread_ran;
static uint32_t fixture_timer_ran;
static uint32_t fixture_work_ran;
static uint32_t fixture_rcu_ran;

static int bytes_equal(const uint8_t *left, const uint8_t *right,
		       size_t length)
{
	size_t index;

	for (index = 0; index < length; index++) {
		if (left[index] != right[index])
			return 0;
	}
	return 1;
}

static int directory_valid(const struct kobox_module_context *context)
{
	static const uint8_t identity[KB2_CORE_RUNTIME_ABI_IDENTITY_SIZE] =
		KB2_CORE_RUNTIME_ABI_IDENTITY_BYTES;
	static const uint8_t digest[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE] =
		KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES;
	const struct kb2_core_directory *directory;

	if (!context || context->size != sizeof(*context) ||
	    context->node_id != ARENA_FIXTURE_NODE_ID || !context->generation ||
	    context->reserved || context->logical_cpu_count != 2 ||
	    context->reserved2 || !context->core_operations)
		return 0;
	directory = context->core_operations;
	return directory->size == sizeof(*directory) && directory->bind &&
	       directory->unbind &&
	       bytes_equal(directory->identity, identity, sizeof(identity)) &&
	       bytes_equal(directory->schema_digest, digest, sizeof(digest));
}

static int allocation_valid(void)
{
	size_t index;

	if (!fixture_allocation)
		return 0;
	for (index = 0; index < ARENA_FIXTURE_SIZE; index++) {
		if (fixture_allocation[index] != (unsigned char)(index ^ 0xa5u))
			return 0;
	}
	return 1;
}

static int percpu_valid(void)
{
	return fixture_percpu_allocation && fixture_cpu_zero &&
	       fixture_cpu_one && fixture_cpu_zero != fixture_cpu_one &&
	       fixture_cpu_zero[0] == 0x12 && fixture_cpu_one[0] == 0x34;
}

static int32_t fixture_thread_entry(void *argument)
{
	kb2_core_thread_t current;

	if (argument != &fixture_thread_ran || !fixture_thread_operations ||
	    fixture_thread_operations->current(fixture_thread_binding.object,
					       &current) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    !current)
		return -1;
	fixture_thread_ran = 1;
	return 73;
}

static void fixture_timer_callback(kb2_core_time_timer_t timer,
				   void *argument,
				   uint64_t expiration_count)
{
	uint32_t context_class;

	if (timer && argument == &fixture_timer_ran && expiration_count == 1 &&
	    fixture_cpu_operations->context_class(
		    fixture_cpu_binding.object, &context_class) ==
		    KB2_CORE_RUNTIME_STATUS_OK &&
	    context_class == KB2_CORE_RUNTIME_CPU_CONTEXT_SOFTIRQ)
		fixture_timer_ran = 1;
}

static void fixture_work_callback(kb2_core_workqueue_work_t work,
				  void *argument)
{
	kb2_core_thread_t current;
	uint32_t context_class;

	if (work && argument == &fixture_work_ran &&
	    fixture_thread_operations->current(fixture_thread_binding.object,
					       &current) ==
		    KB2_CORE_RUNTIME_STATUS_OK &&
	    current &&
	    fixture_cpu_operations->context_class(
		    fixture_cpu_binding.object, &context_class) ==
		    KB2_CORE_RUNTIME_STATUS_OK &&
	    context_class == KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD)
		fixture_work_ran = 1;
}

static void fixture_rcu_callback(void *argument)
{
	kb2_core_thread_t current;
	uint32_t context_class;

	if (argument == &fixture_rcu_ran &&
	    fixture_thread_operations->current(fixture_thread_binding.object,
					       &current) ==
		    KB2_CORE_RUNTIME_STATUS_OK &&
	    current &&
	    fixture_cpu_operations->context_class(
		    fixture_cpu_binding.object, &context_class) ==
		    KB2_CORE_RUNTIME_STATUS_OK &&
	    context_class == KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD)
		fixture_rcu_ran = 1;
}

__attribute__((visibility("default")))
int kobox_arena_fixture_init(const struct kobox_module_context *context)
{
	static const uint8_t digest[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE] =
		KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES;
	const struct kb2_core_directory *directory;
	void *address;
	size_t index;
	uint32_t count;
	uint32_t cpu_id;
	kb2_core_thread_t thread;
	kb2_core_time_timer_t timer;
	kb2_core_workqueue_queue_t queue;
	kb2_core_workqueue_work_t work;
	kb2_core_rcu_domain_t rcu_domain;
	kb2_core_rcu_read_token_t rcu_token;
	uint64_t now;
	uint64_t limit;
	int32_t exit_status;
	uint32_t queued;

	timer = NULL;
	queue = NULL;
	work = NULL;
	rcu_domain = NULL;
	rcu_token = NULL;

	if (!directory_valid(context) || fixture_context || fixture_allocation ||
	    fixture_percpu_allocation)
		return -1;
	directory = context->core_operations;
	if (directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_MEMORY, digest,
			    &fixture_memory_binding) != KB2_CORE_RUNTIME_STATUS_OK)
		return -1;
	fixture_memory_operations = fixture_memory_binding.operations;
	if (!fixture_memory_operations ||
	    fixture_memory_operations->header.size !=
		    sizeof(*fixture_memory_operations) ||
	    fixture_memory_operations->header.interface_id !=
		    KB2_CORE_RUNTIME_INTERFACE_MEMORY ||
	    !bytes_equal(fixture_memory_operations->header.schema_digest, digest,
			 sizeof(digest)) ||
	    fixture_memory_operations->page_allocate(
		    fixture_memory_binding.object, ARENA_FIXTURE_ORDER, 0,
		    (void **)&fixture_allocation) != KB2_CORE_RUNTIME_STATUS_OK) {
		goto release_memory_binding;
	}
	for (index = 0; index < ARENA_FIXTURE_SIZE; index++)
		fixture_allocation[index] = (unsigned char)(index ^ 0xa5u);
	if (directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_CPU, digest,
			    &fixture_cpu_binding) != KB2_CORE_RUNTIME_STATUS_OK)
		goto release_page;
	fixture_cpu_operations = fixture_cpu_binding.operations;
	if (!fixture_cpu_operations ||
	    fixture_cpu_operations->header.size !=
		    sizeof(*fixture_cpu_operations) ||
	    fixture_cpu_operations->header.interface_id !=
		    KB2_CORE_RUNTIME_INTERFACE_CPU ||
	    !bytes_equal(fixture_cpu_operations->header.schema_digest, digest,
			 sizeof(digest)) ||
	    fixture_cpu_operations->possible_count(
		    fixture_cpu_binding.object, &count) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    count != 2 ||
	    fixture_cpu_operations->online_count(
		    fixture_cpu_binding.object, &count) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    count != 2 ||
	    fixture_cpu_operations->current(fixture_cpu_binding.object,
					    &cpu_id) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    cpu_id ||
	    fixture_cpu_operations->percpu_allocate(
		    fixture_cpu_binding.object, 32, 64,
		    KB2_CORE_RUNTIME_CPU_PERCPU_FLAG_ZERO,
		    &fixture_percpu_allocation) != KB2_CORE_RUNTIME_STATUS_OK)
		goto release_cpu_binding;
	if (fixture_cpu_operations->percpu_address(
		    fixture_cpu_binding.object, fixture_percpu_allocation, 0,
		    &address) != KB2_CORE_RUNTIME_STATUS_OK)
		goto release_percpu;
	fixture_cpu_zero = address;
	if (fixture_cpu_operations->percpu_address(
		    fixture_cpu_binding.object, fixture_percpu_allocation, 1,
		    &address) != KB2_CORE_RUNTIME_STATUS_OK)
		goto release_percpu;
	fixture_cpu_one = address;
	if (((uintptr_t)fixture_cpu_zero & 63) ||
	    ((uintptr_t)fixture_cpu_one & 63) || fixture_cpu_zero[0] ||
	    fixture_cpu_one[0])
		goto release_percpu;
	fixture_cpu_zero[0] = 0x12;
	fixture_cpu_one[0] = 0x34;
	if (directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_SYNC, digest,
			    &fixture_sync_binding) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		goto release_percpu;
	fixture_sync_operations = fixture_sync_binding.operations;
	if (!fixture_sync_operations ||
	    fixture_sync_operations->header.size !=
		    sizeof(*fixture_sync_operations) ||
	    directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_THREAD, digest,
			    &fixture_thread_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto release_sync_binding;
	fixture_thread_operations = fixture_thread_binding.operations;
	if (!fixture_thread_operations ||
	    fixture_thread_operations->header.size !=
		    sizeof(*fixture_thread_operations) ||
	    fixture_thread_operations->create(
		    fixture_thread_binding.object, fixture_thread_entry,
		    &fixture_thread_ran, "arena-fixture",
		    sizeof("arena-fixture") - 1, 0, NULL, 0, 0, &thread) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_thread_operations->join(
		    fixture_thread_binding.object, thread, 0, &exit_status) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    exit_status != 73 || !fixture_thread_ran)
		goto release_thread_binding;
	if (directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_TIME, digest,
			    &fixture_time_binding) != KB2_CORE_RUNTIME_STATUS_OK)
		goto release_thread_binding;
	fixture_time_operations = fixture_time_binding.operations;
	if (!fixture_time_operations ||
	    fixture_time_operations->header.size !=
		    sizeof(*fixture_time_operations) ||
	    fixture_time_operations->timer_create(
		    fixture_time_binding.object,
		    KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC,
		    KB2_CORE_RUNTIME_TIME_TIMER_CONTEXT_ATOMIC,
		    fixture_timer_callback, &fixture_timer_ran,
		    KB2_CORE_RUNTIME_TIME_TIMER_FLAG_PINNED, &timer) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_time_operations->monotonic_ns(fixture_time_binding.object,
						  &now) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_time_operations->timer_arm(
		    fixture_time_binding.object, timer,
		    now + UINT64_C(2000000), 0) != KB2_CORE_RUNTIME_STATUS_OK)
		goto release_time_binding;
	limit = now + UINT64_C(1000000000);
	while (!fixture_timer_ran) {
		if (fixture_thread_operations->yield(
			    fixture_thread_binding.object) !=
			    KB2_CORE_RUNTIME_STATUS_OK ||
		    fixture_time_operations->monotonic_ns(
			    fixture_time_binding.object, &now) !=
			    KB2_CORE_RUNTIME_STATUS_OK ||
		    now >= limit)
			goto destroy_timer;
	}
	if (fixture_time_operations->timer_destroy(
		    fixture_time_binding.object, timer) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		goto release_time_binding;
	timer = NULL;
	if (directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE,
			    digest, &fixture_work_binding) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		goto release_time_binding;
	fixture_work_operations = fixture_work_binding.operations;
	if (!fixture_work_operations ||
	    fixture_work_operations->header.size !=
		    sizeof(*fixture_work_operations) ||
	    fixture_work_operations->queue_create(
		    fixture_work_binding.object, "arena-work",
		    sizeof("arena-work") - 1, 0, 1, &queue) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_work_operations->work_create(
		    fixture_work_binding.object, fixture_work_callback,
		    &fixture_work_ran, &work) != KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_work_operations->submit(
		    fixture_work_binding.object, queue, work, 0, &queued) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    !queued ||
	    fixture_work_operations->flush_work(
		    fixture_work_binding.object, work) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    !fixture_work_ran ||
	    fixture_work_operations->work_destroy(
		    fixture_work_binding.object, work) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto release_work_objects;
	work = NULL;
	if (fixture_work_operations->queue_destroy(
		    fixture_work_binding.object, queue) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		goto release_work_objects;
	queue = NULL;
	if (directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_RCU, digest,
			    &fixture_rcu_binding) != KB2_CORE_RUNTIME_STATUS_OK)
		goto release_work_objects;
	fixture_rcu_operations = fixture_rcu_binding.operations;
	if (!fixture_rcu_operations ||
	    fixture_rcu_operations->header.size !=
		    sizeof(*fixture_rcu_operations) ||
	    fixture_rcu_operations->default_domain(
		    fixture_rcu_binding.object, &rcu_domain) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_rcu_operations->read_lock(
		    fixture_rcu_binding.object, rcu_domain, &rcu_token) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_rcu_operations->call(
		    fixture_rcu_binding.object, rcu_domain,
		    fixture_rcu_callback, &fixture_rcu_ran) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_rcu_operations->read_unlock(
		    fixture_rcu_binding.object, rcu_domain, rcu_token) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_rcu_operations->barrier(
		    fixture_rcu_binding.object, rcu_domain) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    !fixture_rcu_ran)
		goto release_rcu_binding;
	rcu_token = NULL;
	fixture_context = context;
	return 0;

release_rcu_binding:
	if (rcu_token)
		(void)fixture_rcu_operations->read_unlock(
			fixture_rcu_binding.object, rcu_domain, rcu_token);
	fixture_rcu_operations = NULL;
	(void)directory->unbind(context, &fixture_rcu_binding);

release_work_objects:
	if (work)
		(void)fixture_work_operations->work_destroy(
			fixture_work_binding.object, work);
	if (queue)
		(void)fixture_work_operations->queue_destroy(
			fixture_work_binding.object, queue);
	fixture_work_operations = NULL;
	(void)directory->unbind(context, &fixture_work_binding);

destroy_timer:
	(void)fixture_time_operations->timer_destroy(
		fixture_time_binding.object, timer);
	timer = NULL;
release_time_binding:
	if (timer)
		(void)fixture_time_operations->timer_destroy(
			fixture_time_binding.object, timer);
	fixture_time_operations = NULL;
	(void)directory->unbind(context, &fixture_time_binding);

release_thread_binding:
	fixture_thread_operations = NULL;
	(void)directory->unbind(context, &fixture_thread_binding);
release_sync_binding:
	fixture_sync_operations = NULL;
	(void)directory->unbind(context, &fixture_sync_binding);
release_percpu:
	(void)fixture_cpu_operations->percpu_release(
		fixture_cpu_binding.object, fixture_percpu_allocation);
	fixture_percpu_allocation = NULL;
	fixture_cpu_zero = NULL;
	fixture_cpu_one = NULL;
release_cpu_binding:
	(void)directory->unbind(context, &fixture_cpu_binding);
	fixture_cpu_operations = NULL;
release_page:
	(void)fixture_memory_operations->page_release(
		fixture_memory_binding.object, fixture_allocation,
		ARENA_FIXTURE_ORDER);
	fixture_allocation = NULL;
release_memory_binding:
	(void)directory->unbind(context, &fixture_memory_binding);
	fixture_memory_operations = NULL;
	return -1;
}

__attribute__((visibility("default")))
int kobox_arena_fixture_run(uint64_t *result_out)
{
	if (!result_out || !allocation_valid() || !percpu_valid() ||
	    !fixture_thread_ran || !fixture_timer_ran || !fixture_work_ran ||
	    !fixture_rcu_ran)
		return -1;
	*result_out = ARENA_FIXTURE_RESULT;
	return 0;
}

__attribute__((visibility("default")))
int kobox_arena_fixture_quiesce(const struct kobox_module_context *context)
{
	return context == fixture_context && allocation_valid() && percpu_valid() &&
		       fixture_thread_ran && fixture_timer_ran && fixture_work_ran &&
		       fixture_rcu_ran ?
		       0 :
		       -1;
}

__attribute__((visibility("default")))
int kobox_arena_fixture_cleanup(const struct kobox_module_context *context)
{
	const struct kb2_core_directory *directory;

	if (context != fixture_context || !allocation_valid() || !percpu_valid() ||
	    !fixture_thread_ran || !fixture_timer_ran || !fixture_work_ran ||
	    !fixture_rcu_ran)
		return -1;
	directory = context->core_operations;
	if (directory->unbind(context, &fixture_rcu_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    directory->unbind(context, &fixture_work_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    directory->unbind(context, &fixture_time_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    directory->unbind(context, &fixture_thread_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    directory->unbind(context, &fixture_sync_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    fixture_cpu_operations->percpu_release(
		    fixture_cpu_binding.object, fixture_percpu_allocation) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    directory->unbind(context, &fixture_cpu_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		return -1;
	fixture_percpu_allocation = NULL;
	fixture_cpu_zero = NULL;
	fixture_cpu_one = NULL;
	fixture_cpu_operations = NULL;
	fixture_sync_operations = NULL;
	fixture_thread_operations = NULL;
	fixture_time_operations = NULL;
	fixture_work_operations = NULL;
	fixture_rcu_operations = NULL;
	fixture_thread_ran = 0;
	fixture_timer_ran = 0;
	fixture_work_ran = 0;
	fixture_rcu_ran = 0;
#ifndef KOBOX_ARENA_FIXTURE_LEAK
	if (fixture_memory_operations->page_release(
		    fixture_memory_binding.object, fixture_allocation,
		    ARENA_FIXTURE_ORDER) != KB2_CORE_RUNTIME_STATUS_OK)
		return -1;
#endif
	fixture_allocation = NULL;
	fixture_memory_operations = NULL;
	fixture_context = NULL;
	return directory->unbind(context, &fixture_memory_binding) ==
		       KB2_CORE_RUNTIME_STATUS_OK ?
		       0 :
		       -1;
}
