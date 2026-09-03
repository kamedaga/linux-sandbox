// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "core_lifecycle.h"

#include "arena.h"
#include "lifecycle.h"
#include "../runtime/memory_resource_interface.h"

#include <kobox2/closure_layout.h>
#include <kobox2/memory_arena_layout.h>

#include <asm/unistd.h>
#include <limits.h>
#include <linux/errno.h>
#include <linux/eventfd.h>
#include <linux/futex.h>
#include <linux/poll.h>
#include <linux/prctl.h>
#include <linux/resource.h>
#include <linux/time_types.h>
#include <linux/timerfd.h>
#include <pthread.h>
#include <time.h>

#define CORE_BINDING_MAGIC UINT64_C(0x6b623262696e6467)
#define CORE_ALLOCATION_MAGIC UINT64_C(0x6b6232616c6c6f63)
#define CORE_CACHE_MAGIC UINT64_C(0x6b62326361636865)
#define CORE_PERCPU_MAGIC UINT64_C(0x6b62327065726370)
#define CORE_SYNC_SPIN_MAGIC UINT64_C(0x6b62327370696e31)
#define CORE_SYNC_MUTEX_MAGIC UINT64_C(0x6b62326d75747831)
#define CORE_SYNC_RWLOCK_MAGIC UINT64_C(0x6b623272776c6b31)
#define CORE_SYNC_SEMAPHORE_MAGIC UINT64_C(0x6b623273656d6131)
#define CORE_SYNC_EVENT_MAGIC UINT64_C(0x6b623265766e7431)
#define CORE_SYNC_COMPLETION_MAGIC UINT64_C(0x6b6232636f6d7031)
#define CORE_THREAD_MAGIC UINT64_C(0x6b62327468726431)
#define CORE_TIMER_MAGIC UINT64_C(0x6b623274696d6572)
#define CORE_WORKQUEUE_MAGIC UINT64_C(0x6b62327771756531)
#define CORE_WORK_MAGIC UINT64_C(0x6b6232776f726b31)
#define CORE_RCU_DOMAIN_MAGIC UINT64_C(0x6b62327263756431)
#define CORE_RCU_TOKEN_MAGIC UINT64_C(0x6b62327263757431)
#define CORE_RCU_CALLBACK_MAGIC UINT64_C(0x6b62327263756331)
#define CORE_PAGE_SIZE 4096u
#define CORE_NATIVE_CPU_LIMIT 8192u
#define CORE_NATIVE_CPU_WORDS (CORE_NATIVE_CPU_LIMIT / 64u)

enum core_state {
	CORE_UNBOUND = 0,
	CORE_ACTIVE,
	CORE_QUIESCED,
};

enum core_allocation_kind {
	CORE_ALLOCATION_PAGE = 1,
	CORE_ALLOCATION_GENERAL,
	CORE_ALLOCATION_CACHE,
	CORE_ALLOCATION_CACHE_OBJECT,
	CORE_ALLOCATION_CPU_LOCAL,
	CORE_ALLOCATION_PERCPU,
	CORE_ALLOCATION_SYNC_OBJECT,
	CORE_ALLOCATION_SYNC_READER,
	CORE_ALLOCATION_THREAD,
	CORE_ALLOCATION_TIMER,
	CORE_ALLOCATION_WORKQUEUE,
	CORE_ALLOCATION_WORK,
	CORE_ALLOCATION_WORK_INTERNAL,
	CORE_ALLOCATION_RCU_DOMAIN,
	CORE_ALLOCATION_RCU_TOKEN,
	CORE_ALLOCATION_RCU_CALLBACK,
};

enum core_thread_state {
	CORE_THREAD_STARTING = 1,
	CORE_THREAD_RUNNING,
	CORE_THREAD_EXITING,
	CORE_THREAD_EXITED,
	CORE_THREAD_JOINED,
};

enum core_sync_kind {
	CORE_SYNC_SPIN = 1,
	CORE_SYNC_MUTEX,
	CORE_SYNC_RWLOCK,
	CORE_SYNC_SEMAPHORE,
	CORE_SYNC_EVENT,
	CORE_SYNC_COMPLETION,
};

enum core_sync_wait_kind {
	CORE_SYNC_WAIT_NORMAL = 1,
	CORE_SYNC_WAIT_READER,
	CORE_SYNC_WAIT_WRITER,
};

struct core_binding {
	uint64_t magic;
	uint64_t generation;
	uint32_t node_id;
	uint32_t interface_id;
	uint32_t instance_id;
	uint32_t reserved;
	size_t object_count;
	struct core_binding *next;
};

struct core_allocation {
	uint64_t magic;
	struct core_binding *binding;
	kb2_core_memory_cache_t cache;
	void *base;
	size_t requested_size;
	size_t usable_size;
	uint32_t order;
	uint32_t kind;
};

struct kb2_core_memory_cache {
	uint64_t magic;
	struct core_binding *binding;
	size_t object_size;
	size_t alignment;
	size_t live_count;
	uint32_t flags;
	uint32_t order;
	kb2_core_cache_constructor_fn constructor;
	kb2_core_cache_destructor_fn destructor;
	void *argument;
	char name[KB2_CORE_RUNTIME_NAME_MAXIMUM_BYTES + 1];
};

struct core_cpu_local_state {
	struct core_cpu_local_state *next;
	struct core_binding *binding;
	uint64_t generation;
	uint32_t preempt_count;
	uint32_t migrate_count;
	uint32_t local_irq_count;
	uint32_t bottom_half_count;
};

struct kb2_core_cpu_percpu_allocation {
	uint64_t magic;
	struct core_binding *binding;
	void *base;
	size_t object_size;
	size_t alignment;
	size_t stride;
	size_t data_offset;
	uint32_t order;
	uint32_t cpu_count;
};

struct core_sync_waiter {
	struct core_sync_waiter *next;
	void *auxiliary;
	uintptr_t owner;
	uint64_t deadline_ns;
	kb2_core_status_t status;
	uint32_t kind;
	uint32_t wait_word;
};

struct core_sync_object {
	uint64_t magic;
	struct core_binding *binding;
	struct core_sync_object *next;
	struct core_sync_waiter *wait_head;
	struct core_sync_waiter *wait_tail;
	uint32_t kind;
	uint32_t lock_word;
	uint32_t closing;
	uint32_t order;
};

struct kb2_core_sync_spin {
	struct core_sync_object object;
	uintptr_t owner;
	uint32_t held;
};

struct kb2_core_sync_mutex {
	struct core_sync_object object;
	uintptr_t owner;
};

struct core_sync_reader {
	struct core_sync_reader *next;
	uintptr_t owner;
};

struct kb2_core_sync_rwlock {
	struct core_sync_object object;
	struct core_sync_reader *readers;
	uintptr_t writer_owner;
	size_t reader_count;
};

struct kb2_core_sync_semaphore {
	struct core_sync_object object;
	uint32_t count;
	uint32_t maximum_count;
};

struct kb2_core_sync_event {
	struct core_sync_object object;
	uint32_t manual_reset;
	uint32_t signaled;
};

struct kb2_core_sync_completion {
	struct core_sync_object object;
	uint32_t count;
	uint32_t complete_all;
};

struct kb2_core_thread {
	uint64_t magic;
	struct core_binding *binding;
	struct kb2_core_thread *next;
	pthread_t native;
	kb2_core_thread_entry_fn entry;
	void *argument;
	uint64_t identity;
	uint64_t generation;
	uint32_t lock_word;
	uint32_t state;
	uint32_t startup_word;
	uint32_t startup_release_word;
	uint32_t done_word;
	uint32_t park_word;
	uint32_t parked;
	uint32_t unpark_permit;
	uint32_t wake_pending;
	uint32_t stop_requested;
	uint32_t interrupted;
	uint32_t detached;
	uint32_t join_in_progress;
	uint32_t ownership_released;
	uint32_t borrowed;
	uint32_t start_parked;
	uint32_t startup_abort;
	uint32_t native_tid;
	uint32_t logical_cpu;
	int wake_descriptor;
	uint32_t *blocking_word;
	uint32_t blocking_interruptible;
	uint32_t time_waiting;
	uint32_t time_wait_interruptible;
	int32_t exit_status;
	int32_t priority;
	char name[KB2_CORE_RUNTIME_NAME_MAXIMUM_BYTES + 1];
};

struct kb2_core_time_timer {
	uint64_t magic;
	struct core_binding *binding;
	struct core_binding *thread_binding;
	struct kb2_core_time_timer *next;
	kb2_core_timer_callback_fn callback;
	void *argument;
	uint64_t generation;
	uint64_t arm_generation;
	uint64_t deadline_ns;
	uint64_t period_ns;
	uint64_t queued_low;
	uint64_t queued_high;
	uint32_t clock_id;
	uint32_t callback_context;
	uint32_t flags;
	uint32_t assigned_cpu;
	uint32_t pending;
	uint32_t running;
	uint32_t closing;
	uint32_t drain_word;
};

struct core_time_cpu {
	pthread_t dispatcher;
	pthread_t worker;
	struct kb2_core_thread worker_thread;
	int dispatcher_descriptor;
	int worker_descriptor;
	int timer_descriptors[3];
	uint32_t logical_cpu;
	uint32_t dispatcher_started;
	uint32_t worker_started;
};

enum core_work_pending_state {
	CORE_WORK_IDLE = 0,
	CORE_WORK_READY,
	CORE_WORK_DELAYED,
};

struct kb2_core_workqueue_queue;

struct kb2_core_workqueue_work {
	uint64_t magic;
	struct core_binding *binding;
	struct kb2_core_workqueue_work *next;
	struct kb2_core_workqueue_work *ready_previous;
	struct kb2_core_workqueue_work *ready_next;
	kb2_core_work_callback_fn callback;
	void *argument;
	struct kb2_core_workqueue_queue *pending_queue;
	struct kb2_core_workqueue_queue *running_queue;
	uint64_t generation;
	uint64_t sequence;
	uint64_t pending_sequence;
	uint64_t running_sequence;
	uint64_t pending_epoch;
	uint64_t running_epoch;
	uint64_t deadline_ns;
	uint32_t pending_cpu;
	uint32_t pending_state;
	uint32_t running;
	uint32_t closing;
	uint32_t drain_word;
};

struct core_work_worker {
	pthread_t native;
	struct kb2_core_workqueue_queue *queue;
	struct kb2_core_thread thread;
	uint32_t cpu_id;
	uint32_t domain;
	uint32_t rescuer;
	uint32_t started;
	uint32_t startup_word;
};

struct kb2_core_workqueue_queue {
	uint64_t magic;
	struct core_binding *binding;
	struct core_binding *thread_binding;
	struct kb2_core_workqueue_queue *next;
	struct kb2_core_workqueue_work *ready_head;
	struct kb2_core_workqueue_work *ready_tail;
	struct core_work_worker *workers;
	uint32_t *active_counts;
	int *domain_descriptors;
	pthread_t dispatcher;
	uint64_t generation;
	uint64_t submission_epoch;
	size_t worker_count;
	uint32_t worker_order;
	uint32_t active_order;
	uint32_t domain_order;
	uint32_t flags;
	uint32_t maximum_active;
	uint32_t domain_count;
	size_t normal_worker_count;
	uint32_t dispatcher_started;
	uint32_t accepting;
	uint32_t stop;
	uint32_t closing;
	uint32_t drain_word;
	int dispatcher_descriptor;
	int timer_descriptor;
	char name[KB2_CORE_RUNTIME_NAME_MAXIMUM_BYTES + 1];
};

struct kb2_core_rcu_domain;

struct kb2_core_rcu_read_token {
	uint64_t magic;
	struct core_binding *binding;
	struct kb2_core_rcu_domain *domain;
	struct kb2_core_rcu_read_token *next;
	uint64_t generation;
	uint64_t sequence;
	uint64_t owner;
};

struct core_rcu_callback {
	uint64_t magic;
	struct core_binding *binding;
	struct kb2_core_rcu_domain *domain;
	struct core_rcu_callback *next;
	kb2_core_rcu_callback_fn callback;
	void *argument;
	uint64_t generation;
	uint64_t sequence;
	uint64_t reader_target;
};

struct kb2_core_rcu_domain {
	uint64_t magic;
	struct core_binding *binding;
	struct kb2_core_rcu_domain *next;
	struct kb2_core_rcu_read_token *readers;
	struct core_rcu_callback *callback_head;
	struct core_rcu_callback *callback_tail;
	uint64_t generation;
	uint64_t reader_sequence;
	uint64_t callback_sequence;
	uint64_t callback_completed;
	uint64_t running_callback_sequence;
	uint64_t running_reader_target;
	uint32_t domain_class;
	uint32_t default_domain;
	uint32_t closing;
	uint32_t wait_word;
};

static struct kobox_provider_arena *core_arena;
static struct kobox_provider_lifecycle core_lifecycle;
static struct core_binding *core_bindings;
static struct core_binding *core_retired_bindings;
static uint64_t core_generation;
static uint32_t core_node_id;
static uint32_t core_state;
static uint32_t core_lock_word;
static uint32_t core_next_binding_id;
static uint32_t core_cpu_count;
static size_t core_cpu_local_state_count;
static size_t core_cpu_percpu_count;
static struct core_sync_object *core_sync_objects;
static size_t core_sync_object_count;
static uint32_t core_sync_active;
static uint64_t core_sync_next_thread_id = 1;
static struct kb2_core_thread *core_threads;
static struct kb2_core_thread core_root_thread;
static size_t core_thread_count;
static uint32_t core_thread_active;
static uint32_t core_time_active;
static size_t core_time_sleep_count;
static uint32_t core_time_sleep_word;
static struct kb2_core_time_timer *core_timers;
static size_t core_timer_count;
static uint32_t core_time_lock_word;
static uint32_t core_time_stop;
static uint32_t core_time_next_cpu;
static struct core_time_cpu core_time_cpus[CORE_NATIVE_CPU_LIMIT];
static struct kb2_core_workqueue_queue *core_work_queues;
static struct kb2_core_workqueue_work *core_works;
static size_t core_work_queue_count;
static size_t core_work_count;
static uint32_t core_work_lock_word;
static uint32_t core_work_active;
static struct kb2_core_rcu_domain core_rcu_default_domain;
static struct kb2_core_rcu_domain *core_rcu_domains;
static size_t core_rcu_domain_count;
static size_t core_rcu_token_count;
static size_t core_rcu_callback_count;
static pthread_t core_rcu_worker;
static struct kb2_core_thread core_rcu_worker_thread;
static int core_rcu_descriptor = -1;
static uint32_t core_rcu_lock_word;
static uint32_t core_rcu_active;
static uint32_t core_rcu_stop;
static uint32_t core_rcu_worker_started;
static uint32_t core_rcu_worker_startup_word;
static uint32_t core_native_cpu_ids[CORE_NATIVE_CPU_LIMIT];
static uint32_t core_native_cpu_count;
static uint64_t core_native_original_mask[CORE_NATIVE_CPU_WORDS];
static __thread struct core_cpu_local_state *core_cpu_local_states;
static __thread uint32_t core_cpu_id;
static __thread uint32_t core_cpu_context_class;
static __thread uint64_t core_sync_thread_id;
static __thread struct kb2_core_thread *core_current_thread;
static __thread struct kb2_core_time_timer *core_current_timer;
static __thread struct kb2_core_workqueue_work *core_current_work;
static __thread struct core_rcu_callback *core_current_rcu_callback;
static __thread uint32_t core_rcu_read_depth;

static const struct kb2_core_memory_operations core_memory_operations;
static const struct kb2_core_cpu_operations core_cpu_operations;
static const struct kb2_core_sync_operations core_sync_operations;
static const struct kb2_core_thread_operations core_thread_operations;
static const struct kb2_core_time_operations core_time_operations;
static const struct kb2_core_workqueue_operations core_workqueue_operations;
static const struct kb2_core_rcu_operations core_rcu_operations;

static int cpu_lifecycle_init(const struct kobox_module_context *context);
static int cpu_lifecycle_quiesce(const struct kobox_module_context *context);
static int cpu_lifecycle_cleanup(const struct kobox_module_context *context);
static int sync_lifecycle_init(const struct kobox_module_context *context);
static int sync_lifecycle_quiesce(const struct kobox_module_context *context);
static int sync_lifecycle_cleanup(const struct kobox_module_context *context);
static int thread_lifecycle_init(const struct kobox_module_context *context);
static int thread_lifecycle_quiesce(const struct kobox_module_context *context);
static int thread_lifecycle_cleanup(const struct kobox_module_context *context);
static int time_lifecycle_init(const struct kobox_module_context *context);
static int time_lifecycle_quiesce(const struct kobox_module_context *context);
static int time_lifecycle_cleanup(const struct kobox_module_context *context);
static int work_lifecycle_init(const struct kobox_module_context *context);
static int work_lifecycle_quiesce(const struct kobox_module_context *context);
static int work_lifecycle_cleanup(const struct kobox_module_context *context);
static int rcu_lifecycle_init(const struct kobox_module_context *context);
static int rcu_lifecycle_quiesce(const struct kobox_module_context *context);
static int rcu_lifecycle_cleanup(const struct kobox_module_context *context);
static long core_linux_syscall6(long number, long argument1, long argument2,
				long argument3, long argument4, long argument5,
				long argument6);

static void core_lock(void)
{
	while (__atomic_exchange_n(&core_lock_word, 1, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#endif
	}
}

static void core_unlock(void)
{
	__atomic_store_n(&core_lock_word, 0, __ATOMIC_RELEASE);
}

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

static void bytes_zero(void *address, size_t length)
{
	unsigned char *bytes = address;
	size_t index;

	for (index = 0; index < length; index++)
		bytes[index] = 0;
}

static void bytes_copy(void *destination, const void *source, size_t length)
{
	unsigned char *output = destination;
	const unsigned char *input = source;
	size_t index;

	for (index = 0; index < length; index++)
		output[index] = input[index];
}

static int context_valid(const struct kobox_module_context *context)
{
	static const uint8_t identity[KOBOX_MODULE_INTERFACE_IDENTITY_SIZE] =
		KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER;
	const struct kobox_module_runtime_operations *runtime;

	if (!context || context->size != sizeof(*context) ||
	    !context->generation || !context->node_id || context->reserved ||
	    !context->logical_cpu_count || context->reserved2 ||
	    !bytes_equal(context->identity, identity, sizeof(identity)) ||
	    context->core_operations != &kobox_linux_core_directory ||
	    !context->runtime_operations)
		return 0;
	runtime = context->runtime_operations;
	return runtime->size == sizeof(*runtime) &&
	       bytes_equal(runtime->identity, identity, sizeof(identity)) &&
	       runtime->resource_count && runtime->resource_acquire &&
	       runtime->resource_bind && runtime->resource_info;
}

static int binding_list_contains(const struct core_binding *binding)
{
	const struct core_binding *current;

	for (current = core_bindings; current; current = current->next) {
		if (current == binding)
			return 1;
	}
	return 0;
}

static kb2_core_status_t binding_validate_locked(
	const struct core_binding *binding, uint32_t interface_id, int allocation)
{
	if (!binding || !binding_list_contains(binding) ||
	    binding->magic != CORE_BINDING_MAGIC)
		return KB2_CORE_RUNTIME_STATUS_STALE;
	if (binding->generation != core_generation)
		return KB2_CORE_RUNTIME_STATUS_STALE;
	if (binding->interface_id != interface_id)
		return KB2_CORE_RUNTIME_STATUS_INTERFACE;
	if (core_state != CORE_ACTIVE &&
	    (allocation || core_state != CORE_QUIESCED))
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t binding_object_begin(struct core_binding *binding,
					       uint32_t interface_id)
{
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(binding, interface_id, 1);
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		if (binding->object_count == SIZE_MAX)
			status = KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
		else
			binding->object_count++;
	}
	core_unlock();
	return status;
}

static void binding_object_end(struct core_binding *binding)
{
	core_lock();
	if (binding_list_contains(binding) && binding->object_count)
		binding->object_count--;
	core_unlock();
}

static int memory_flags_valid(uint32_t flags)
{
	return !(flags & ~(KB2_CORE_RUNTIME_MEMORY_FLAG_ZERO |
			  KB2_CORE_RUNTIME_MEMORY_FLAG_ATOMIC |
			  KB2_CORE_RUNTIME_MEMORY_FLAG_RECLAIMABLE));
}

static int alignment_valid(size_t alignment)
{
	return alignment && !(alignment & (alignment - 1));
}

static int align_up(size_t value, size_t alignment, size_t *result_out)
{
	size_t mask = alignment - 1;

	if (!alignment_valid(alignment) || value > SIZE_MAX - mask)
		return 0;
	*result_out = (value + mask) & ~mask;
	return 1;
}

static int allocation_order(size_t size, uint32_t *order_out)
{
	size_t pages;
	size_t available = 1;
	uint32_t order = 0;

	if (!size || size > SIZE_MAX - (CORE_PAGE_SIZE - 1))
		return 0;
	pages = (size + CORE_PAGE_SIZE - 1) / CORE_PAGE_SIZE;
	while (available < pages) {
		if (available > SIZE_MAX / 2 || order == UINT32_MAX)
			return 0;
		available *= 2;
		order++;
	}
	*order_out = order;
	return 1;
}

static int arena_init(const struct kobox_module_context *context)
{
	static const uint8_t digest[KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE] =
		KB2_MEMORY_ARENA_SCHEMA_SHA256_BYTES;
	static const uint8_t identity[KB2_MEMORY_ARENA_ABI_IDENTITY_SIZE] =
		KB2_MEMORY_ARENA_ABI_IDENTITY_BYTES;
	const struct kobox_memory_arena_resource_operations *operations;
	struct kobox_module_resource_binding binding;
	struct kobox_module_resource_handle handle;
	struct kobox_module_resource_info info;
	uint32_t state;
	void *address;
	size_t count;
	size_t length;

	if (core_arena ||
	    context->runtime_operations->resource_count(
		    context, KOBOX_LINUX_CORE_MEMORY_SLOT_ID, &state, &count) !=
		    KOBOX_MODULE_RESOURCE_OK ||
	    state != KOBOX_MODULE_RESOURCE_PRESENT_STATE || count != 1 ||
	    context->runtime_operations->resource_acquire(
		    context, KOBOX_LINUX_CORE_MEMORY_SLOT_ID, 0,
		    KB2_MEMORY_ARENA_REQUIRED_RIGHTS, &handle) !=
		    KOBOX_MODULE_RESOURCE_OK ||
	    context->runtime_operations->resource_info(context, handle, &info) !=
		    KOBOX_MODULE_RESOURCE_OK ||
	    info.resource_type != KB2_CLOSURE_RESOURCE_MEMORY ||
	    info.granted_rights != KB2_MEMORY_ARENA_REQUIRED_RIGHTS ||
	    context->runtime_operations->resource_bind(context, handle, digest,
						 &binding) !=
		    KOBOX_MODULE_RESOURCE_OK ||
	    !binding.operations || !binding.object ||
	    binding.operations->size < sizeof(*operations))
		return -1;
	operations = (const struct kobox_memory_arena_resource_operations *)
		binding.operations;
	if (!bytes_equal(operations->base.identity, identity, sizeof(identity)) ||
	    !operations->mapped_range ||
	    operations->mapped_range(binding.object, &address, &length) ||
	    length < KB2_MEMORY_ARENA_MINIMUM_LENGTH ||
	    length % KB2_MEMORY_ARENA_PAGE_SIZE ||
	    (uintptr_t)address % KB2_MEMORY_ARENA_PAGE_SIZE ||
	    kobox_provider_arena_init(address, length, &core_arena) !=
		    KOBOX_PROVIDER_ARENA_OK)
		return -1;
	return 0;
}

static int arena_quiesce(const struct kobox_module_context *context)
{
	(void)context;
	return core_arena ? 0 : -1;
}

static int arena_cleanup(const struct kobox_module_context *context)
{
	struct core_binding *retired;
	int empty;

	(void)context;
	core_lock();
	empty = !core_bindings;
	retired = core_retired_bindings;
	if (empty)
		core_retired_bindings = NULL;
	core_unlock();
	if (!empty)
		return -1;
	while (retired) {
		struct core_binding *next = retired->next;

		if (kobox_provider_arena_release(core_arena, retired, 0) !=
		    KOBOX_PROVIDER_ARENA_OK)
			return -1;
		retired = next;
	}
	return kobox_provider_arena_destroy(&core_arena) ==
		       KOBOX_PROVIDER_ARENA_OK ?
		       0 :
		       -1;
}

static const struct kobox_provider_init_entry core_entries[] = {
	{
		.level = 0,
		.link_order = 0,
		.name = "memory-arena",
		.init = arena_init,
		.quiesce = arena_quiesce,
		.cleanup = arena_cleanup,
	},
	{
		.level = 1,
		.link_order = 0,
		.name = "cpu",
		.init = cpu_lifecycle_init,
		.quiesce = cpu_lifecycle_quiesce,
		.cleanup = cpu_lifecycle_cleanup,
	},
	{
		.level = 2,
		.link_order = 0,
		.name = "synchronization",
		.init = sync_lifecycle_init,
		.quiesce = sync_lifecycle_quiesce,
		.cleanup = sync_lifecycle_cleanup,
	},
	{
		.level = 3,
		.link_order = 0,
		.name = "thread",
		.init = thread_lifecycle_init,
		.quiesce = thread_lifecycle_quiesce,
		.cleanup = thread_lifecycle_cleanup,
	},
	{
		.level = 4,
		.link_order = 0,
		.name = "time",
		.init = time_lifecycle_init,
		.quiesce = time_lifecycle_quiesce,
		.cleanup = time_lifecycle_cleanup,
	},
	{
		.level = 5,
		.link_order = 0,
		.name = "workqueue",
		.init = work_lifecycle_init,
		.quiesce = work_lifecycle_quiesce,
		.cleanup = work_lifecycle_cleanup,
	},
	{
		.level = 6,
		.link_order = 0,
		.name = "rcu",
		.init = rcu_lifecycle_init,
		.quiesce = rcu_lifecycle_quiesce,
		.cleanup = rcu_lifecycle_cleanup,
	},
};

static uint32_t interface_dependency_mask(uint32_t interface_id)
{
	switch (interface_id) {
	case KB2_CORE_RUNTIME_INTERFACE_MEMORY:
		return KB2_CORE_RUNTIME_INTERFACE_MEMORY_DEPENDENCY_MASK;
	case KB2_CORE_RUNTIME_INTERFACE_CPU:
		return KB2_CORE_RUNTIME_INTERFACE_CPU_DEPENDENCY_MASK;
	case KB2_CORE_RUNTIME_INTERFACE_SYNC:
		return KB2_CORE_RUNTIME_INTERFACE_SYNC_DEPENDENCY_MASK;
	case KB2_CORE_RUNTIME_INTERFACE_THREAD:
		return KB2_CORE_RUNTIME_INTERFACE_THREAD_DEPENDENCY_MASK;
	case KB2_CORE_RUNTIME_INTERFACE_TIME:
		return KB2_CORE_RUNTIME_INTERFACE_TIME_DEPENDENCY_MASK;
	case KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE:
		return KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE_DEPENDENCY_MASK;
	case KB2_CORE_RUNTIME_INTERFACE_RCU:
		return KB2_CORE_RUNTIME_INTERFACE_RCU_DEPENDENCY_MASK;
	default:
		return UINT32_MAX;
	}
}

static const void *interface_operations(uint32_t interface_id)
{
	switch (interface_id) {
	case KB2_CORE_RUNTIME_INTERFACE_MEMORY:
		return &core_memory_operations;
	case KB2_CORE_RUNTIME_INTERFACE_CPU:
		return &core_cpu_operations;
	case KB2_CORE_RUNTIME_INTERFACE_SYNC:
		return &core_sync_operations;
	case KB2_CORE_RUNTIME_INTERFACE_THREAD:
		return &core_thread_operations;
	case KB2_CORE_RUNTIME_INTERFACE_TIME:
		return &core_time_operations;
	case KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE:
		return &core_workqueue_operations;
	case KB2_CORE_RUNTIME_INTERFACE_RCU:
		return &core_rcu_operations;
	default:
		return NULL;
	}
}

static uint32_t node_interface_mask_locked(uint32_t node_id)
{
	const struct core_binding *binding;
	uint32_t mask = 0;

	for (binding = core_bindings; binding; binding = binding->next) {
		if (binding->node_id == node_id && binding->interface_id &&
		    binding->interface_id <= KB2_CORE_RUNTIME_INTERFACE_COUNT)
			mask |= UINT32_C(1) << (binding->interface_id - 1);
	}
	return mask;
}

static int binding_has_dependent_locked(const struct core_binding *binding)
{
	const struct core_binding *current;
	uint32_t dependency = UINT32_C(1) << (binding->interface_id - 1);

	for (current = core_bindings; current; current = current->next) {
		if (current != binding && current->node_id == binding->node_id &&
		    (interface_dependency_mask(current->interface_id) & dependency))
			return 1;
	}
	return 0;
}

static kb2_core_status_t core_bind(
	const struct kobox_module_context *context, uint32_t interface_id,
	const uint8_t schema_digest[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE],
	struct kb2_core_binding *binding_out)
{
	static const uint8_t digest[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE] =
		KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES;
	struct core_binding *binding;
	struct core_binding *current;
	const void *operations;
	uint32_t dependencies;

	if (!binding_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	binding_out->operations = NULL;
	binding_out->object = NULL;
	if (!context_valid(context) || !schema_digest ||
	    context->logical_cpu_count != core_cpu_count)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	if (!bytes_equal(schema_digest, digest, sizeof(digest)))
		return KB2_CORE_RUNTIME_STATUS_INTERFACE;
	operations = interface_operations(interface_id);
	dependencies = interface_dependency_mask(interface_id);
	if (!operations || dependencies == UINT32_MAX)
		return KB2_CORE_RUNTIME_STATUS_INTERFACE;
	core_lock();
	if (core_state != CORE_ACTIVE || context->generation != core_generation) {
		core_unlock();
		return context->generation != core_generation ?
			       KB2_CORE_RUNTIME_STATUS_STALE :
			       KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	}
	if ((node_interface_mask_locked(context->node_id) & dependencies) !=
	    dependencies) {
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	}
	for (current = core_bindings; current; current = current->next) {
		if (current->node_id == context->node_id &&
		    current->interface_id == interface_id) {
			core_unlock();
			return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		}
	}
	core_unlock();
	binding = kobox_provider_arena_allocate(core_arena, 0);
	if (!binding)
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	bytes_zero(binding, CORE_PAGE_SIZE);
	binding->magic = CORE_BINDING_MAGIC;
	binding->generation = context->generation;
	binding->node_id = context->node_id;
	binding->interface_id = interface_id;
	core_lock();
	if (core_state != CORE_ACTIVE || context->generation != core_generation) {
		core_unlock();
		binding->magic = 0;
		(void)kobox_provider_arena_release(core_arena, binding, 0);
		return KB2_CORE_RUNTIME_STATUS_STALE;
	}
	if ((node_interface_mask_locked(context->node_id) & dependencies) !=
	    dependencies || !core_next_binding_id) {
		core_unlock();
		binding->magic = 0;
		(void)kobox_provider_arena_release(core_arena, binding, 0);
		return !core_next_binding_id ? KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
					       KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	}
	for (current = core_bindings; current; current = current->next) {
		if (current->node_id == context->node_id &&
		    current->interface_id == interface_id) {
			core_unlock();
			binding->magic = 0;
			(void)kobox_provider_arena_release(core_arena, binding, 0);
			return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		}
	}
	binding->next = core_bindings;
	binding->instance_id = core_next_binding_id++;
	core_bindings = binding;
	core_unlock();
	binding_out->operations = operations;
	binding_out->object = binding;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t core_unbind(
	const struct kobox_module_context *context,
	struct kb2_core_binding *public_binding)
{
	struct core_binding **link;
	struct core_binding *binding;
	const void *operations;

	if (!context_valid(context) ||
	    context->logical_cpu_count != core_cpu_count || !public_binding ||
	    !public_binding->operations || !public_binding->object)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	binding = public_binding->object;
	core_lock();
	for (link = &core_bindings; *link && *link != binding;
	     link = &(*link)->next)
		;
	if (!*link || binding->magic != CORE_BINDING_MAGIC ||
	    binding->generation != core_generation ||
	    binding->generation != context->generation) {
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_STALE;
	}
	operations = interface_operations(binding->interface_id);
	if (!operations || public_binding->operations != operations) {
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	}
	if (binding->node_id != context->node_id) {
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	}
	if (binding->object_count || binding_has_dependent_locked(binding)) {
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_BUSY;
	}
	*link = binding->next;
	binding->magic = 0;
	binding->next = core_retired_bindings;
	core_retired_bindings = binding;
	core_unlock();
	public_binding->operations = NULL;
	public_binding->object = NULL;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t memory_page_allocate(void *binding_object,
					       uint32_t order, uint32_t flags,
					       void **allocation_out)
{
	struct core_binding *binding = binding_object;
	void *allocation;
	size_t length;
	kb2_core_status_t status;

	if (!allocation_out || !memory_flags_valid(flags) ||
	    order >= sizeof(size_t) * CHAR_BIT ||
	    ((size_t)1 << order) > SIZE_MAX / CORE_PAGE_SIZE)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*allocation_out = NULL;
	status = binding_object_begin(
		binding, KB2_CORE_RUNTIME_INTERFACE_MEMORY);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	allocation = kobox_provider_arena_allocate_owned(
		core_arena, order, binding, CORE_ALLOCATION_PAGE);
	if (!allocation) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	length = ((size_t)1 << order) * CORE_PAGE_SIZE;
	if (flags & KB2_CORE_RUNTIME_MEMORY_FLAG_ZERO)
		bytes_zero(allocation, length);
	*allocation_out = allocation;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t memory_page_release(void *binding_object,
					      void *allocation,
					      uint32_t order)
{
	struct core_binding *binding = binding_object;
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_MEMORY, 0);
	core_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!kobox_provider_arena_validate_owner(
		    core_arena, allocation, order, binding, CORE_ALLOCATION_PAGE))
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	if (kobox_provider_arena_release_owned(core_arena, allocation, order,
						 binding,
						 CORE_ALLOCATION_PAGE) !=
	    KOBOX_PROVIDER_ARENA_OK)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	binding_object_end(binding);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t allocation_create(
	struct core_binding *binding, size_t size, size_t alignment, uint32_t flags,
	kb2_core_memory_cache_t cache, uint32_t kind, void **allocation_out)
{
	struct core_allocation *record;
	void *base;
	size_t header_end;
	size_t user_offset;
	size_t required;
	size_t block_size;
	uint32_t order;
	kb2_core_status_t status;

	if (!allocation_out || !size || !alignment_valid(alignment) ||
	    !memory_flags_valid(flags) ||
	    sizeof(*record) > SIZE_MAX - (alignment - 1) ||
	    sizeof(*record) + alignment - 1 > SIZE_MAX - size)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*allocation_out = NULL;
	required = sizeof(*record) + alignment - 1 + size;
	if (!allocation_order(required, &order) ||
	    order >= sizeof(size_t) * CHAR_BIT ||
	    ((size_t)1 << order) > SIZE_MAX / CORE_PAGE_SIZE)
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	status = binding_object_begin(
		binding, KB2_CORE_RUNTIME_INTERFACE_MEMORY);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	base = kobox_provider_arena_allocate_owned(core_arena, order, binding,
						 kind);
	if (!base) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	header_end = (size_t)(uintptr_t)base + sizeof(*record);
	if (!align_up(header_end, alignment, &user_offset)) {
		(void)kobox_provider_arena_release_owned(core_arena, base, order,
							 binding, kind);
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	block_size = ((size_t)1 << order) * CORE_PAGE_SIZE;
	record = (struct core_allocation *)(uintptr_t)(user_offset -
							 sizeof(*record));
	*record = (struct core_allocation){
		.magic = CORE_ALLOCATION_MAGIC,
		.binding = binding,
		.cache = cache,
		.base = base,
		.requested_size = size,
		.usable_size = block_size - (user_offset - (size_t)(uintptr_t)base),
		.order = order,
		.kind = kind,
	};
	*allocation_out = (void *)(uintptr_t)user_offset;
	if (flags & KB2_CORE_RUNTIME_MEMORY_FLAG_ZERO)
		bytes_zero(*allocation_out, record->usable_size);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t allocation_find(
	struct core_binding *binding, void *allocation, uint32_t expected_kind,
	kb2_core_memory_cache_t expected_cache,
	struct core_allocation **record_out)
{
	struct core_allocation *record;
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_MEMORY, 0);
	core_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!allocation ||
	    (uintptr_t)allocation < sizeof(struct core_allocation))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	record = (struct core_allocation *)((unsigned char *)allocation -
						  sizeof(*record));
	if (!kobox_provider_arena_contains(core_arena, record, sizeof(*record)) ||
	    record->magic != CORE_ALLOCATION_MAGIC ||
	    record->binding != binding || record->kind != expected_kind ||
	    record->cache != expected_cache)
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	if (!kobox_provider_arena_validate_owner(
		    core_arena, record->base, record->order, binding, expected_kind))
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	*record_out = record;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t allocation_release(
	struct core_binding *binding, void *allocation, uint32_t expected_kind,
	kb2_core_memory_cache_t expected_cache)
{
	struct core_allocation *record;
	void *base;
	uint32_t order;
	kb2_core_status_t status;

	status = allocation_find(binding, allocation, expected_kind,
				 expected_cache, &record);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	base = record->base;
	order = record->order;
	if (kobox_provider_arena_release_owned(core_arena, base, order, binding,
						 expected_kind) !=
	    KOBOX_PROVIDER_ARENA_OK)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	binding_object_end(binding);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t memory_allocate(void *binding_object, size_t size,
					  size_t alignment, uint32_t flags,
					  void **allocation_out)
{
	return allocation_create(binding_object, size, alignment, flags, NULL,
				 CORE_ALLOCATION_GENERAL, allocation_out);
}

static kb2_core_status_t memory_reallocate(void *binding_object,
					    void *allocation, size_t size,
					    size_t alignment, uint32_t flags,
					    void **allocation_out)
{
	struct core_binding *binding = binding_object;
	struct core_allocation *old_record;
	void *replacement;
	size_t copy_size;
	kb2_core_status_t status;

	if (!allocation_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*allocation_out = NULL;
	status = allocation_find(binding, allocation, CORE_ALLOCATION_GENERAL,
				 NULL, &old_record);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = allocation_create(binding, size, alignment, flags, NULL,
				   CORE_ALLOCATION_GENERAL, &replacement);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	copy_size = old_record->requested_size < size ?
			    old_record->requested_size :
			    size;
	bytes_copy(replacement, allocation, copy_size);
	status = allocation_release(binding, allocation, CORE_ALLOCATION_GENERAL,
				    NULL);
	if (status != KB2_CORE_RUNTIME_STATUS_OK) {
		(void)allocation_release(binding, replacement,
					 CORE_ALLOCATION_GENERAL, NULL);
		return status;
	}
	*allocation_out = replacement;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t memory_release(void *binding_object, void *allocation)
{
	return allocation_release(binding_object, allocation,
				  CORE_ALLOCATION_GENERAL, NULL);
}

static kb2_core_status_t memory_usable_size(void *binding_object,
					     void *allocation,
					     size_t *size_out)
{
	struct core_allocation *record;
	kb2_core_status_t status;

	if (!size_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = allocation_find(binding_object, allocation,
				 CORE_ALLOCATION_GENERAL, NULL, &record);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	*size_out = record->usable_size;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cache_validate_locked(
	struct core_binding *binding, kb2_core_memory_cache_t cache)
{
	kb2_core_status_t status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_MEMORY, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!cache ||
	    !kobox_provider_arena_contains(core_arena, cache, sizeof(*cache)) ||
	    cache->magic != CORE_CACHE_MAGIC ||
	    cache->binding != binding ||
	    !kobox_provider_arena_validate_owner(
		    core_arena, cache, cache->order, binding, CORE_ALLOCATION_CACHE))
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t memory_cache_create(
	void *binding_object, const char *name, size_t name_length,
	size_t object_size, size_t alignment, uint32_t flags,
	kb2_core_cache_constructor_fn constructor,
	kb2_core_cache_destructor_fn destructor, void *argument,
	kb2_core_memory_cache_t *cache_out)
{
	struct core_binding *binding = binding_object;
	kb2_core_memory_cache_t cache;
	uint32_t order;
	kb2_core_status_t status;

	if (!cache_out || !name || !name_length ||
	    name_length > KB2_CORE_RUNTIME_NAME_MAXIMUM_BYTES || !object_size ||
	    !alignment_valid(alignment) || !memory_flags_valid(flags) ||
	    !allocation_order(sizeof(*cache), &order))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*cache_out = NULL;
	status = binding_object_begin(
		binding, KB2_CORE_RUNTIME_INTERFACE_MEMORY);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	cache = kobox_provider_arena_allocate_owned(
		core_arena, order, binding, CORE_ALLOCATION_CACHE);
	if (!cache) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(cache, sizeof(*cache));
	cache->magic = CORE_CACHE_MAGIC;
	cache->binding = binding;
	cache->object_size = object_size;
	cache->alignment = alignment;
	cache->flags = flags;
	cache->order = order;
	cache->constructor = constructor;
	cache->destructor = destructor;
	cache->argument = argument;
	bytes_copy(cache->name, name, name_length);
	cache->name[name_length] = '\0';
	*cache_out = cache;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t memory_cache_destroy(
	void *binding_object, kb2_core_memory_cache_t cache)
{
	struct core_binding *binding = binding_object;
	uint32_t order;
	kb2_core_status_t status;

	core_lock();
	status = cache_validate_locked(binding, cache);
	if (status == KB2_CORE_RUNTIME_STATUS_OK && cache->live_count)
		status = KB2_CORE_RUNTIME_STATUS_BUSY;
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		order = cache->order;
		cache->magic = 0;
	}
	core_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (kobox_provider_arena_release_owned(core_arena, cache, order, binding,
						 CORE_ALLOCATION_CACHE) !=
	    KOBOX_PROVIDER_ARENA_OK)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	binding_object_end(binding);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t memory_cache_allocate(
	void *binding_object, kb2_core_memory_cache_t cache, uint32_t flags,
	void **allocation_out)
{
	struct core_binding *binding = binding_object;
	void *allocation;
	kb2_core_status_t status;
	uint32_t combined_flags;

	if (!allocation_out || !memory_flags_valid(flags))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*allocation_out = NULL;
	core_lock();
	status = cache_validate_locked(binding, cache);
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		if (cache->live_count == SIZE_MAX)
			status = KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
		else
			cache->live_count++;
	}
	core_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	combined_flags = cache->flags | flags;
	status = allocation_create(binding, cache->object_size, cache->alignment,
				   combined_flags, cache,
				   CORE_ALLOCATION_CACHE_OBJECT, &allocation);
	if (status == KB2_CORE_RUNTIME_STATUS_OK && cache->constructor) {
		status = cache->constructor(allocation, cache->object_size,
					    cache->argument);
		if (status < (kb2_core_status_t)KB2_CORE_RUNTIME_STATUS_OK ||
		    status >
			    (kb2_core_status_t)KB2_CORE_RUNTIME_STATUS_INTERFACE)
			status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	if (status != KB2_CORE_RUNTIME_STATUS_OK) {
		if (allocation)
			(void)allocation_release(binding, allocation,
						 CORE_ALLOCATION_CACHE_OBJECT,
						 cache);
		core_lock();
		if (cache->live_count)
			cache->live_count--;
		core_unlock();
		return status;
	}
	*allocation_out = allocation;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t memory_cache_release(
	void *binding_object, kb2_core_memory_cache_t cache, void *allocation)
{
	struct core_binding *binding = binding_object;
	struct core_allocation *record;
	kb2_core_status_t status;

	status = allocation_find(binding, allocation,
				 CORE_ALLOCATION_CACHE_OBJECT, cache, &record);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (cache->destructor)
		cache->destructor(allocation, cache->object_size, cache->argument);
	status = allocation_release(binding, allocation,
				    CORE_ALLOCATION_CACHE_OBJECT, cache);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	core_lock();
	if (!cache->live_count)
		status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
	else
		cache->live_count--;
	core_unlock();
	return status;
}

static kb2_core_status_t memory_statistics(void *binding_object,
					    size_t *total_pages_out,
					    size_t *free_pages_out,
					    uint32_t *largest_order_out)
{
	kb2_core_status_t status;

	if (!total_pages_out || !free_pages_out || !largest_order_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	core_lock();
	status = binding_validate_locked(
		binding_object, KB2_CORE_RUNTIME_INTERFACE_MEMORY, 0);
	core_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	*total_pages_out = kobox_provider_arena_total_pages(core_arena);
	*free_pages_out = kobox_provider_arena_free_pages(core_arena);
	*largest_order_out = kobox_provider_arena_max_order(core_arena);
	return *total_pages_out ? KB2_CORE_RUNTIME_STATUS_OK :
				KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

enum core_cpu_counter {
	CORE_CPU_COUNTER_PREEMPT = 1,
	CORE_CPU_COUNTER_MIGRATE,
	CORE_CPU_COUNTER_LOCAL_IRQ,
	CORE_CPU_COUNTER_BOTTOM_HALF,
};

static kb2_core_status_t cpu_binding_validate(struct core_binding *binding,
					       int allocation)
{
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_CPU, allocation);
	core_unlock();
	return status;
}

static struct core_cpu_local_state *cpu_local_find(
	const struct core_binding *binding)
{
	struct core_cpu_local_state *state;

	for (state = core_cpu_local_states; state; state = state->next) {
		if (state->binding == binding)
			return state;
	}
	return NULL;
}

static kb2_core_status_t cpu_local_create(
	struct core_binding *binding, struct core_cpu_local_state **state_out)
{
	struct core_cpu_local_state *state;
	kb2_core_status_t status;

	status = binding_object_begin(binding, KB2_CORE_RUNTIME_INTERFACE_CPU);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	state = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_CPU_LOCAL);
	if (!state) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(state, CORE_PAGE_SIZE);
	state->binding = binding;
	state->generation = binding->generation;
	core_lock();
	if (core_cpu_local_state_count == SIZE_MAX) {
		core_unlock();
		(void)kobox_provider_arena_release_owned(
			core_arena, state, 0, binding, CORE_ALLOCATION_CPU_LOCAL);
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	}
	core_cpu_local_state_count++;
	core_unlock();
	state->next = core_cpu_local_states;
	core_cpu_local_states = state;
	*state_out = state;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cpu_local_get(
	struct core_binding *binding, int create,
	struct core_cpu_local_state **state_out)
{
	struct core_cpu_local_state *state;
	kb2_core_status_t status;

	status = cpu_binding_validate(binding, create);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	state = cpu_local_find(binding);
	if (state && state->generation != binding->generation)
		return KB2_CORE_RUNTIME_STATUS_STALE;
	if (!state && create)
		return cpu_local_create(binding, state_out);
	*state_out = state;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static int cpu_local_idle(const struct core_cpu_local_state *state)
{
	return !state->preempt_count && !state->migrate_count &&
	       !state->local_irq_count && !state->bottom_half_count;
}

static kb2_core_status_t cpu_local_release_if_idle(
	struct core_cpu_local_state *state)
{
	struct core_cpu_local_state **link;
	struct core_binding *binding;

	if (!cpu_local_idle(state))
		return KB2_CORE_RUNTIME_STATUS_OK;
	for (link = &core_cpu_local_states; *link && *link != state;
	     link = &(*link)->next)
		;
	if (!*link)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	*link = state->next;
	binding = state->binding;
	if (kobox_provider_arena_release_owned(
		    core_arena, state, 0, binding,
		    CORE_ALLOCATION_CPU_LOCAL) != KOBOX_PROVIDER_ARENA_OK)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	core_lock();
	if (!core_cpu_local_state_count) {
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	core_cpu_local_state_count--;
	core_unlock();
	binding_object_end(binding);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static uint32_t *cpu_counter_address(struct core_cpu_local_state *state,
				     enum core_cpu_counter counter)
{
	switch (counter) {
	case CORE_CPU_COUNTER_PREEMPT:
		return &state->preempt_count;
	case CORE_CPU_COUNTER_MIGRATE:
		return &state->migrate_count;
	case CORE_CPU_COUNTER_LOCAL_IRQ:
		return &state->local_irq_count;
	case CORE_CPU_COUNTER_BOTTOM_HALF:
		return &state->bottom_half_count;
	default:
		return NULL;
	}
}

static kb2_core_status_t cpu_counter_increment(
	void *binding_object, enum core_cpu_counter counter)
{
	struct core_binding *binding = binding_object;
	struct core_cpu_local_state *state;
	uint32_t *count;
	kb2_core_status_t status;

	status = cpu_local_get(binding, 1, &state);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	count = cpu_counter_address(state, counter);
	if (!count)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	if (*count == UINT32_MAX)
		return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	(*count)++;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cpu_counter_decrement(
	void *binding_object, enum core_cpu_counter counter)
{
	struct core_binding *binding = binding_object;
	struct core_cpu_local_state *state;
	uint32_t *count;
	kb2_core_status_t status;

	status = cpu_local_get(binding, 0, &state);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!state)
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	count = cpu_counter_address(state, counter);
	if (!count)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	if (!*count)
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	(*count)--;
	return cpu_local_release_if_idle(state);
}

static kb2_core_status_t cpu_counter_read(
	void *binding_object, enum core_cpu_counter counter, uint32_t *count_out)
{
	struct core_binding *binding = binding_object;
	struct core_cpu_local_state *state;
	uint32_t *count;
	kb2_core_status_t status;

	if (!count_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = cpu_local_get(binding, 0, &state);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!state) {
		*count_out = 0;
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	count = cpu_counter_address(state, counter);
	if (!count)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	*count_out = *count;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cpu_topology_count(void *binding_object,
					     uint32_t *count_out)
{
	kb2_core_status_t status;

	if (!count_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = cpu_binding_validate(binding_object, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	*count_out = core_cpu_count;
	return core_cpu_count ? KB2_CORE_RUNTIME_STATUS_OK :
				KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

static kb2_core_status_t cpu_current(void *binding_object,
				      uint32_t *cpu_id_out)
{
	uint32_t native_cpu;
	uint32_t index;
	kb2_core_status_t status;

	if (!cpu_id_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = cpu_binding_validate(binding_object, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (core_linux_syscall6(__NR_getcpu, (long)(uintptr_t)&native_cpu, 0,
				0, 0, 0, 0) < 0)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	for (index = 0; index < core_native_cpu_count; index++) {
		if (core_native_cpu_ids[index] == native_cpu) {
			*cpu_id_out = index;
			core_cpu_id = index;
			return KB2_CORE_RUNTIME_STATUS_OK;
		}
	}
	if (core_cpu_id >= core_cpu_count)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	return KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

static kb2_core_status_t cpu_is_online(void *binding_object, uint32_t cpu_id,
					uint32_t *online_out)
{
	kb2_core_status_t status;

	if (!online_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*online_out = 0;
	status = cpu_binding_validate(binding_object, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (cpu_id >= core_cpu_count)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*online_out = 1;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cpu_context_class(void *binding_object,
					    uint32_t *context_class_out)
{
	kb2_core_status_t status;

	if (!context_class_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = cpu_binding_validate(binding_object, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	*context_class_out = core_cpu_context_class ?
				     core_cpu_context_class :
				     KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cpu_preempt_disable(void *binding_object)
{
	return cpu_counter_increment(binding_object, CORE_CPU_COUNTER_PREEMPT);
}

static kb2_core_status_t cpu_preempt_enable(void *binding_object)
{
	return cpu_counter_decrement(binding_object, CORE_CPU_COUNTER_PREEMPT);
}

static kb2_core_status_t cpu_preempt_count(void *binding_object,
					    uint32_t *count_out)
{
	return cpu_counter_read(binding_object, CORE_CPU_COUNTER_PREEMPT,
				count_out);
}

static kb2_core_status_t cpu_migrate_disable(void *binding_object)
{
	return cpu_counter_increment(binding_object, CORE_CPU_COUNTER_MIGRATE);
}

static kb2_core_status_t cpu_migrate_enable(void *binding_object)
{
	return cpu_counter_decrement(binding_object, CORE_CPU_COUNTER_MIGRATE);
}

static kb2_core_status_t cpu_migrate_count(void *binding_object,
					    uint32_t *count_out)
{
	return cpu_counter_read(binding_object, CORE_CPU_COUNTER_MIGRATE,
				count_out);
}

static kb2_core_status_t cpu_local_irq_disable(void *binding_object)
{
	return cpu_counter_increment(binding_object,
				     CORE_CPU_COUNTER_LOCAL_IRQ);
}

static kb2_core_status_t cpu_local_irq_enable(void *binding_object)
{
	return cpu_counter_decrement(binding_object,
				     CORE_CPU_COUNTER_LOCAL_IRQ);
}

static kb2_core_status_t cpu_local_irq_save(void *binding_object,
					     uint64_t *irq_state_out)
{
	struct core_binding *binding = binding_object;
	struct core_cpu_local_state *state;
	kb2_core_status_t status;

	if (!irq_state_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = cpu_counter_increment(binding, CORE_CPU_COUNTER_LOCAL_IRQ);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	state = cpu_local_find(binding);
	if (!state || !state->local_irq_count)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	*irq_state_out = ((uint64_t)binding->instance_id << 32) |
			 (state->local_irq_count - 1);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cpu_local_irq_restore(void *binding_object,
						uint64_t irq_state)
{
	struct core_binding *binding = binding_object;
	struct core_cpu_local_state *state;
	uint32_t saved_count = (uint32_t)irq_state;
	kb2_core_status_t status;

	status = cpu_local_get(binding, 0, &state);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!state || (uint32_t)(irq_state >> 32) != binding->instance_id ||
	    saved_count == UINT32_MAX ||
	    state->local_irq_count != saved_count + 1)
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	state->local_irq_count = saved_count;
	return cpu_local_release_if_idle(state);
}

static kb2_core_status_t cpu_bottom_half_disable(void *binding_object)
{
	return cpu_counter_increment(binding_object,
				     CORE_CPU_COUNTER_BOTTOM_HALF);
}

static kb2_core_status_t cpu_bottom_half_enable(void *binding_object)
{
	return cpu_counter_decrement(binding_object,
				     CORE_CPU_COUNTER_BOTTOM_HALF);
}

static kb2_core_status_t cpu_percpu_validate(
	struct core_binding *binding,
	kb2_core_cpu_percpu_allocation_t allocation)
{
	kb2_core_status_t status = cpu_binding_validate(binding, 0);
	size_t expected_stride;
	size_t block_size;

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!allocation || !kobox_provider_arena_contains(
				   core_arena, allocation, sizeof(*allocation)) ||
	    allocation->magic != CORE_PERCPU_MAGIC ||
	    allocation->binding != binding || allocation->base != allocation ||
	    allocation->cpu_count != core_cpu_count)
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	if (!allocation->object_size ||
	    !alignment_valid(allocation->alignment) ||
	    !align_up(allocation->object_size, allocation->alignment,
		      &expected_stride) ||
	    allocation->stride != expected_stride ||
	    allocation->order >= sizeof(size_t) * CHAR_BIT ||
	    ((size_t)1 << allocation->order) > SIZE_MAX / CORE_PAGE_SIZE)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	block_size = ((size_t)1 << allocation->order) * CORE_PAGE_SIZE;
	if (allocation->data_offset < sizeof(*allocation) ||
	    allocation->data_offset > block_size ||
	    (uintptr_t)((unsigned char *)allocation +
			allocation->data_offset) % allocation->alignment ||
	    allocation->stride >
		    (block_size - allocation->data_offset) /
			    allocation->cpu_count)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	if (!kobox_provider_arena_validate_owner(
		    core_arena, allocation->base, allocation->order, binding,
		    CORE_ALLOCATION_PERCPU))
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cpu_percpu_allocate(
	void *binding_object, size_t size, size_t alignment, uint32_t flags,
	kb2_core_cpu_percpu_allocation_t *allocation_out)
{
	struct core_binding *binding = binding_object;
	kb2_core_cpu_percpu_allocation_t allocation;
	size_t stride;
	size_t payload_size;
	size_t required;
	size_t data_address;
	uint32_t order;
	kb2_core_status_t status;

	if (!allocation_out || !size || !alignment_valid(alignment) ||
	    flags & ~KB2_CORE_RUNTIME_CPU_PERCPU_FLAG_ZERO ||
	    !align_up(size, alignment, &stride) || !core_cpu_count ||
	    stride > SIZE_MAX / core_cpu_count)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*allocation_out = NULL;
	payload_size = stride * core_cpu_count;
	if (sizeof(*allocation) > SIZE_MAX - (alignment - 1) ||
	    sizeof(*allocation) + alignment - 1 > SIZE_MAX - payload_size)
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	required = sizeof(*allocation) + alignment - 1 + payload_size;
	if (!allocation_order(required, &order))
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	status = binding_object_begin(binding, KB2_CORE_RUNTIME_INTERFACE_CPU);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	allocation = kobox_provider_arena_allocate_owned(
		core_arena, order, binding, CORE_ALLOCATION_PERCPU);
	if (!allocation) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	if (!align_up((size_t)(uintptr_t)allocation + sizeof(*allocation),
		      alignment, &data_address)) {
		(void)kobox_provider_arena_release_owned(
			core_arena, allocation, order, binding,
			CORE_ALLOCATION_PERCPU);
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	*allocation = (struct kb2_core_cpu_percpu_allocation){
		.magic = CORE_PERCPU_MAGIC,
		.binding = binding,
		.base = allocation,
		.object_size = size,
		.alignment = alignment,
		.stride = stride,
		.data_offset = data_address - (size_t)(uintptr_t)allocation,
		.order = order,
		.cpu_count = core_cpu_count,
	};
	if (flags & KB2_CORE_RUNTIME_CPU_PERCPU_FLAG_ZERO)
		bytes_zero((void *)(uintptr_t)data_address, payload_size);
	core_lock();
	if (core_cpu_percpu_count == SIZE_MAX) {
		core_unlock();
		allocation->magic = 0;
		(void)kobox_provider_arena_release_owned(
			core_arena, allocation, order, binding,
			CORE_ALLOCATION_PERCPU);
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	}
	core_cpu_percpu_count++;
	core_unlock();
	*allocation_out = allocation;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cpu_percpu_release(
	void *binding_object, kb2_core_cpu_percpu_allocation_t allocation)
{
	struct core_binding *binding = binding_object;
	uint32_t order;
	kb2_core_status_t status;

	status = cpu_percpu_validate(binding, allocation);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	order = allocation->order;
	allocation->magic = 0;
	if (kobox_provider_arena_release_owned(
		    core_arena, allocation, order, binding,
		    CORE_ALLOCATION_PERCPU) != KOBOX_PROVIDER_ARENA_OK)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	core_lock();
	if (!core_cpu_percpu_count) {
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	core_cpu_percpu_count--;
	core_unlock();
	binding_object_end(binding);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t cpu_percpu_address(
	void *binding_object, kb2_core_cpu_percpu_allocation_t allocation,
	uint32_t cpu_id, void **address_out)
{
	struct core_binding *binding = binding_object;
	kb2_core_status_t status;

	if (!address_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*address_out = NULL;
	status = cpu_percpu_validate(binding, allocation);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (cpu_id >= allocation->cpu_count)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*address_out = (unsigned char *)allocation + allocation->data_offset +
		       (size_t)cpu_id * allocation->stride;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static int cpu_lifecycle_init(const struct kobox_module_context *context)
{
	uint64_t available[CORE_NATIVE_CPU_WORDS];
	uint64_t selected[CORE_NATIVE_CPU_WORDS];
	long result;
	uint32_t native_cpu;
	uint32_t count = 0;

	if (!context || !context->logical_cpu_count || core_cpu_count ||
	    core_cpu_local_state_count || core_cpu_percpu_count ||
	    core_cpu_local_states)
		return -1;
	bytes_zero(available, sizeof(available));
	bytes_zero(selected, sizeof(selected));
	result = core_linux_syscall6(
		__NR_sched_getaffinity, 0, sizeof(available),
		(long)(uintptr_t)available, 0, 0, 0);
	if (result < 0)
		return -1;
	bytes_copy(core_native_original_mask, available, sizeof(available));
	for (native_cpu = 0;
	     native_cpu < CORE_NATIVE_CPU_LIMIT &&
	     count < context->logical_cpu_count;
	     native_cpu++) {
		if (!(available[native_cpu / 64] &
		      (UINT64_C(1) << (native_cpu % 64))))
			continue;
		core_native_cpu_ids[count++] = native_cpu;
	}
	if (count != context->logical_cpu_count) {
		bytes_zero(core_native_cpu_ids, sizeof(core_native_cpu_ids));
		bytes_zero(core_native_original_mask,
			   sizeof(core_native_original_mask));
		return -1;
	}
	selected[core_native_cpu_ids[0] / 64] |=
		UINT64_C(1) << (core_native_cpu_ids[0] % 64);
	if (core_linux_syscall6(__NR_sched_setaffinity, 0, sizeof(selected),
				(long)(uintptr_t)selected, 0, 0, 0) < 0) {
		bytes_zero(core_native_cpu_ids, sizeof(core_native_cpu_ids));
		bytes_zero(core_native_original_mask,
			   sizeof(core_native_original_mask));
		return -1;
	}
	core_cpu_count = context->logical_cpu_count;
	core_native_cpu_count = count;
	core_cpu_id = 0;
	core_cpu_context_class = KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD;
	return 0;
}

static int cpu_lifecycle_quiesce(const struct kobox_module_context *context)
{
	int idle;

	(void)context;
	core_lock();
	idle = !core_cpu_local_state_count;
	core_unlock();
	return idle ? 0 : -1;
}

static int cpu_lifecycle_cleanup(const struct kobox_module_context *context)
{
	int empty;
	int restore_failed = 0;

	(void)context;
	core_lock();
	empty = !core_cpu_local_state_count && !core_cpu_percpu_count;
	if (empty)
		core_cpu_count = 0;
	core_unlock();
	if (!empty)
		return -1;
	if (core_linux_syscall6(
		    __NR_sched_setaffinity, 0, sizeof(core_native_original_mask),
		    (long)(uintptr_t)core_native_original_mask, 0, 0, 0) < 0)
		restore_failed = 1;
	core_cpu_local_states = NULL;
	core_cpu_id = 0;
	core_cpu_context_class = 0;
	core_native_cpu_count = 0;
	bytes_zero(core_native_cpu_ids, sizeof(core_native_cpu_ids));
	bytes_zero(core_native_original_mask,
		   sizeof(core_native_original_mask));
	return restore_failed ? -1 : 0;
}

static long core_linux_syscall6(long number, long argument1, long argument2,
				long argument3, long argument4, long argument5,
				long argument6)
{
#if defined(__x86_64__)
	register long register10 __asm__("r10") = argument4;
	register long register8 __asm__("r8") = argument5;
	register long register9 __asm__("r9") = argument6;
	long result;

	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(argument1), "S"(argument2),
			   "d"(argument3), "r"(register10), "r"(register8),
			   "r"(register9)
			 : "rcx", "r11", "memory");
	return result;
#else
#error "The Linux synchronization provider currently requires x86-64"
#endif
}

static long sync_futex_wait(uint32_t *word, uint32_t expected,
			    uint64_t deadline_ns)
{
	struct __kernel_timespec timeout;
	struct __kernel_timespec *timeout_pointer = NULL;

	if (deadline_ns) {
		timeout.tv_sec = deadline_ns / UINT64_C(1000000000);
		timeout.tv_nsec = deadline_ns % UINT64_C(1000000000);
		timeout_pointer = &timeout;
	}
	return core_linux_syscall6(
		__NR_futex, (long)(uintptr_t)word,
		FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, expected,
		(long)(uintptr_t)timeout_pointer, 0, FUTEX_BITSET_MATCH_ANY);
}

static void sync_futex_wake(uint32_t *word)
{
	long result = core_linux_syscall6(
		__NR_futex, (long)(uintptr_t)word,
		FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);

	if (result < 0)
		__builtin_trap();
}

static void thread_lock(struct kb2_core_thread *thread)
{
	while (__atomic_exchange_n(&thread->lock_word, 1, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#endif
	}
}

static void thread_unlock(struct kb2_core_thread *thread)
{
	__atomic_store_n(&thread->lock_word, 0, __ATOMIC_RELEASE);
}

static int thread_wake_descriptor_create(void)
{
	long result = core_linux_syscall6(__NR_eventfd2, 0,
					 EFD_CLOEXEC | EFD_NONBLOCK, 0, 0, 0, 0);

	return result >= 0 && result <= INT_MAX ? (int)result : -1;
}

static void thread_wake_descriptor_signal(struct kb2_core_thread *thread)
{
	uint64_t value = 1;
	long result;

	if (thread->wake_descriptor < 0)
		return;
	result = core_linux_syscall6(
		__NR_write, thread->wake_descriptor, (long)(uintptr_t)&value,
		sizeof(value), 0, 0, 0);
	if (result < 0 && result != -EAGAIN)
		__builtin_trap();
}

static void thread_wake_descriptor_close(struct kb2_core_thread *thread)
{
	if (thread->wake_descriptor >= 0) {
		if (core_linux_syscall6(__NR_close, thread->wake_descriptor,
					0, 0, 0, 0, 0) < 0)
			__builtin_trap();
		thread->wake_descriptor = -1;
	}
}

static uint64_t thread_allocate_identity(void)
{
	uint64_t next = __atomic_load_n(&core_sync_next_thread_id,
					__ATOMIC_RELAXED);

	for (;;) {
		if (!next || next == UINT64_MAX)
			__builtin_trap();
		if (__atomic_compare_exchange_n(
			    &core_sync_next_thread_id, &next, next + 1, 0,
			    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			return next;
	}
}

static void thread_private_notify(struct kb2_core_thread *thread)
{
	uint32_t *word;

	thread_lock(thread);
	word = thread->blocking_word;
	if (word && thread->blocking_interruptible) {
		(void)__atomic_add_fetch(word, 1, __ATOMIC_RELEASE);
		sync_futex_wake(word);
	}
	if (thread->parked) {
		(void)__atomic_add_fetch(&thread->park_word, 1,
					 __ATOMIC_RELEASE);
		sync_futex_wake(&thread->park_word);
	}
	if (thread->time_waiting)
		thread_wake_descriptor_signal(thread);
	thread_unlock(thread);
}

static kb2_core_status_t thread_private_wait_register(uint32_t *word)
{
	struct kb2_core_thread *thread = core_current_thread;
	kb2_core_status_t status = KB2_CORE_RUNTIME_STATUS_OK;

	if (!thread)
		return status;
	thread_lock(thread);
	if (thread->blocking_word)
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	else if (thread->stop_requested)
		status = KB2_CORE_RUNTIME_STATUS_CANCELED;
	else if (thread->interrupted)
		status = KB2_CORE_RUNTIME_STATUS_INTERRUPTED;
	else {
		thread->blocking_word = word;
		thread->blocking_interruptible = 1;
	}
	thread_unlock(thread);
	return status;
}

static void thread_private_wait_unregister(uint32_t *word)
{
	struct kb2_core_thread *thread = core_current_thread;

	if (!thread)
		return;
	thread_lock(thread);
	if (thread->blocking_word != word ||
	    !thread->blocking_interruptible) {
		thread_unlock(thread);
		__builtin_trap();
	}
	thread->blocking_word = NULL;
	thread->blocking_interruptible = 0;
	thread_unlock(thread);
}

static void sync_object_lock(struct core_sync_object *object)
{
	while (__atomic_exchange_n(&object->lock_word, 1, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#endif
	}
}

static void sync_object_unlock(struct core_sync_object *object)
{
	__atomic_store_n(&object->lock_word, 0, __ATOMIC_RELEASE);
}

static uint64_t sync_magic(uint32_t kind)
{
	switch (kind) {
	case CORE_SYNC_SPIN:
		return CORE_SYNC_SPIN_MAGIC;
	case CORE_SYNC_MUTEX:
		return CORE_SYNC_MUTEX_MAGIC;
	case CORE_SYNC_RWLOCK:
		return CORE_SYNC_RWLOCK_MAGIC;
	case CORE_SYNC_SEMAPHORE:
		return CORE_SYNC_SEMAPHORE_MAGIC;
	case CORE_SYNC_EVENT:
		return CORE_SYNC_EVENT_MAGIC;
	case CORE_SYNC_COMPLETION:
		return CORE_SYNC_COMPLETION_MAGIC;
	default:
		return 0;
	}
}

static uintptr_t sync_current_thread(void)
{
	if (core_current_thread)
		return core_current_thread->identity;
	if (core_sync_thread_id)
		return core_sync_thread_id;
	core_sync_thread_id = thread_allocate_identity();
	return core_sync_thread_id;
}

static kb2_core_status_t sync_object_validate_locked(
	struct core_binding *binding, struct core_sync_object *object,
	uint32_t kind)
{
	kb2_core_status_t status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_SYNC, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!object ||
	    !kobox_provider_arena_contains(core_arena, object, sizeof(*object)) ||
	    object->magic != sync_magic(kind) || object->kind != kind ||
	    object->binding != binding || object->order != 0 ||
	    !kobox_provider_arena_validate_owner(
		    core_arena, object, object->order, binding,
		    CORE_ALLOCATION_SYNC_OBJECT))
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_object_validate(
	struct core_binding *binding, struct core_sync_object *object,
	uint32_t kind)
{
	kb2_core_status_t status;

	core_lock();
	status = sync_object_validate_locked(binding, object, kind);
	core_unlock();
	return status;
}

static kb2_core_status_t sync_sleepable(struct core_binding *binding)
{
	struct core_binding *cpu_binding = NULL;
	struct core_binding *current;
	struct core_cpu_local_state *state;
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_SYNC, 0);
	if (status == KB2_CORE_RUNTIME_STATUS_OK && !core_sync_active)
		status = KB2_CORE_RUNTIME_STATUS_CANCELED;
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		for (current = core_bindings; current; current = current->next) {
			if (current->node_id == binding->node_id &&
			    current->interface_id ==
				    KB2_CORE_RUNTIME_INTERFACE_CPU) {
				cpu_binding = current;
				break;
			}
		}
		if (!cpu_binding)
			status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	core_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (core_cpu_context_class &&
	    core_cpu_context_class != KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD)
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	state = cpu_local_find(cpu_binding);
	if (state && !cpu_local_idle(state))
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_cpu_binding(
	struct core_binding *binding, struct core_binding **cpu_binding_out)
{
	struct core_binding *current;
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_SYNC, 0);
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
		for (current = core_bindings; current; current = current->next) {
			if (current->node_id == binding->node_id &&
			    current->interface_id ==
				    KB2_CORE_RUNTIME_INTERFACE_CPU) {
				*cpu_binding_out = current;
				status = KB2_CORE_RUNTIME_STATUS_OK;
				break;
			}
		}
	}
	core_unlock();
	return status;
}

static kb2_core_status_t sync_object_create(
	struct core_binding *binding, uint32_t kind, size_t size,
	struct core_sync_object **object_out)
{
	struct core_sync_object *object;
	kb2_core_status_t status;
	int exhausted;

	if (!object_out || !sync_magic(kind) || size < sizeof(*object) ||
	    size > CORE_PAGE_SIZE)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*object_out = NULL;
	status = binding_object_begin(
		binding, KB2_CORE_RUNTIME_INTERFACE_SYNC);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	object = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_SYNC_OBJECT);
	if (!object) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(object, CORE_PAGE_SIZE);
	object->magic = sync_magic(kind);
	object->binding = binding;
	object->kind = kind;
	core_lock();
	exhausted = core_sync_object_count == SIZE_MAX;
	if (!core_sync_active || core_state != CORE_ACTIVE ||
	    exhausted) {
		core_unlock();
		object->magic = 0;
		(void)kobox_provider_arena_release_owned(
			core_arena, object, 0, binding,
			CORE_ALLOCATION_SYNC_OBJECT);
		binding_object_end(binding);
		return exhausted ?
			       KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
			       KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	}
	object->next = core_sync_objects;
	core_sync_objects = object;
	core_sync_object_count++;
	core_unlock();
	*object_out = object;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static int sync_object_busy_locked(struct core_sync_object *object)
{
	if (object->wait_head || object->wait_tail)
		return 1;
	switch (object->kind) {
	case CORE_SYNC_SPIN:
		return ((struct kb2_core_sync_spin *)object)->held;
	case CORE_SYNC_MUTEX:
		return ((struct kb2_core_sync_mutex *)object)->owner != 0;
	case CORE_SYNC_RWLOCK: {
		struct kb2_core_sync_rwlock *rwlock =
			(struct kb2_core_sync_rwlock *)object;

		return rwlock->writer_owner || rwlock->reader_count ||
		       rwlock->readers;
	}
	default:
		return 0;
	}
}

static kb2_core_status_t sync_object_destroy(
	struct core_binding *binding, struct core_sync_object *object,
	uint32_t kind)
{
	struct core_sync_object **link;
	kb2_core_status_t status;

	core_lock();
	status = sync_object_validate_locked(binding, object, kind);
	if (status != KB2_CORE_RUNTIME_STATUS_OK) {
		core_unlock();
		return status;
	}
	sync_object_lock(object);
	if (sync_object_busy_locked(object)) {
		sync_object_unlock(object);
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_BUSY;
	}
	for (link = &core_sync_objects; *link && *link != object;
	     link = &(*link)->next)
		;
	if (!*link || !core_sync_object_count) {
		sync_object_unlock(object);
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	*link = object->next;
	core_sync_object_count--;
	object->magic = 0;
	sync_object_unlock(object);
	core_unlock();
	if (kobox_provider_arena_release_owned(
		    core_arena, object, 0, binding,
		    CORE_ALLOCATION_SYNC_OBJECT) != KOBOX_PROVIDER_ARENA_OK)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	binding_object_end(binding);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static void sync_wait_enqueue(struct core_sync_object *object,
			      struct core_sync_waiter *waiter)
{
	waiter->next = NULL;
	if (object->wait_tail)
		object->wait_tail->next = waiter;
	else
		object->wait_head = waiter;
	object->wait_tail = waiter;
}

static int sync_wait_remove(struct core_sync_object *object,
			    struct core_sync_waiter *waiter)
{
	struct core_sync_waiter **link;

	for (link = &object->wait_head; *link && *link != waiter;
	     link = &(*link)->next)
		;
	if (!*link)
		return 0;
	*link = waiter->next;
	if (object->wait_tail == waiter) {
		object->wait_tail = NULL;
		if (object->wait_head) {
			struct core_sync_waiter *tail = object->wait_head;

			while (tail->next)
				tail = tail->next;
			object->wait_tail = tail;
		}
	}
	waiter->next = NULL;
	return 1;
}

static struct core_sync_waiter *sync_wait_pop(
	struct core_sync_object *object)
{
	struct core_sync_waiter *waiter = object->wait_head;

	if (!waiter)
		return NULL;
	object->wait_head = waiter->next;
	if (!object->wait_head)
		object->wait_tail = NULL;
	waiter->next = NULL;
	return waiter;
}

static void sync_wait_complete(struct core_sync_waiter *waiter,
			       kb2_core_status_t status)
{
	__atomic_store_n(&waiter->status, status, __ATOMIC_RELAXED);
	(void)__atomic_add_fetch(&waiter->wait_word, 1, __ATOMIC_RELEASE);
	sync_futex_wake(&waiter->wait_word);
}

static kb2_core_status_t sync_wait_block(
	struct core_sync_object *object, struct core_sync_waiter *waiter,
	uint32_t wait_flags)
{
	int registered = 0;
	kb2_core_status_t terminal = KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK;

	if (wait_flags & KB2_CORE_RUNTIME_SYNC_WAIT_FLAG_INTERRUPTIBLE) {
		kb2_core_status_t status = thread_private_wait_register(
			&waiter->wait_word);

		if (status != KB2_CORE_RUNTIME_STATUS_OK) {
			sync_object_lock(object);
			if (__atomic_load_n(&waiter->status, __ATOMIC_RELAXED) ==
			    KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK) {
				if (!sync_wait_remove(object, waiter))
					status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
			} else {
				status = __atomic_load_n(&waiter->status,
						 __ATOMIC_RELAXED);
			}
			sync_object_unlock(object);
			return status;
		}
		registered = 1;
	}
	for (;;) {
		long result;
		uint32_t expected;
		expected = __atomic_load_n(&waiter->wait_word,
					   __ATOMIC_ACQUIRE);
		terminal = __atomic_load_n(&waiter->status, __ATOMIC_ACQUIRE);
		if (terminal != KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK) {
			goto complete;
		}
		result = sync_futex_wait(&waiter->wait_word, expected,
					 waiter->deadline_ns);
		sync_object_lock(object);
		terminal = __atomic_load_n(&waiter->status, __ATOMIC_RELAXED);
		if (terminal != KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK) {
			sync_object_unlock(object);
			goto complete;
		}
		if (registered && core_current_thread) {
			thread_lock(core_current_thread);
			if (core_current_thread->stop_requested)
				terminal = KB2_CORE_RUNTIME_STATUS_CANCELED;
			else if (core_current_thread->interrupted)
				terminal = KB2_CORE_RUNTIME_STATUS_INTERRUPTED;
			thread_unlock(core_current_thread);
			if (terminal != KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK) {
				if (!sync_wait_remove(object, waiter))
					terminal = KB2_CORE_RUNTIME_STATUS_CORRUPT;
				sync_object_unlock(object);
				goto complete;
			}
		}
		if (result == -ETIMEDOUT) {
			if (!sync_wait_remove(object, waiter)) {
				sync_object_unlock(object);
				terminal = KB2_CORE_RUNTIME_STATUS_CORRUPT;
				goto complete;
			}
			sync_object_unlock(object);
			terminal = KB2_CORE_RUNTIME_STATUS_TIMED_OUT;
			goto complete;
		}
		if (result < 0 && result != -EAGAIN && result != -EINTR) {
			(void)sync_wait_remove(object, waiter);
			sync_object_unlock(object);
			terminal = KB2_CORE_RUNTIME_STATUS_CORRUPT;
			goto complete;
		}
		sync_object_unlock(object);
	}

complete:
	if (registered)
		thread_private_wait_unregister(&waiter->wait_word);
	return terminal;
}

static int sync_wait_flags_valid(uint32_t wait_flags)
{
	return !(wait_flags &
		 ~KB2_CORE_RUNTIME_SYNC_WAIT_FLAG_INTERRUPTIBLE);
}

static struct core_sync_reader *sync_reader_allocate(
	struct core_binding *binding, uintptr_t owner)
{
	struct core_sync_reader *reader = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_SYNC_READER);

	if (reader) {
		bytes_zero(reader, CORE_PAGE_SIZE);
		reader->owner = owner;
	}
	return reader;
}

static kb2_core_status_t sync_reader_release(
	struct core_binding *binding, struct core_sync_reader *reader)
{
	return kobox_provider_arena_release_owned(
		       core_arena, reader, 0, binding,
		       CORE_ALLOCATION_SYNC_READER) == KOBOX_PROVIDER_ARENA_OK ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

static kb2_core_status_t sync_spin_create(
	void *binding_object, kb2_core_sync_spin_t *spin_out)
{
	struct core_sync_object *object;
	kb2_core_status_t status;

	if (!spin_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*spin_out = NULL;
	status = sync_object_create(binding_object, CORE_SYNC_SPIN,
				    sizeof(struct kb2_core_sync_spin), &object);
	if (status == KB2_CORE_RUNTIME_STATUS_OK)
		*spin_out = (struct kb2_core_sync_spin *)object;
	return status;
}

static kb2_core_status_t sync_spin_destroy(void *binding_object,
					    kb2_core_sync_spin_t spin)
{
	return sync_object_destroy(binding_object,
				   spin ? &spin->object : NULL, CORE_SYNC_SPIN);
}

static kb2_core_status_t sync_spin_lock(void *binding_object,
					 kb2_core_sync_spin_t spin)
{
	struct core_binding *binding = binding_object;
	struct core_binding *cpu_binding;
	uintptr_t owner = sync_current_thread();
	kb2_core_status_t status;

	status = sync_object_validate(binding, spin ? &spin->object : NULL,
				      CORE_SYNC_SPIN);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (__atomic_load_n(&spin->object.closing, __ATOMIC_ACQUIRE))
		return KB2_CORE_RUNTIME_STATUS_CANCELED;
	if (__atomic_load_n(&spin->held, __ATOMIC_ACQUIRE) &&
	    __atomic_load_n(&spin->owner, __ATOMIC_RELAXED) == owner)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	status = sync_cpu_binding(binding, &cpu_binding);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = cpu_counter_increment(cpu_binding, CORE_CPU_COUNTER_PREEMPT);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	while (__atomic_exchange_n(&spin->held, 1, __ATOMIC_ACQUIRE)) {
		if (__atomic_load_n(&spin->owner, __ATOMIC_RELAXED) == owner) {
			(void)cpu_counter_decrement(
				cpu_binding, CORE_CPU_COUNTER_PREEMPT);
			return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
		}
		if (__atomic_load_n(&spin->object.closing,
				    __ATOMIC_ACQUIRE)) {
			(void)cpu_counter_decrement(
				cpu_binding, CORE_CPU_COUNTER_PREEMPT);
			return KB2_CORE_RUNTIME_STATUS_CANCELED;
		}
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#endif
	}
	__atomic_store_n(&spin->owner, owner, __ATOMIC_RELAXED);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_spin_try_lock(
	void *binding_object, kb2_core_sync_spin_t spin, uint32_t *acquired_out)
{
	struct core_binding *binding = binding_object;
	struct core_binding *cpu_binding;
	uintptr_t owner = sync_current_thread();
	kb2_core_status_t status;

	if (!acquired_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*acquired_out = 0;
	status = sync_object_validate(binding, spin ? &spin->object : NULL,
				      CORE_SYNC_SPIN);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (__atomic_load_n(&spin->object.closing, __ATOMIC_ACQUIRE))
		return KB2_CORE_RUNTIME_STATUS_CANCELED;
	if (__atomic_load_n(&spin->held, __ATOMIC_ACQUIRE) &&
	    __atomic_load_n(&spin->owner, __ATOMIC_RELAXED) == owner)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	status = sync_cpu_binding(binding, &cpu_binding);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = cpu_counter_increment(cpu_binding, CORE_CPU_COUNTER_PREEMPT);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (__atomic_exchange_n(&spin->held, 1, __ATOMIC_ACQUIRE)) {
		status = cpu_counter_decrement(
			cpu_binding, CORE_CPU_COUNTER_PREEMPT);
		return status;
	}
	__atomic_store_n(&spin->owner, owner, __ATOMIC_RELAXED);
	*acquired_out = 1;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_spin_unlock(void *binding_object,
					   kb2_core_sync_spin_t spin)
{
	struct core_binding *binding = binding_object;
	struct core_binding *cpu_binding;
	kb2_core_status_t status;

	status = sync_object_validate(binding, spin ? &spin->object : NULL,
				      CORE_SYNC_SPIN);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!__atomic_load_n(&spin->held, __ATOMIC_ACQUIRE))
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	if (__atomic_load_n(&spin->owner, __ATOMIC_RELAXED) !=
	    sync_current_thread())
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	status = sync_cpu_binding(binding, &cpu_binding);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	__atomic_store_n(&spin->owner, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&spin->held, 0, __ATOMIC_RELEASE);
	return cpu_counter_decrement(cpu_binding, CORE_CPU_COUNTER_PREEMPT);
}

static kb2_core_status_t sync_mutex_create(
	void *binding_object, kb2_core_sync_mutex_t *mutex_out)
{
	struct core_sync_object *object;
	kb2_core_status_t status;

	if (!mutex_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*mutex_out = NULL;
	status = sync_object_create(binding_object, CORE_SYNC_MUTEX,
				    sizeof(struct kb2_core_sync_mutex), &object);
	if (status == KB2_CORE_RUNTIME_STATUS_OK)
		*mutex_out = (struct kb2_core_sync_mutex *)object;
	return status;
}

static kb2_core_status_t sync_mutex_destroy(void *binding_object,
					      kb2_core_sync_mutex_t mutex)
{
	return sync_object_destroy(binding_object,
				   mutex ? &mutex->object : NULL,
				   CORE_SYNC_MUTEX);
}

static kb2_core_status_t sync_mutex_lock_common(
	struct core_binding *binding, kb2_core_sync_mutex_t mutex,
	uint64_t deadline_ns, uint32_t wait_flags, int try_only,
	uint32_t *acquired_out)
{
	struct core_sync_waiter waiter = {
		.owner = sync_current_thread(),
		.deadline_ns = deadline_ns,
		.status = KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK,
		.kind = CORE_SYNC_WAIT_NORMAL,
	};
	kb2_core_status_t status;

	if (!sync_wait_flags_valid(wait_flags))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = sync_object_validate(binding, mutex ? &mutex->object : NULL,
				      CORE_SYNC_MUTEX);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!try_only) {
		status = sync_sleepable(binding);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			return status;
	}
	sync_object_lock(&mutex->object);
	if (__atomic_load_n(&mutex->object.closing, __ATOMIC_ACQUIRE)) {
		sync_object_unlock(&mutex->object);
		return KB2_CORE_RUNTIME_STATUS_CANCELED;
	}
	if (mutex->owner == waiter.owner) {
		sync_object_unlock(&mutex->object);
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	}
	if (!mutex->owner && !mutex->object.wait_head) {
		mutex->owner = waiter.owner;
		if (acquired_out)
			*acquired_out = 1;
		sync_object_unlock(&mutex->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	if (try_only) {
		if (acquired_out)
			*acquired_out = 0;
		sync_object_unlock(&mutex->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	sync_wait_enqueue(&mutex->object, &waiter);
	sync_object_unlock(&mutex->object);
	return sync_wait_block(&mutex->object, &waiter, wait_flags);
}

static kb2_core_status_t sync_mutex_lock(void *binding_object,
					   kb2_core_sync_mutex_t mutex,
					   uint32_t wait_flags)
{
	return sync_mutex_lock_common(binding_object, mutex, 0, wait_flags, 0,
				      NULL);
}

static kb2_core_status_t sync_mutex_try_lock(
	void *binding_object, kb2_core_sync_mutex_t mutex, uint32_t *acquired_out)
{
	if (!acquired_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*acquired_out = 0;
	return sync_mutex_lock_common(binding_object, mutex, 0, 0, 1,
				      acquired_out);
}

static kb2_core_status_t sync_mutex_lock_until(
	void *binding_object, kb2_core_sync_mutex_t mutex, uint64_t deadline_ns,
	uint32_t wait_flags)
{
	return sync_mutex_lock_common(binding_object, mutex, deadline_ns,
				      wait_flags, 0, NULL);
}

static kb2_core_status_t sync_mutex_unlock(void *binding_object,
					    kb2_core_sync_mutex_t mutex)
{
	struct core_sync_waiter *waiter;
	kb2_core_status_t status = sync_object_validate(
		binding_object, mutex ? &mutex->object : NULL, CORE_SYNC_MUTEX);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	sync_object_lock(&mutex->object);
	if (!mutex->owner) {
		sync_object_unlock(&mutex->object);
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	}
	if (mutex->owner != sync_current_thread()) {
		sync_object_unlock(&mutex->object);
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	}
	mutex->owner = 0;
	waiter = sync_wait_pop(&mutex->object);
	if (waiter) {
		mutex->owner = waiter->owner;
		sync_wait_complete(waiter, KB2_CORE_RUNTIME_STATUS_OK);
	}
	sync_object_unlock(&mutex->object);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static struct core_sync_reader *sync_rwlock_reader_find(
	kb2_core_sync_rwlock_t rwlock, uintptr_t owner,
	struct core_sync_reader ***link_out)
{
	struct core_sync_reader **link;

	for (link = &rwlock->readers; *link && (*link)->owner != owner;
	     link = &(*link)->next)
		;
	if (link_out)
		*link_out = link;
	return *link;
}

static void sync_rwlock_wake(kb2_core_sync_rwlock_t rwlock)
{
	struct core_sync_waiter *waiter;

	if (rwlock->writer_owner || rwlock->reader_count ||
	    !rwlock->object.wait_head)
		return;
	if (rwlock->object.wait_head->kind == CORE_SYNC_WAIT_WRITER) {
		waiter = sync_wait_pop(&rwlock->object);
		rwlock->writer_owner = waiter->owner;
		sync_wait_complete(waiter, KB2_CORE_RUNTIME_STATUS_OK);
		return;
	}
	while (rwlock->object.wait_head &&
	       rwlock->object.wait_head->kind == CORE_SYNC_WAIT_READER) {
		struct core_sync_reader *reader;

		waiter = sync_wait_pop(&rwlock->object);
		reader = waiter->auxiliary;
		if (!reader || rwlock->reader_count == SIZE_MAX)
			__builtin_trap();
		reader->next = rwlock->readers;
		rwlock->readers = reader;
		rwlock->reader_count++;
		sync_wait_complete(waiter, KB2_CORE_RUNTIME_STATUS_OK);
	}
}

static kb2_core_status_t sync_rwlock_create(
	void *binding_object, kb2_core_sync_rwlock_t *rwlock_out)
{
	struct core_sync_object *object;
	kb2_core_status_t status;

	if (!rwlock_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*rwlock_out = NULL;
	status = sync_object_create(binding_object, CORE_SYNC_RWLOCK,
				    sizeof(struct kb2_core_sync_rwlock), &object);
	if (status == KB2_CORE_RUNTIME_STATUS_OK)
		*rwlock_out = (struct kb2_core_sync_rwlock *)object;
	return status;
}

static kb2_core_status_t sync_rwlock_destroy(void *binding_object,
					       kb2_core_sync_rwlock_t rwlock)
{
	return sync_object_destroy(binding_object,
				   rwlock ? &rwlock->object : NULL,
				   CORE_SYNC_RWLOCK);
}

static kb2_core_status_t sync_rwlock_read_common(
	struct core_binding *binding, kb2_core_sync_rwlock_t rwlock,
	uint64_t deadline_ns, uint32_t wait_flags, int try_only,
	uint32_t *acquired_out)
{
	uintptr_t owner = sync_current_thread();
	struct core_sync_reader *reader = NULL;
	struct core_sync_waiter waiter = {
		.owner = owner,
		.deadline_ns = deadline_ns,
		.status = KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK,
		.kind = CORE_SYNC_WAIT_READER,
	};
	kb2_core_status_t status;
	int available;

	if (!sync_wait_flags_valid(wait_flags))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = sync_object_validate(binding, rwlock ? &rwlock->object : NULL,
				      CORE_SYNC_RWLOCK);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!try_only) {
		status = sync_sleepable(binding);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			return status;
	}
	sync_object_lock(&rwlock->object);
	if (__atomic_load_n(&rwlock->object.closing, __ATOMIC_ACQUIRE)) {
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_CANCELED;
	}
	if (rwlock->writer_owner == owner ||
	    sync_rwlock_reader_find(rwlock, owner, NULL)) {
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	}
	available = !rwlock->writer_owner && !rwlock->object.wait_head;
	if (!available && try_only) {
		if (acquired_out)
			*acquired_out = 0;
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	sync_object_unlock(&rwlock->object);
	reader = sync_reader_allocate(binding, owner);
	if (!reader)
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	waiter.auxiliary = reader;
	sync_object_lock(&rwlock->object);
	if (__atomic_load_n(&rwlock->object.closing, __ATOMIC_ACQUIRE)) {
		sync_object_unlock(&rwlock->object);
		return sync_reader_release(binding, reader) ==
				       KB2_CORE_RUNTIME_STATUS_OK ?
			       KB2_CORE_RUNTIME_STATUS_CANCELED :
			       KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	if (rwlock->writer_owner == owner ||
	    sync_rwlock_reader_find(rwlock, owner, NULL)) {
		sync_object_unlock(&rwlock->object);
		(void)sync_reader_release(binding, reader);
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	}
	available = !rwlock->writer_owner && !rwlock->object.wait_head;
	if (available) {
		if (rwlock->reader_count == SIZE_MAX) {
			sync_object_unlock(&rwlock->object);
			(void)sync_reader_release(binding, reader);
			return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
		}
		reader->next = rwlock->readers;
		rwlock->readers = reader;
		rwlock->reader_count++;
		if (acquired_out)
			*acquired_out = 1;
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	if (try_only) {
		if (acquired_out)
			*acquired_out = 0;
		sync_object_unlock(&rwlock->object);
		return sync_reader_release(binding, reader);
	}
	sync_wait_enqueue(&rwlock->object, &waiter);
	sync_object_unlock(&rwlock->object);
	status = sync_wait_block(&rwlock->object, &waiter, wait_flags);
	if (status != KB2_CORE_RUNTIME_STATUS_OK &&
	    sync_reader_release(binding, reader) != KB2_CORE_RUNTIME_STATUS_OK)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	return status;
}

static kb2_core_status_t sync_rwlock_read_lock(
	void *binding_object, kb2_core_sync_rwlock_t rwlock,
	uint32_t wait_flags)
{
	return sync_rwlock_read_common(binding_object, rwlock, 0, wait_flags, 0,
				       NULL);
}

static kb2_core_status_t sync_rwlock_read_try_lock(
	void *binding_object, kb2_core_sync_rwlock_t rwlock,
	uint32_t *acquired_out)
{
	if (!acquired_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*acquired_out = 0;
	return sync_rwlock_read_common(binding_object, rwlock, 0, 0, 1,
				       acquired_out);
}

static kb2_core_status_t sync_rwlock_read_lock_until(
	void *binding_object, kb2_core_sync_rwlock_t rwlock,
	uint64_t deadline_ns, uint32_t wait_flags)
{
	return sync_rwlock_read_common(binding_object, rwlock, deadline_ns,
				       wait_flags, 0, NULL);
}

static kb2_core_status_t sync_rwlock_read_unlock(
	void *binding_object, kb2_core_sync_rwlock_t rwlock)
{
	struct core_binding *binding = binding_object;
	struct core_sync_reader **link;
	struct core_sync_reader *reader;
	kb2_core_status_t status = sync_object_validate(
		binding, rwlock ? &rwlock->object : NULL, CORE_SYNC_RWLOCK);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	sync_object_lock(&rwlock->object);
	reader = sync_rwlock_reader_find(
		rwlock, sync_current_thread(), &link);
	if (!reader) {
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	}
	if (!rwlock->reader_count) {
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	*link = reader->next;
	rwlock->reader_count--;
	sync_rwlock_wake(rwlock);
	sync_object_unlock(&rwlock->object);
	return sync_reader_release(binding, reader);
}

static kb2_core_status_t sync_rwlock_write_common(
	struct core_binding *binding, kb2_core_sync_rwlock_t rwlock,
	uint64_t deadline_ns, uint32_t wait_flags, int try_only,
	uint32_t *acquired_out)
{
	struct core_sync_waiter waiter = {
		.owner = sync_current_thread(),
		.deadline_ns = deadline_ns,
		.status = KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK,
		.kind = CORE_SYNC_WAIT_WRITER,
	};
	kb2_core_status_t status;

	if (!sync_wait_flags_valid(wait_flags))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = sync_object_validate(binding, rwlock ? &rwlock->object : NULL,
				      CORE_SYNC_RWLOCK);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!try_only) {
		status = sync_sleepable(binding);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			return status;
	}
	sync_object_lock(&rwlock->object);
	if (__atomic_load_n(&rwlock->object.closing, __ATOMIC_ACQUIRE)) {
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_CANCELED;
	}
	if (rwlock->writer_owner == waiter.owner ||
	    sync_rwlock_reader_find(rwlock, waiter.owner, NULL)) {
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	}
	if (!rwlock->writer_owner && !rwlock->reader_count &&
	    !rwlock->object.wait_head) {
		rwlock->writer_owner = waiter.owner;
		if (acquired_out)
			*acquired_out = 1;
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	if (try_only) {
		if (acquired_out)
			*acquired_out = 0;
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	sync_wait_enqueue(&rwlock->object, &waiter);
	sync_object_unlock(&rwlock->object);
	return sync_wait_block(&rwlock->object, &waiter, wait_flags);
}

static kb2_core_status_t sync_rwlock_write_lock(
	void *binding_object, kb2_core_sync_rwlock_t rwlock,
	uint32_t wait_flags)
{
	return sync_rwlock_write_common(binding_object, rwlock, 0, wait_flags,
					0, NULL);
}

static kb2_core_status_t sync_rwlock_write_try_lock(
	void *binding_object, kb2_core_sync_rwlock_t rwlock,
	uint32_t *acquired_out)
{
	if (!acquired_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*acquired_out = 0;
	return sync_rwlock_write_common(binding_object, rwlock, 0, 0, 1,
					acquired_out);
}

static kb2_core_status_t sync_rwlock_write_lock_until(
	void *binding_object, kb2_core_sync_rwlock_t rwlock,
	uint64_t deadline_ns, uint32_t wait_flags)
{
	return sync_rwlock_write_common(binding_object, rwlock, deadline_ns,
					wait_flags, 0, NULL);
}

static kb2_core_status_t sync_rwlock_write_unlock(
	void *binding_object, kb2_core_sync_rwlock_t rwlock)
{
	kb2_core_status_t status = sync_object_validate(
		binding_object, rwlock ? &rwlock->object : NULL,
		CORE_SYNC_RWLOCK);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	sync_object_lock(&rwlock->object);
	if (!rwlock->writer_owner) {
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	}
	if (rwlock->writer_owner != sync_current_thread()) {
		sync_object_unlock(&rwlock->object);
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	}
	rwlock->writer_owner = 0;
	sync_rwlock_wake(rwlock);
	sync_object_unlock(&rwlock->object);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_semaphore_create(
	void *binding_object, uint32_t initial_count, uint32_t maximum_count,
	kb2_core_sync_semaphore_t *semaphore_out)
{
	struct core_sync_object *object;
	kb2_core_status_t status;

	if (!semaphore_out || !maximum_count ||
	    initial_count > maximum_count)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*semaphore_out = NULL;
	status = sync_object_create(binding_object, CORE_SYNC_SEMAPHORE,
				    sizeof(struct kb2_core_sync_semaphore),
				    &object);
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		struct kb2_core_sync_semaphore *semaphore =
			(struct kb2_core_sync_semaphore *)object;

		semaphore->count = initial_count;
		semaphore->maximum_count = maximum_count;
		*semaphore_out = semaphore;
	}
	return status;
}

static kb2_core_status_t sync_semaphore_destroy(
	void *binding_object, kb2_core_sync_semaphore_t semaphore)
{
	return sync_object_destroy(binding_object,
				   semaphore ? &semaphore->object : NULL,
				   CORE_SYNC_SEMAPHORE);
}

static kb2_core_status_t sync_semaphore_down_common(
	struct core_binding *binding, kb2_core_sync_semaphore_t semaphore,
	uint64_t deadline_ns, uint32_t wait_flags, int try_only,
	uint32_t *acquired_out)
{
	struct core_sync_waiter waiter = {
		.owner = sync_current_thread(),
		.deadline_ns = deadline_ns,
		.status = KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK,
		.kind = CORE_SYNC_WAIT_NORMAL,
	};
	kb2_core_status_t status;

	if (!sync_wait_flags_valid(wait_flags))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = sync_object_validate(
		binding, semaphore ? &semaphore->object : NULL,
		CORE_SYNC_SEMAPHORE);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!try_only) {
		status = sync_sleepable(binding);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			return status;
	}
	sync_object_lock(&semaphore->object);
	if (__atomic_load_n(&semaphore->object.closing, __ATOMIC_ACQUIRE)) {
		sync_object_unlock(&semaphore->object);
		return KB2_CORE_RUNTIME_STATUS_CANCELED;
	}
	if (semaphore->count && !semaphore->object.wait_head) {
		semaphore->count--;
		if (acquired_out)
			*acquired_out = 1;
		sync_object_unlock(&semaphore->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	if (try_only) {
		if (acquired_out)
			*acquired_out = 0;
		sync_object_unlock(&semaphore->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	sync_wait_enqueue(&semaphore->object, &waiter);
	sync_object_unlock(&semaphore->object);
	return sync_wait_block(&semaphore->object, &waiter, wait_flags);
}

static kb2_core_status_t sync_semaphore_down(
	void *binding_object, kb2_core_sync_semaphore_t semaphore,
	uint32_t wait_flags)
{
	return sync_semaphore_down_common(binding_object, semaphore, 0,
					  wait_flags, 0, NULL);
}

static kb2_core_status_t sync_semaphore_try_down(
	void *binding_object, kb2_core_sync_semaphore_t semaphore,
	uint32_t *acquired_out)
{
	if (!acquired_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*acquired_out = 0;
	return sync_semaphore_down_common(binding_object, semaphore, 0, 0, 1,
					  acquired_out);
}

static kb2_core_status_t sync_semaphore_down_until(
	void *binding_object, kb2_core_sync_semaphore_t semaphore,
	uint64_t deadline_ns, uint32_t wait_flags)
{
	return sync_semaphore_down_common(binding_object, semaphore,
					  deadline_ns, wait_flags, 0, NULL);
}

static kb2_core_status_t sync_semaphore_up(
	void *binding_object, kb2_core_sync_semaphore_t semaphore,
	uint32_t count)
{
	struct core_sync_waiter *waiter;
	uint32_t grant_count = 0;
	uint32_t remaining;
	kb2_core_status_t status;

	if (!count)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = sync_object_validate(
		binding_object, semaphore ? &semaphore->object : NULL,
		CORE_SYNC_SEMAPHORE);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	sync_object_lock(&semaphore->object);
	for (waiter = semaphore->object.wait_head;
	     waiter && grant_count < count; waiter = waiter->next)
		grant_count++;
	remaining = count - grant_count;
	if (remaining > semaphore->maximum_count - semaphore->count) {
		sync_object_unlock(&semaphore->object);
		return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	}
	while (grant_count--) {
		waiter = sync_wait_pop(&semaphore->object);
		if (!waiter)
			__builtin_trap();
		sync_wait_complete(waiter, KB2_CORE_RUNTIME_STATUS_OK);
	}
	semaphore->count += remaining;
	sync_object_unlock(&semaphore->object);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_event_create(
	void *binding_object, uint32_t flags, kb2_core_sync_event_t *event_out)
{
	struct core_sync_object *object;
	kb2_core_status_t status;

	if (!event_out ||
	    flags & ~(KB2_CORE_RUNTIME_SYNC_EVENT_FLAG_MANUAL_RESET |
		      KB2_CORE_RUNTIME_SYNC_EVENT_FLAG_INITIAL_SIGNALED))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*event_out = NULL;
	status = sync_object_create(binding_object, CORE_SYNC_EVENT,
				    sizeof(struct kb2_core_sync_event), &object);
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		struct kb2_core_sync_event *event =
			(struct kb2_core_sync_event *)object;

		event->manual_reset =
			!!(flags &
			   KB2_CORE_RUNTIME_SYNC_EVENT_FLAG_MANUAL_RESET);
		event->signaled =
			!!(flags &
			   KB2_CORE_RUNTIME_SYNC_EVENT_FLAG_INITIAL_SIGNALED);
		*event_out = event;
	}
	return status;
}

static kb2_core_status_t sync_event_destroy(void *binding_object,
					      kb2_core_sync_event_t event)
{
	return sync_object_destroy(binding_object,
				   event ? &event->object : NULL, CORE_SYNC_EVENT);
}

static kb2_core_status_t sync_event_wait_common(
	struct core_binding *binding, kb2_core_sync_event_t event,
	uint64_t deadline_ns, uint32_t wait_flags, int try_only,
	uint32_t *signaled_out)
{
	struct core_sync_waiter waiter = {
		.owner = sync_current_thread(),
		.deadline_ns = deadline_ns,
		.status = KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK,
		.kind = CORE_SYNC_WAIT_NORMAL,
	};
	kb2_core_status_t status;

	if (!sync_wait_flags_valid(wait_flags))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = sync_object_validate(binding, event ? &event->object : NULL,
				      CORE_SYNC_EVENT);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!try_only) {
		status = sync_sleepable(binding);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			return status;
	}
	sync_object_lock(&event->object);
	if (__atomic_load_n(&event->object.closing, __ATOMIC_ACQUIRE)) {
		sync_object_unlock(&event->object);
		return KB2_CORE_RUNTIME_STATUS_CANCELED;
	}
	if (event->signaled) {
		if (!event->manual_reset)
			event->signaled = 0;
		if (signaled_out)
			*signaled_out = 1;
		sync_object_unlock(&event->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	if (try_only) {
		if (signaled_out)
			*signaled_out = 0;
		sync_object_unlock(&event->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	sync_wait_enqueue(&event->object, &waiter);
	sync_object_unlock(&event->object);
	return sync_wait_block(&event->object, &waiter, wait_flags);
}

static kb2_core_status_t sync_event_wait(
	void *binding_object, kb2_core_sync_event_t event, uint32_t wait_flags)
{
	return sync_event_wait_common(binding_object, event, 0, wait_flags, 0,
				      NULL);
}

static kb2_core_status_t sync_event_try_wait(
	void *binding_object, kb2_core_sync_event_t event, uint32_t *signaled_out)
{
	if (!signaled_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*signaled_out = 0;
	return sync_event_wait_common(binding_object, event, 0, 0, 1,
				      signaled_out);
}

static kb2_core_status_t sync_event_wait_until(
	void *binding_object, kb2_core_sync_event_t event, uint64_t deadline_ns,
	uint32_t wait_flags)
{
	return sync_event_wait_common(binding_object, event, deadline_ns,
				      wait_flags, 0, NULL);
}

static kb2_core_status_t sync_event_signal(void *binding_object,
					     kb2_core_sync_event_t event)
{
	struct core_sync_waiter *waiter;
	kb2_core_status_t status = sync_object_validate(
		binding_object, event ? &event->object : NULL, CORE_SYNC_EVENT);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	sync_object_lock(&event->object);
	if (event->manual_reset) {
		event->signaled = 1;
		while ((waiter = sync_wait_pop(&event->object)) != NULL)
			sync_wait_complete(waiter, KB2_CORE_RUNTIME_STATUS_OK);
	} else {
		waiter = sync_wait_pop(&event->object);
		if (waiter)
			sync_wait_complete(waiter, KB2_CORE_RUNTIME_STATUS_OK);
		else
			event->signaled = 1;
	}
	sync_object_unlock(&event->object);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_event_reset(void *binding_object,
					    kb2_core_sync_event_t event)
{
	kb2_core_status_t status = sync_object_validate(
		binding_object, event ? &event->object : NULL, CORE_SYNC_EVENT);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	sync_object_lock(&event->object);
	event->signaled = 0;
	sync_object_unlock(&event->object);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_completion_create(
	void *binding_object, kb2_core_sync_completion_t *completion_out)
{
	struct core_sync_object *object;
	kb2_core_status_t status;

	if (!completion_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*completion_out = NULL;
	status = sync_object_create(binding_object, CORE_SYNC_COMPLETION,
				    sizeof(struct kb2_core_sync_completion),
				    &object);
	if (status == KB2_CORE_RUNTIME_STATUS_OK)
		*completion_out = (struct kb2_core_sync_completion *)object;
	return status;
}

static kb2_core_status_t sync_completion_destroy(
	void *binding_object, kb2_core_sync_completion_t completion)
{
	return sync_object_destroy(binding_object,
				   completion ? &completion->object : NULL,
				   CORE_SYNC_COMPLETION);
}

static kb2_core_status_t sync_completion_wait_common(
	struct core_binding *binding, kb2_core_sync_completion_t completion,
	uint64_t deadline_ns, uint32_t wait_flags, int try_only,
	uint32_t *completed_out)
{
	struct core_sync_waiter waiter = {
		.owner = sync_current_thread(),
		.deadline_ns = deadline_ns,
		.status = KB2_CORE_RUNTIME_STATUS_WOULD_BLOCK,
		.kind = CORE_SYNC_WAIT_NORMAL,
	};
	kb2_core_status_t status;

	if (!sync_wait_flags_valid(wait_flags))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = sync_object_validate(
		binding, completion ? &completion->object : NULL,
		CORE_SYNC_COMPLETION);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!try_only) {
		status = sync_sleepable(binding);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			return status;
	}
	sync_object_lock(&completion->object);
	if (__atomic_load_n(&completion->object.closing, __ATOMIC_ACQUIRE)) {
		sync_object_unlock(&completion->object);
		return KB2_CORE_RUNTIME_STATUS_CANCELED;
	}
	if (completion->complete_all || completion->count) {
		if (!completion->complete_all)
			completion->count--;
		if (completed_out)
			*completed_out = 1;
		sync_object_unlock(&completion->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	if (try_only) {
		if (completed_out)
			*completed_out = 0;
		sync_object_unlock(&completion->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	sync_wait_enqueue(&completion->object, &waiter);
	sync_object_unlock(&completion->object);
	return sync_wait_block(&completion->object, &waiter, wait_flags);
}

static kb2_core_status_t sync_completion_wait(
	void *binding_object, kb2_core_sync_completion_t completion,
	uint32_t wait_flags)
{
	return sync_completion_wait_common(binding_object, completion, 0,
					   wait_flags, 0, NULL);
}

static kb2_core_status_t sync_completion_try_wait(
	void *binding_object, kb2_core_sync_completion_t completion,
	uint32_t *completed_out)
{
	if (!completed_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*completed_out = 0;
	return sync_completion_wait_common(binding_object, completion, 0, 0, 1,
					   completed_out);
}

static kb2_core_status_t sync_completion_wait_until(
	void *binding_object, kb2_core_sync_completion_t completion,
	uint64_t deadline_ns, uint32_t wait_flags)
{
	return sync_completion_wait_common(binding_object, completion,
					   deadline_ns, wait_flags, 0, NULL);
}

static kb2_core_status_t sync_completion_complete(
	void *binding_object, kb2_core_sync_completion_t completion)
{
	struct core_sync_waiter *waiter;
	kb2_core_status_t status = sync_object_validate(
		binding_object, completion ? &completion->object : NULL,
		CORE_SYNC_COMPLETION);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	sync_object_lock(&completion->object);
	if (completion->complete_all) {
		sync_object_unlock(&completion->object);
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	waiter = sync_wait_pop(&completion->object);
	if (waiter)
		sync_wait_complete(waiter, KB2_CORE_RUNTIME_STATUS_OK);
	else if (completion->count == UINT32_MAX) {
		sync_object_unlock(&completion->object);
		return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	} else {
		completion->count++;
	}
	sync_object_unlock(&completion->object);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_completion_complete_all(
	void *binding_object, kb2_core_sync_completion_t completion)
{
	struct core_sync_waiter *waiter;
	kb2_core_status_t status = sync_object_validate(
		binding_object, completion ? &completion->object : NULL,
		CORE_SYNC_COMPLETION);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	sync_object_lock(&completion->object);
	completion->complete_all = 1;
	completion->count = 0;
	while ((waiter = sync_wait_pop(&completion->object)) != NULL)
		sync_wait_complete(waiter, KB2_CORE_RUNTIME_STATUS_OK);
	sync_object_unlock(&completion->object);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t sync_completion_reinit(
	void *binding_object, kb2_core_sync_completion_t completion)
{
	kb2_core_status_t status = sync_object_validate(
		binding_object, completion ? &completion->object : NULL,
		CORE_SYNC_COMPLETION);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	sync_object_lock(&completion->object);
	if (completion->object.wait_head) {
		sync_object_unlock(&completion->object);
		return KB2_CORE_RUNTIME_STATUS_BUSY;
	}
	completion->complete_all = 0;
	completion->count = 0;
	sync_object_unlock(&completion->object);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static int sync_lifecycle_init(const struct kobox_module_context *context)
{
	(void)context;
	if (core_sync_active || core_sync_objects || core_sync_object_count)
		return -1;
	core_sync_active = 1;
	return 0;
}

static int sync_lifecycle_quiesce(const struct kobox_module_context *context)
{
	struct core_sync_object *object;

	(void)context;
	core_lock();
	if (!core_sync_active) {
		core_unlock();
		return -1;
	}
	core_sync_active = 0;
	for (object = core_sync_objects; object; object = object->next) {
		struct core_sync_waiter *waiter;

		sync_object_lock(object);
		__atomic_store_n(&object->closing, 1, __ATOMIC_RELEASE);
		while ((waiter = sync_wait_pop(object)) != NULL)
			sync_wait_complete(waiter,
					   KB2_CORE_RUNTIME_STATUS_CANCELED);
		sync_object_unlock(object);
	}
	core_unlock();
	return 0;
}

static int sync_lifecycle_cleanup(const struct kobox_module_context *context)
{
	int empty;

	(void)context;
	core_lock();
	empty = !core_sync_active && !core_sync_objects &&
		!core_sync_object_count;
	core_unlock();
	return empty ? 0 : -1;
}

static int thread_name_valid(const char *name, size_t length)
{
	size_t index;

	if ((!name && length) || length > KB2_CORE_RUNTIME_NAME_MAXIMUM_BYTES)
		return 0;
	for (index = 0; index < length; index++) {
		if (!name[index])
			return 0;
	}
	return 1;
}

static kb2_core_status_t thread_native_status(long status)
{
	long error = status < 0 ? -status : status;

	switch (error) {
	case 0:
		return KB2_CORE_RUNTIME_STATUS_OK;
	case EINVAL:
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	case ESRCH:
		return KB2_CORE_RUNTIME_STATUS_STALE;
	case EPERM:
	case EACCES:
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	case EAGAIN:
	case ENOMEM:
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	default:
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
}

static int thread_list_contains_locked(const struct kb2_core_thread *thread)
{
	const struct kb2_core_thread *current;

	for (current = core_threads; current; current = current->next) {
		if (current == thread)
			return 1;
	}
	return 0;
}

static int thread_remove_locked(struct kb2_core_thread *thread)
{
	struct kb2_core_thread **link;

	for (link = &core_threads; *link && *link != thread;
	     link = &(*link)->next)
		;
	if (!*link || !core_thread_count)
		return 0;
	*link = thread->next;
	thread->next = NULL;
	core_thread_count--;
	return 1;
}

static kb2_core_status_t thread_collect_detached(void)
{
	for (;;) {
		struct kb2_core_thread *thread;
		struct core_binding *binding;

		core_lock();
		for (thread = core_threads; thread; thread = thread->next) {
			thread_lock(thread);
			if (thread->detached &&
			    thread->state == CORE_THREAD_EXITED)
				break;
			thread_unlock(thread);
		}
		if (!thread) {
			core_unlock();
			return KB2_CORE_RUNTIME_STATUS_OK;
		}
		if (!thread->ownership_released || !thread_remove_locked(thread)) {
			thread_unlock(thread);
			core_unlock();
			return KB2_CORE_RUNTIME_STATUS_CORRUPT;
		}
		binding = thread->binding;
		thread->state = CORE_THREAD_JOINED;
		thread_wake_descriptor_close(thread);
		thread->magic = 0;
		thread_unlock(thread);
		core_unlock();
		if (kobox_provider_arena_release_owned(
			    core_arena, thread, 0, binding,
			    CORE_ALLOCATION_THREAD) != KOBOX_PROVIDER_ARENA_OK)
			return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
}

static kb2_core_status_t thread_validate(struct core_binding *binding,
					  struct kb2_core_thread *thread,
					  int allow_exited)
{
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_THREAD, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		goto out;
	if (thread && thread->borrowed) {
		if (thread->magic != CORE_THREAD_MAGIC ||
		    thread->generation != core_generation ||
		    core_current_thread != thread ||
		    (thread != &core_root_thread && thread->binding != binding))
			status = KB2_CORE_RUNTIME_STATUS_OWNER;
		goto out;
	}
	if (!thread ||
	    !kobox_provider_arena_contains(core_arena, thread,
					  sizeof(*thread)) ||
	    thread->magic != CORE_THREAD_MAGIC || thread->borrowed ||
	    thread->binding != binding ||
	    thread->generation != core_generation ||
	    !thread_list_contains_locked(thread) ||
	    !kobox_provider_arena_validate_owner(
		    core_arena, thread, 0, binding, CORE_ALLOCATION_THREAD)) {
		status = KB2_CORE_RUNTIME_STATUS_OWNER;
		goto out;
	}
	thread_lock(thread);
	if (thread->detached || thread->state == CORE_THREAD_JOINED ||
	    (!allow_exited && thread->state >= CORE_THREAD_EXITING))
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	thread_unlock(thread);
out:
	core_unlock();
	return status;
}

static kb2_core_status_t thread_cpu_mask_build(
	const uint64_t *logical_words, size_t logical_word_count,
	uint64_t native_words[CORE_NATIVE_CPU_WORDS], uint32_t *first_cpu_out)
{
	size_t expected_words = (core_cpu_count + 63u) / 64u;
	uint32_t first = UINT32_MAX;
	uint32_t logical_cpu;

	bytes_zero(native_words,
		   sizeof(uint64_t) * CORE_NATIVE_CPU_WORDS);
	if (!logical_words && !logical_word_count) {
		for (logical_cpu = 0; logical_cpu < core_cpu_count;
		     logical_cpu++) {
			uint32_t native_cpu = core_native_cpu_ids[logical_cpu];

			native_words[native_cpu / 64] |=
				UINT64_C(1) << (native_cpu % 64);
		}
		*first_cpu_out = 0;
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	if (!logical_words || logical_word_count != expected_words)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	if (core_cpu_count % 64 &&
	    logical_words[expected_words - 1] &
		    ~((UINT64_C(1) << (core_cpu_count % 64)) - 1))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	for (logical_cpu = 0; logical_cpu < core_cpu_count; logical_cpu++) {
		uint32_t native_cpu;

		if (!(logical_words[logical_cpu / 64] &
		      (UINT64_C(1) << (logical_cpu % 64))))
			continue;
		if (first == UINT32_MAX)
			first = logical_cpu;
		native_cpu = core_native_cpu_ids[logical_cpu];
		native_words[native_cpu / 64] |=
			UINT64_C(1) << (native_cpu % 64);
	}
	if (first == UINT32_MAX)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*first_cpu_out = first;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t thread_set_native_name(
	struct kb2_core_thread *thread, const char *name, size_t length)
{
	char native_name[16];
	size_t native_length = length < sizeof(native_name) - 1 ?
				       length : sizeof(native_name) - 1;
	int result;

	bytes_zero(native_name, sizeof(native_name));
	if (native_length)
		bytes_copy(native_name, name, native_length);
	result = pthread_setname_np(thread->native, native_name);
	return thread_native_status(result);
}

static kb2_core_status_t thread_set_native_priority(
	struct kb2_core_thread *thread, int32_t priority)
{
	long result;

	if (priority < (int32_t)KB2_CORE_RUNTIME_THREAD_PRIORITY_HIGHEST ||
	    priority > (int32_t)KB2_CORE_RUNTIME_THREAD_PRIORITY_LOWEST)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	result = core_linux_syscall6(
		__NR_setpriority, PRIO_PROCESS, thread->native_tid,
		priority - KB2_CORE_RUNTIME_THREAD_PRIORITY_DEFAULT, 0, 0, 0);
	return thread_native_status(result);
}

static kb2_core_status_t thread_park_internal(
	struct kb2_core_thread *thread, uint64_t deadline_ns)
{
	for (;;) {
		uint32_t expected;
		long result;

		thread_lock(thread);
		if (thread->stop_requested) {
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_CANCELED;
		}
		if (thread->interrupted) {
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_INTERRUPTED;
		}
		if (thread->unpark_permit) {
			thread->unpark_permit = 0;
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_OK;
		}
		if (thread->wake_pending) {
			thread->wake_pending = 0;
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_OK;
		}
		thread->parked = 1;
		expected = __atomic_load_n(&thread->park_word,
					   __ATOMIC_ACQUIRE);
		thread_unlock(thread);
		result = sync_futex_wait(&thread->park_word, expected,
					 deadline_ns);
		thread_lock(thread);
		thread->parked = 0;
		if (thread->stop_requested) {
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_CANCELED;
		}
		if (thread->interrupted) {
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_INTERRUPTED;
		}
		if (thread->unpark_permit) {
			thread->unpark_permit = 0;
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_OK;
		}
		if (thread->wake_pending) {
			thread->wake_pending = 0;
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_OK;
		}
		thread_unlock(thread);
		if (result == -ETIMEDOUT)
			return KB2_CORE_RUNTIME_STATUS_TIMED_OUT;
		if (result < 0 && result != -EAGAIN && result != -EINTR)
			return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
}

static int thread_owns_lock(uint64_t identity)
{
	struct core_sync_object *object;
	int owned = 0;

	core_lock();
	for (object = core_sync_objects; object && !owned;
	     object = object->next) {
		sync_object_lock(object);
		switch (object->kind) {
		case CORE_SYNC_SPIN:
			owned = ((struct kb2_core_sync_spin *)object)->owner ==
				identity;
			break;
		case CORE_SYNC_MUTEX:
			owned = ((struct kb2_core_sync_mutex *)object)->owner ==
				identity;
			break;
		case CORE_SYNC_RWLOCK: {
			struct kb2_core_sync_rwlock *rwlock =
				(struct kb2_core_sync_rwlock *)object;
			struct core_sync_reader *reader;

			owned = rwlock->writer_owner == identity;
			for (reader = rwlock->readers; reader && !owned;
			     reader = reader->next)
				owned = reader->owner == identity;
			break;
		}
		default:
			break;
		}
		sync_object_unlock(object);
	}
	core_unlock();
	return owned;
}

static void thread_finish(struct kb2_core_thread *thread, int32_t status)
{
	int release_ownership = 0;
	struct core_binding *binding = thread->binding;

	if (core_rcu_read_depth || core_cpu_local_states ||
	    thread_owns_lock(thread->identity))
		__builtin_trap();
	thread_lock(thread);
	thread->exit_status = status;
	thread->state = CORE_THREAD_EXITING;
	if (thread->detached && !thread->ownership_released) {
		thread->ownership_released = 1;
		release_ownership = 1;
	}
	thread_unlock(thread);
	if (release_ownership)
		binding_object_end(binding);
	thread_lock(thread);
	thread->state = CORE_THREAD_EXITED;
	(void)__atomic_add_fetch(&thread->done_word, 1, __ATOMIC_RELEASE);
	sync_futex_wake(&thread->done_word);
	thread_unlock(thread);
}

static void *thread_native_entry(void *argument)
{
	struct kb2_core_thread *thread = argument;
	uint32_t expected;
	int32_t result = -1;

	core_current_thread = thread;
	core_sync_thread_id = thread->identity;
	core_cpu_id = thread->logical_cpu;
	core_cpu_context_class = KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD;
	thread->native_tid = (uint32_t)core_linux_syscall6(
		__NR_gettid, 0, 0, 0, 0, 0, 0);
	(void)__atomic_add_fetch(&thread->startup_word, 1, __ATOMIC_RELEASE);
	sync_futex_wake(&thread->startup_word);
	for (;;) {
		expected = __atomic_load_n(&thread->startup_release_word,
					   __ATOMIC_ACQUIRE);
		if (expected)
			break;
		(void)sync_futex_wait(&thread->startup_release_word, expected, 0);
	}
	thread_lock(thread);
	if (!thread->startup_abort)
		thread->state = CORE_THREAD_RUNNING;
	thread_unlock(thread);
	if (!thread->startup_abort &&
	    (!thread->start_parked ||
	     thread_park_internal(thread, 0) == KB2_CORE_RUNTIME_STATUS_OK))
		result = thread->entry(thread->argument);
	thread_finish(thread, result);
	core_current_thread = NULL;
	core_sync_thread_id = 0;
	core_cpu_id = 0;
	core_cpu_context_class = 0;
	return NULL;
}

static kb2_core_status_t thread_current(void *binding_object,
					 kb2_core_thread_t *thread_out)
{
	struct core_binding *binding = binding_object;
	kb2_core_status_t status;

	if (!thread_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*thread_out = NULL;
	core_lock();
	status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_THREAD, 0);
	if (status == KB2_CORE_RUNTIME_STATUS_OK && !core_current_thread)
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	if (status == KB2_CORE_RUNTIME_STATUS_OK &&
	    core_current_thread != &core_root_thread &&
	    core_current_thread->binding != binding)
		status = KB2_CORE_RUNTIME_STATUS_OWNER;
	if (status == KB2_CORE_RUNTIME_STATUS_OK)
		*thread_out = core_current_thread;
	core_unlock();
	return status;
}

static kb2_core_status_t thread_create(
	void *binding_object, kb2_core_thread_entry_fn thread_entry,
	void *argument, const char *name, size_t name_length, size_t stack_size,
	const uint64_t *cpu_mask_words, size_t cpu_mask_word_count,
	uint32_t flags, kb2_core_thread_t *thread_out)
{
	struct core_binding *binding = binding_object;
	uint64_t native_mask[CORE_NATIVE_CPU_WORDS];
	struct kb2_core_thread *thread;
	pthread_attr_t attributes;
	kb2_core_status_t status;
	uint32_t first_cpu;
	int result;
	int attributes_initialized = 0;
	int created = 0;

	if (!thread_out || !thread_entry || !thread_name_valid(name, name_length) ||
	    flags & ~(KB2_CORE_RUNTIME_THREAD_FLAG_START_PARKED |
		      KB2_CORE_RUNTIME_THREAD_FLAG_HIGH_PRIORITY) ||
	    (stack_size &&
	     (stack_size < KB2_CORE_RUNTIME_THREAD_STACK_MINIMUM_BYTES ||
	      stack_size % KB2_CORE_RUNTIME_THREAD_STACK_ALIGNMENT)))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*thread_out = NULL;
	status = thread_collect_detached();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = thread_cpu_mask_build(cpu_mask_words, cpu_mask_word_count,
				       native_mask, &first_cpu);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = binding_object_begin(
		binding, KB2_CORE_RUNTIME_INTERFACE_THREAD);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_THREAD);
	if (!thread) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(thread, CORE_PAGE_SIZE);
	thread->wake_descriptor = -1;
	thread->wake_descriptor = thread_wake_descriptor_create();
	if (thread->wake_descriptor < 0) {
		binding_object_end(binding);
		(void)kobox_provider_arena_release_owned(
			core_arena, thread, 0, binding, CORE_ALLOCATION_THREAD);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	thread->magic = CORE_THREAD_MAGIC;
	thread->binding = binding;
	thread->entry = thread_entry;
	thread->argument = argument;
	thread->identity = thread_allocate_identity();
	thread->generation = core_generation;
	thread->state = CORE_THREAD_STARTING;
	thread->start_parked =
		!!(flags & KB2_CORE_RUNTIME_THREAD_FLAG_START_PARKED);
	thread->priority = flags & KB2_CORE_RUNTIME_THREAD_FLAG_HIGH_PRIORITY ?
				   KB2_CORE_RUNTIME_THREAD_PRIORITY_HIGHEST :
				   KB2_CORE_RUNTIME_THREAD_PRIORITY_DEFAULT;
	thread->logical_cpu = first_cpu;
	if (name_length)
		bytes_copy(thread->name, name, name_length);
	result = pthread_attr_init(&attributes);
	if (result) {
		status = thread_native_status(result);
		goto fail;
	}
	attributes_initialized = 1;
	if (stack_size) {
		result = pthread_attr_setstacksize(&attributes, stack_size);
		if (result) {
			status = thread_native_status(result);
			goto fail;
		}
	}
	core_lock();
	if (!core_thread_active || core_thread_count == SIZE_MAX) {
		core_unlock();
		status = core_thread_count == SIZE_MAX ?
				 KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
				 KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		goto fail;
	}
	thread->next = core_threads;
	core_threads = thread;
	core_thread_count++;
	core_unlock();
	result = pthread_create(&thread->native, &attributes,
				thread_native_entry, thread);
	if (result) {
		status = thread_native_status(result);
		core_lock();
		if (!thread_remove_locked(thread))
			status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
		core_unlock();
		goto fail;
	}
	created = 1;
	(void)pthread_attr_destroy(&attributes);
	attributes_initialized = 0;
	while (!__atomic_load_n(&thread->startup_word, __ATOMIC_ACQUIRE)) {
		uint32_t expected = __atomic_load_n(
			&thread->startup_word, __ATOMIC_RELAXED);

		(void)sync_futex_wait(&thread->startup_word, expected, 0);
	}
	result = pthread_setaffinity_np(thread->native, sizeof(native_mask),
					(const cpu_set_t *)native_mask);
	status = thread_native_status(result);
	if (status == KB2_CORE_RUNTIME_STATUS_OK)
		status = thread_set_native_name(thread, name, name_length);
	if (status == KB2_CORE_RUNTIME_STATUS_OK &&
	    thread->priority != KB2_CORE_RUNTIME_THREAD_PRIORITY_DEFAULT)
		status = thread_set_native_priority(thread, thread->priority);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		thread->startup_abort = 1;
	(void)__atomic_add_fetch(&thread->startup_release_word, 1,
				 __ATOMIC_RELEASE);
	sync_futex_wake(&thread->startup_release_word);
	if (status != KB2_CORE_RUNTIME_STATUS_OK) {
		(void)pthread_join(thread->native, NULL);
		core_lock();
		if (!thread_remove_locked(thread))
			status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
		core_unlock();
		binding_object_end(binding);
		thread_wake_descriptor_close(thread);
		thread->magic = 0;
		(void)kobox_provider_arena_release_owned(
			core_arena, thread, 0, binding, CORE_ALLOCATION_THREAD);
		return status;
	}
	*thread_out = thread;
	return KB2_CORE_RUNTIME_STATUS_OK;

fail:
	if (attributes_initialized)
		(void)pthread_attr_destroy(&attributes);
	if (created)
		(void)pthread_join(thread->native, NULL);
	thread_wake_descriptor_close(thread);
	thread->magic = 0;
	(void)kobox_provider_arena_release_owned(
		core_arena, thread, 0, binding, CORE_ALLOCATION_THREAD);
	binding_object_end(binding);
	return status;
}

static kb2_core_status_t thread_join(void *binding_object,
				      kb2_core_thread_t thread,
				      uint64_t deadline_ns,
				      int32_t *exit_status_out)
{
	struct core_binding *binding = binding_object;
	kb2_core_status_t status;
	uint32_t expected;
	long result;
	int32_t exit_status;

	if (!exit_status_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = thread_validate(binding, thread, 1);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (thread == core_current_thread)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	thread_lock(thread);
	if (thread->join_in_progress || thread->detached) {
		thread_unlock(thread);
		return KB2_CORE_RUNTIME_STATUS_BUSY;
	}
	thread->join_in_progress = 1;
	thread_unlock(thread);
	for (;;) {
		thread_lock(thread);
		if (thread->state == CORE_THREAD_EXITED) {
			exit_status = thread->exit_status;
			thread_unlock(thread);
			break;
		}
		expected = __atomic_load_n(&thread->done_word,
					   __ATOMIC_ACQUIRE);
		thread_unlock(thread);
		result = sync_futex_wait(&thread->done_word, expected,
					 deadline_ns);
		if (result == -ETIMEDOUT) {
			thread_lock(thread);
			if (thread->state == CORE_THREAD_EXITED) {
				exit_status = thread->exit_status;
				thread_unlock(thread);
				break;
			}
			thread->join_in_progress = 0;
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_TIMED_OUT;
		}
		if (result < 0 && result != -EAGAIN && result != -EINTR) {
			thread_lock(thread);
			thread->join_in_progress = 0;
			thread_unlock(thread);
			return KB2_CORE_RUNTIME_STATUS_CORRUPT;
		}
	}
	status = thread_native_status(pthread_join(thread->native, NULL));
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	core_lock();
	if (!thread_remove_locked(thread)) {
		core_unlock();
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	core_unlock();
	thread->state = CORE_THREAD_JOINED;
	thread_wake_descriptor_close(thread);
	thread->magic = 0;
	binding_object_end(binding);
	status = kobox_provider_arena_release_owned(
			 core_arena, thread, 0, binding,
			 CORE_ALLOCATION_THREAD) == KOBOX_PROVIDER_ARENA_OK ?
			 KB2_CORE_RUNTIME_STATUS_OK :
			 KB2_CORE_RUNTIME_STATUS_CORRUPT;
	if (status == KB2_CORE_RUNTIME_STATUS_OK)
		*exit_status_out = exit_status;
	return status;
}

static kb2_core_status_t thread_detach(void *binding_object,
					kb2_core_thread_t thread)
{
	struct core_binding *binding = binding_object;
	kb2_core_status_t status = thread_validate(binding, thread, 1);
	int release_ownership = 0;

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (thread == core_current_thread)
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	thread_lock(thread);
	if (thread->join_in_progress || thread->detached) {
		thread_unlock(thread);
		return KB2_CORE_RUNTIME_STATUS_BUSY;
	}
	status = thread_native_status(pthread_detach(thread->native));
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		thread->detached = 1;
		if ((thread->state == CORE_THREAD_EXITING ||
		     thread->state == CORE_THREAD_EXITED) &&
		    !thread->ownership_released) {
			thread->ownership_released = 1;
			release_ownership = 1;
		}
	}
	thread_unlock(thread);
	if (release_ownership)
		binding_object_end(binding);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	return thread_collect_detached();
}

static kb2_core_status_t thread_request_stop(void *binding_object,
					      kb2_core_thread_t thread)
{
	kb2_core_status_t status = thread_validate(binding_object, thread, 1);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread_lock(thread);
	thread->stop_requested = 1;
	thread_unlock(thread);
	thread_private_notify(thread);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t thread_stop_requested(void *binding_object,
					       kb2_core_thread_t thread,
					       uint32_t *requested_out)
{
	kb2_core_status_t status;

	if (!requested_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = thread_validate(binding_object, thread, 1);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread_lock(thread);
	*requested_out = thread->stop_requested;
	thread_unlock(thread);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t thread_interrupt(void *binding_object,
					   kb2_core_thread_t thread)
{
	kb2_core_status_t status = thread_validate(binding_object, thread, 1);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread_lock(thread);
	thread->interrupted = 1;
	thread_unlock(thread);
	thread_private_notify(thread);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t thread_is_interrupted(void *binding_object,
					       kb2_core_thread_t thread,
					       uint32_t *interrupted_out)
{
	kb2_core_status_t status;

	if (!interrupted_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = thread_validate(binding_object, thread, 1);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread_lock(thread);
	*interrupted_out = thread->interrupted;
	thread_unlock(thread);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t thread_clear_interrupt(void *binding_object,
						kb2_core_thread_t thread)
{
	kb2_core_status_t status = thread_validate(binding_object, thread, 1);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (thread != core_current_thread)
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	thread_lock(thread);
	thread->interrupted = 0;
	thread_unlock(thread);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t thread_park(void *binding_object,
				      kb2_core_thread_t thread,
				      uint64_t deadline_ns)
{
	kb2_core_status_t status = thread_validate(binding_object, thread, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (thread != core_current_thread)
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	return thread_park_internal(thread, deadline_ns);
}

static kb2_core_status_t thread_unpark(void *binding_object,
					kb2_core_thread_t thread)
{
	kb2_core_status_t status = thread_validate(binding_object, thread, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread_lock(thread);
	thread->unpark_permit = 1;
	if (thread->parked) {
		(void)__atomic_add_fetch(&thread->park_word, 1,
					 __ATOMIC_RELEASE);
		sync_futex_wake(&thread->park_word);
	}
	thread_unlock(thread);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t thread_wake(void *binding_object,
				      kb2_core_thread_t thread)
{
	kb2_core_status_t status = thread_validate(binding_object, thread, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread_lock(thread);
	if (thread->parked) {
		thread->wake_pending = 1;
		(void)__atomic_add_fetch(&thread->park_word, 1,
					 __ATOMIC_RELEASE);
		sync_futex_wake(&thread->park_word);
	}
	thread_unlock(thread);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t thread_yield(void *binding_object)
{
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(binding_object,
					 KB2_CORE_RUNTIME_INTERFACE_THREAD, 0);
	core_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	return thread_native_status(
		core_linux_syscall6(__NR_sched_yield, 0, 0, 0, 0, 0, 0));
}

static kb2_core_status_t thread_set_name(void *binding_object,
					  kb2_core_thread_t thread,
					  const char *name,
					  size_t name_length)
{
	kb2_core_status_t status;

	if (!thread_name_valid(name, name_length))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = thread_validate(binding_object, thread, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = thread_set_native_name(thread, name, name_length);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread_lock(thread);
	bytes_zero(thread->name, sizeof(thread->name));
	if (name_length)
		bytes_copy(thread->name, name, name_length);
	thread_unlock(thread);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t thread_set_affinity(
	void *binding_object, kb2_core_thread_t thread,
	const uint64_t *cpu_mask_words, size_t cpu_mask_word_count)
{
	uint64_t native_mask[CORE_NATIVE_CPU_WORDS];
	uint32_t first_cpu;
	kb2_core_status_t status = thread_validate(binding_object, thread, 0);
	int result;

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = thread_cpu_mask_build(cpu_mask_words, cpu_mask_word_count,
				       native_mask, &first_cpu);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	result = pthread_setaffinity_np(thread->native, sizeof(native_mask),
					(const cpu_set_t *)native_mask);
	status = thread_native_status(result);
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		thread_lock(thread);
		thread->logical_cpu = first_cpu;
		thread_unlock(thread);
		if (thread == core_current_thread)
			core_cpu_id = first_cpu;
	}
	return status;
}

static kb2_core_status_t thread_set_priority(void *binding_object,
					      kb2_core_thread_t thread,
					      int32_t priority)
{
	kb2_core_status_t status = thread_validate(binding_object, thread, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = thread_set_native_priority(thread, priority);
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		thread_lock(thread);
		thread->priority = priority;
		thread_unlock(thread);
	}
	return status;
}

static int thread_lifecycle_init(const struct kobox_module_context *context)
{
	long tid;

	if (!context || core_thread_active || core_threads || core_thread_count ||
	    core_current_thread || core_root_thread.magic)
		return -1;
	tid = core_linux_syscall6(__NR_gettid, 0, 0, 0, 0, 0, 0);
	if (tid <= 0 || (unsigned long)tid > UINT32_MAX)
		return -1;
	bytes_zero(&core_root_thread, sizeof(core_root_thread));
	core_root_thread.wake_descriptor = -1;
	core_root_thread.wake_descriptor = thread_wake_descriptor_create();
	if (core_root_thread.wake_descriptor < 0)
		return -1;
	core_root_thread.magic = CORE_THREAD_MAGIC;
	core_root_thread.native = pthread_self();
	core_root_thread.identity = thread_allocate_identity();
	core_root_thread.generation = context->generation;
	core_root_thread.state = CORE_THREAD_RUNNING;
	core_root_thread.borrowed = 1;
	core_root_thread.native_tid = (uint32_t)tid;
	core_root_thread.priority = KB2_CORE_RUNTIME_THREAD_PRIORITY_DEFAULT;
	core_current_thread = &core_root_thread;
	core_sync_thread_id = core_root_thread.identity;
	core_thread_active = 1;
	return 0;
}

static int thread_lifecycle_quiesce(const struct kobox_module_context *context)
{
	struct kb2_core_thread *thread;

	(void)context;
	core_lock();
	if (!core_thread_active) {
		core_unlock();
		return -1;
	}
	core_thread_active = 0;
	for (thread = core_threads; thread; thread = thread->next) {
		thread_lock(thread);
		thread->stop_requested = 1;
		thread->interrupted = 1;
		thread->wake_pending = 1;
		thread_unlock(thread);
		thread_private_notify(thread);
	}
	core_unlock();
	for (;;) {
		uint32_t expected;

		core_lock();
		thread = core_threads;
		core_unlock();
		if (!thread)
			break;
		for (;;) {
			int exited;

			thread_lock(thread);
			exited = thread->state == CORE_THREAD_EXITED;
			expected = __atomic_load_n(&thread->done_word,
						   __ATOMIC_RELAXED);
			thread_unlock(thread);
			if (exited)
				break;
			(void)sync_futex_wait(&thread->done_word, expected, 0);
		}
		thread_lock(thread);
		if (!thread->detached &&
		    pthread_join(thread->native, NULL)) {
			thread_unlock(thread);
			return -1;
		}
		if (!thread->ownership_released) {
			thread->ownership_released = 1;
			thread_unlock(thread);
			binding_object_end(thread->binding);
		} else {
			thread_unlock(thread);
		}
		core_lock();
		if (!thread_remove_locked(thread)) {
			core_unlock();
			return -1;
		}
		core_unlock();
		thread->magic = 0;
		thread_wake_descriptor_close(thread);
		if (kobox_provider_arena_release_owned(
			    core_arena, thread, 0, thread->binding,
			    CORE_ALLOCATION_THREAD) != KOBOX_PROVIDER_ARENA_OK)
			return -1;
	}
	return core_thread_count ? -1 : 0;
}

static int thread_lifecycle_cleanup(const struct kobox_module_context *context)
{
	(void)context;
	if (core_thread_active || core_threads || core_thread_count)
		return -1;
	core_current_thread = NULL;
	core_sync_thread_id = 0;
	thread_wake_descriptor_close(&core_root_thread);
	bytes_zero(&core_root_thread, sizeof(core_root_thread));
	return 0;
}

struct core_poll_descriptor {
	int descriptor;
	short events;
	short returned_events;
};

static void time_lock(void)
{
	while (__atomic_exchange_n(&core_time_lock_word, 1, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#endif
	}
}

static void time_unlock(void)
{
	__atomic_store_n(&core_time_lock_word, 0, __ATOMIC_RELEASE);
}

static void time_descriptor_signal(int descriptor)
{
	uint64_t value = 1;
	long result = core_linux_syscall6(
		__NR_write, descriptor, (long)(uintptr_t)&value,
		sizeof(value), 0, 0, 0);

	if (result < 0 && result != -EAGAIN)
		__builtin_trap();
}

static void time_cpu_signal(uint32_t cpu_id)
{
	if (cpu_id < core_cpu_count &&
	    core_time_cpus[cpu_id].dispatcher_descriptor >= 0)
		time_descriptor_signal(
			core_time_cpus[cpu_id].dispatcher_descriptor);
}

static struct core_binding *time_thread_binding(uint32_t node_id)
{
	struct core_binding *binding;

	core_lock();
	for (binding = core_bindings; binding; binding = binding->next) {
		if (binding->node_id == node_id &&
		    binding->interface_id == KB2_CORE_RUNTIME_INTERFACE_THREAD)
			break;
	}
	core_unlock();
	return binding;
}

static void time_descriptor_drain(int descriptor);

static int time_timer_list_contains_locked(
	const struct kb2_core_time_timer *timer)
{
	const struct kb2_core_time_timer *current;

	for (current = core_timers; current; current = current->next) {
		if (current == timer)
			return 1;
	}
	return 0;
}

static kb2_core_status_t time_timer_validate(
	struct core_binding *binding, struct kb2_core_time_timer *timer)
{
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(
		binding, KB2_CORE_RUNTIME_INTERFACE_TIME, 0);
	core_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!timer || !kobox_provider_arena_contains(
			      core_arena, timer, sizeof(*timer)))
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	time_lock();
	if (timer->magic != CORE_TIMER_MAGIC || timer->binding != binding ||
	    timer->generation != core_generation ||
	    !time_timer_list_contains_locked(timer))
		status = KB2_CORE_RUNTIME_STATUS_OWNER;
	else if (timer->closing)
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	time_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	return kobox_provider_arena_validate_owner(
		       core_arena, timer, 0, binding, CORE_ALLOCATION_TIMER) ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_OWNER;
}

static void time_queued_add(struct kb2_core_time_timer *timer,
			    uint64_t count)
{
	uint64_t previous = timer->queued_low;

	timer->queued_low += count;
	if (timer->queued_low < previous) {
		if (timer->queued_high == UINT64_MAX)
			__builtin_trap();
		timer->queued_high++;
	}
}

static uint64_t time_queued_take(struct kb2_core_time_timer *timer)
{
	uint64_t result;

	if (timer->queued_high) {
		result = UINT64_MAX;
		if (timer->queued_low < UINT64_MAX) {
			timer->queued_high--;
			timer->queued_low++;
		}
	} else {
		result = timer->queued_low;
		timer->queued_low = 0;
	}
	return result;
}

static uint64_t time_expiration_count(
	struct kb2_core_time_timer *timer, uint64_t now,
	uint64_t *extra_out)
{
	uint64_t count = 1;

	*extra_out = 0;
	if (timer->period_ns) {
		uint64_t elapsed =
			(now - timer->deadline_ns) / timer->period_ns;

		if (elapsed == UINT64_MAX) {
			count = UINT64_MAX;
			*extra_out = 1;
		} else {
			count += elapsed;
		}
		if (*extra_out ||
		    count > (UINT64_MAX - timer->deadline_ns) /
				    timer->period_ns) {
			timer->pending = 0;
		} else {
			timer->deadline_ns += count * timer->period_ns;
		}
	} else {
		timer->pending = 0;
	}
	return count;
}

static int time_clock_native(uint32_t clock_id)
{
	switch (clock_id) {
	case KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC:
		return CLOCK_MONOTONIC;
	case KB2_CORE_RUNTIME_TIME_CLOCK_BOOTTIME:
		return CLOCK_BOOTTIME;
	case KB2_CORE_RUNTIME_TIME_CLOCK_REALTIME:
		return CLOCK_REALTIME;
	default:
		return -1;
	}
}

static kb2_core_status_t time_now_native(int native_clock,
					 uint64_t *time_ns_out)
{
	struct __kernel_timespec value;
	long result;

	result = core_linux_syscall6(__NR_clock_gettime, native_clock,
				    (long)(uintptr_t)&value, 0, 0, 0, 0);
	if (result < 0 || value.tv_sec < 0 || value.tv_nsec < 0 ||
	    value.tv_nsec >= 1000000000 ||
	    (uint64_t)value.tv_sec >
		    (UINT64_MAX - (uint64_t)value.tv_nsec) /
			    UINT64_C(1000000000))
		return result < 0 ? KB2_CORE_RUNTIME_STATUS_CORRUPT :
				    KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	*time_ns_out = (uint64_t)value.tv_sec * UINT64_C(1000000000) +
		       (uint64_t)value.tv_nsec;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static void time_timer_program_cpu(struct core_time_cpu *cpu)
{
	uint32_t clock_id;

	for (clock_id = KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC;
	     clock_id <= KB2_CORE_RUNTIME_TIME_CLOCK_REALTIME; clock_id++) {
		struct __kernel_itimerspec setting;
		struct kb2_core_time_timer *timer;
		uint64_t deadline = 0;
		long result;

		time_lock();
		for (timer = core_timers; timer; timer = timer->next) {
			if (!timer->closing && timer->pending &&
			    timer->assigned_cpu == cpu->logical_cpu &&
			    timer->clock_id == clock_id &&
			    !(timer->flags &
			      KB2_CORE_RUNTIME_TIME_TIMER_FLAG_DEFERRABLE) &&
			    (!deadline || timer->deadline_ns < deadline))
				deadline = timer->deadline_ns;
		}
		time_unlock();
		bytes_zero(&setting, sizeof(setting));
		if (deadline) {
			setting.it_value.tv_sec =
				deadline / UINT64_C(1000000000);
			setting.it_value.tv_nsec =
				deadline % UINT64_C(1000000000);
		}
		result = core_linux_syscall6(
			__NR_timerfd_settime,
			cpu->timer_descriptors[clock_id - 1],
			deadline ? TFD_TIMER_ABSTIME |
					   (clock_id ==
						    KB2_CORE_RUNTIME_TIME_CLOCK_REALTIME ?
						    TFD_TIMER_CANCEL_ON_SET : 0) :
				   0,
			(long)(uintptr_t)&setting, 0, 0, 0);
		if (result < 0)
			__builtin_trap();
	}
}

static int time_timer_due_locked(struct kb2_core_time_timer *timer,
					 uint32_t cpu_id,
					 const uint64_t now[3])
{
	return !timer->closing && timer->pending &&
	       timer->assigned_cpu == cpu_id &&
	       timer->deadline_ns <= now[timer->clock_id - 1];
}

static void time_timer_callback_complete(
	struct kb2_core_time_timer *timer, uint32_t cpu_id)
{
	time_lock();
	if (!timer->running)
		__builtin_trap();
	timer->running = 0;
	(void)__atomic_add_fetch(&timer->drain_word, 1, __ATOMIC_RELEASE);
	sync_futex_wake(&timer->drain_word);
	time_unlock();
	time_cpu_signal(cpu_id);
}

static void time_timer_process_due(struct core_time_cpu *cpu)
{
	uint64_t now[3];
	uint32_t clock_id;

	for (clock_id = KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC;
	     clock_id <= KB2_CORE_RUNTIME_TIME_CLOCK_REALTIME; clock_id++) {
		if (time_now_native(time_clock_native(clock_id),
				    &now[clock_id - 1]) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
			__builtin_trap();
	}
	for (;;) {
		struct kb2_core_time_timer *timer;
		kb2_core_timer_callback_fn callback = NULL;
		void *argument = NULL;
		uint64_t count = 0;
		uint64_t extra = 0;

		time_lock();
		for (timer = core_timers; timer; timer = timer->next) {
			if (!timer->closing && !timer->running &&
			    timer->assigned_cpu == cpu->logical_cpu &&
			    timer->callback_context ==
				    KB2_CORE_RUNTIME_TIME_TIMER_CONTEXT_ATOMIC &&
			    (timer->queued_low || timer->queued_high)) {
				timer->running = 1;
				count = time_queued_take(timer);
				callback = timer->callback;
				argument = timer->argument;
				break;
			}
			if (!time_timer_due_locked(timer, cpu->logical_cpu, now))
				continue;
			count = time_expiration_count(
				timer, now[timer->clock_id - 1], &extra);
			if (timer->callback_context ==
			    KB2_CORE_RUNTIME_TIME_TIMER_CONTEXT_THREAD) {
				time_queued_add(timer, count);
				if (extra)
					time_queued_add(timer, extra);
				time_descriptor_signal(cpu->worker_descriptor);
				timer = NULL;
				break;
			}
			if (timer->running) {
				time_queued_add(timer, count);
				timer = NULL;
				break;
			}
			timer->running = 1;
			if (extra)
				time_queued_add(timer, extra);
			callback = timer->callback;
			argument = timer->argument;
			break;
		}
		time_unlock();
		if (!timer) {
			if (!callback)
				break;
			continue;
		}
		core_cpu_id = cpu->logical_cpu;
		core_cpu_context_class =
			KB2_CORE_RUNTIME_CPU_CONTEXT_SOFTIRQ;
		core_current_timer = timer;
		callback(timer, argument, count);
		core_current_timer = NULL;
		if (core_rcu_read_depth)
			__builtin_trap();
		core_cpu_context_class = 0;
		core_cpu_id = 0;
		time_timer_callback_complete(timer, cpu->logical_cpu);
		for (clock_id = KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC;
		     clock_id <= KB2_CORE_RUNTIME_TIME_CLOCK_REALTIME;
		     clock_id++) {
			if (time_now_native(time_clock_native(clock_id),
					    &now[clock_id - 1]) !=
			    KB2_CORE_RUNTIME_STATUS_OK)
				__builtin_trap();
		}
	}
}

static void *time_dispatcher_entry(void *argument)
{
	struct core_time_cpu *cpu = argument;
	struct core_poll_descriptor descriptors[4];
	size_t index;

	core_cpu_id = cpu->logical_cpu;
	core_cpu_context_class = KB2_CORE_RUNTIME_CPU_CONTEXT_SOFTIRQ;
	for (;;) {
		descriptors[0] = (struct core_poll_descriptor){
			.descriptor = cpu->dispatcher_descriptor,
			.events = POLLIN,
		};
		for (index = 0; index < 3; index++) {
			descriptors[index + 1] = (struct core_poll_descriptor){
				.descriptor = cpu->timer_descriptors[index],
				.events = POLLIN,
			};
		}
		if (core_linux_syscall6(__NR_poll,
					(long)(uintptr_t)descriptors, 4, -1,
					0, 0, 0) < 0)
			continue;
		time_descriptor_drain(cpu->dispatcher_descriptor);
		for (index = 0; index < 3; index++)
			time_descriptor_drain(cpu->timer_descriptors[index]);
		time_timer_process_due(cpu);
		if (__atomic_load_n(&core_time_stop, __ATOMIC_ACQUIRE))
			break;
		time_timer_program_cpu(cpu);
	}
	core_cpu_context_class = 0;
	core_cpu_id = 0;
	return NULL;
}

static void *time_worker_entry(void *argument)
{
	struct core_time_cpu *cpu = argument;
	struct core_poll_descriptor descriptor;
	struct kb2_core_thread *thread = &cpu->worker_thread;

	core_current_thread = thread;
	core_sync_thread_id = thread->identity;
	core_cpu_id = cpu->logical_cpu;
	core_cpu_context_class = KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD;
	thread->native = pthread_self();
	thread->native_tid = (uint32_t)core_linux_syscall6(
		__NR_gettid, 0, 0, 0, 0, 0, 0);
	for (;;) {
		descriptor = (struct core_poll_descriptor){
			.descriptor = cpu->worker_descriptor,
			.events = POLLIN,
		};
		if (core_linux_syscall6(__NR_poll, (long)(uintptr_t)&descriptor,
					1, -1, 0, 0, 0) < 0)
			continue;
		time_descriptor_drain(cpu->worker_descriptor);
		for (;;) {
			struct kb2_core_time_timer *timer;
			kb2_core_timer_callback_fn callback;
			void *callback_argument;
			uint64_t count;

			time_lock();
			for (timer = core_timers; timer; timer = timer->next) {
				if (!timer->closing && !timer->running &&
				    timer->assigned_cpu == cpu->logical_cpu &&
				    timer->callback_context ==
					    KB2_CORE_RUNTIME_TIME_TIMER_CONTEXT_THREAD &&
				    (timer->queued_low || timer->queued_high))
					break;
			}
			if (!timer) {
				time_unlock();
				break;
			}
			timer->running = 1;
			count = time_queued_take(timer);
			callback = timer->callback;
			callback_argument = timer->argument;
			thread->binding = timer->thread_binding;
			time_unlock();
			if (!thread->binding)
				__builtin_trap();
			core_current_timer = timer;
			callback(timer, callback_argument, count);
			core_current_timer = NULL;
			if (core_rcu_read_depth)
				__builtin_trap();
			thread->binding = NULL;
			time_timer_callback_complete(timer, cpu->logical_cpu);
		}
		if (__atomic_load_n(&core_time_stop, __ATOMIC_ACQUIRE))
			break;
	}
	core_current_thread = NULL;
	core_sync_thread_id = 0;
	core_cpu_id = 0;
	core_cpu_context_class = 0;
	return NULL;
}

static kb2_core_status_t time_binding_validate(void *binding_object,
						int allocation)
{
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(binding_object,
					 KB2_CORE_RUNTIME_INTERFACE_TIME,
					 allocation);
	if (status == KB2_CORE_RUNTIME_STATUS_OK && !core_time_active)
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	core_unlock();
	return status;
}

static kb2_core_status_t time_read_clock(void *binding_object,
					 uint32_t clock_id,
					 uint64_t *time_ns_out)
{
	int native_clock;
	kb2_core_status_t status;

	if (!time_ns_out || (native_clock = time_clock_native(clock_id)) < 0)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = time_binding_validate(binding_object, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	return time_now_native(native_clock, time_ns_out);
}

static kb2_core_status_t time_monotonic_ns(void *binding_object,
					   uint64_t *time_ns_out)
{
	return time_read_clock(binding_object,
			       KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC,
			       time_ns_out);
}

static kb2_core_status_t time_boottime_ns(void *binding_object,
					  uint64_t *time_ns_out)
{
	return time_read_clock(binding_object,
			       KB2_CORE_RUNTIME_TIME_CLOCK_BOOTTIME,
			       time_ns_out);
}

static kb2_core_status_t time_realtime_ns(void *binding_object,
					  uint64_t *time_ns_out)
{
	return time_read_clock(binding_object,
			       KB2_CORE_RUNTIME_TIME_CLOCK_REALTIME,
			       time_ns_out);
}

static void time_descriptor_drain(int descriptor)
{
	uint64_t value;
	long result;

	do {
		result = core_linux_syscall6(
			__NR_read, descriptor, (long)(uintptr_t)&value,
			sizeof(value), 0, 0, 0);
	} while (result == (long)sizeof(value) || result == -EINTR);
	if (result < 0 && result != -EAGAIN && result != -ECANCELED)
		__builtin_trap();
}

static kb2_core_status_t time_sleep_until(void *binding_object,
					  uint32_t clock_id,
					  uint64_t deadline_ns,
					  uint32_t wait_flags)
{
	struct __kernel_itimerspec setting;
	struct core_poll_descriptor descriptors[2];
	struct kb2_core_thread *thread = core_current_thread;
	kb2_core_status_t status;
	uint64_t now;
	int native_clock = time_clock_native(clock_id);
	int timer_descriptor = -1;
	long result;

	if (native_clock < 0 ||
	    wait_flags & ~KB2_CORE_RUNTIME_TIME_WAIT_FLAG_INTERRUPTIBLE)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = time_binding_validate(binding_object, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!thread || thread->wake_descriptor < 0 ||
	    core_cpu_context_class != KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD)
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	status = time_now_native(native_clock, &now);
	if (status != KB2_CORE_RUNTIME_STATUS_OK || deadline_ns <= now)
		return status;
	timer_descriptor = (int)core_linux_syscall6(
		__NR_timerfd_create, native_clock, TFD_CLOEXEC | TFD_NONBLOCK,
		0, 0, 0, 0);
	if (timer_descriptor < 0)
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	time_descriptor_drain(thread->wake_descriptor);
	thread_lock(thread);
	if (thread->time_waiting) {
		thread_unlock(thread);
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		goto out;
	}
	thread->time_waiting = 1;
	thread->time_wait_interruptible = !!(
		wait_flags & KB2_CORE_RUNTIME_TIME_WAIT_FLAG_INTERRUPTIBLE);
	if (thread->time_wait_interruptible && thread->stop_requested)
		status = KB2_CORE_RUNTIME_STATUS_CANCELED;
	else if (thread->time_wait_interruptible && thread->interrupted)
		status = KB2_CORE_RUNTIME_STATUS_INTERRUPTED;
	else
		status = KB2_CORE_RUNTIME_STATUS_OK;
	thread_unlock(thread);
	if (status != KB2_CORE_RUNTIME_STATUS_OK) {
		kb2_core_status_t clock_status =
			time_now_native(native_clock, &now);

		if (clock_status != KB2_CORE_RUNTIME_STATUS_OK)
			status = clock_status;
		else if (now >= deadline_ns)
			status = KB2_CORE_RUNTIME_STATUS_OK;
		goto unregister;
	}
	core_lock();
	if (!core_time_active || core_time_sleep_count == SIZE_MAX) {
		status = core_time_active ? KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
					    KB2_CORE_RUNTIME_STATUS_CANCELED;
		core_unlock();
		goto unregister;
	}
	core_time_sleep_count++;
	core_unlock();
	for (;;) {
		bytes_zero(&setting, sizeof(setting));
		setting.it_value.tv_sec = deadline_ns / UINT64_C(1000000000);
		setting.it_value.tv_nsec = deadline_ns % UINT64_C(1000000000);
		result = core_linux_syscall6(
			__NR_timerfd_settime, timer_descriptor,
			TFD_TIMER_ABSTIME |
				(native_clock == CLOCK_REALTIME ?
					 TFD_TIMER_CANCEL_ON_SET : 0),
			(long)(uintptr_t)&setting, 0, 0, 0);
		if (result < 0) {
			status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
			break;
		}
		descriptors[0] = (struct core_poll_descriptor){
			.descriptor = timer_descriptor,
			.events = POLLIN,
		};
		descriptors[1] = (struct core_poll_descriptor){
			.descriptor = thread->wake_descriptor,
			.events = POLLIN,
		};
		result = core_linux_syscall6(
			__NR_poll, (long)(uintptr_t)descriptors, 2, -1, 0, 0, 0);
		if (result < 0 && result != -EINTR) {
			status = KB2_CORE_RUNTIME_STATUS_CORRUPT;
			break;
		}
		status = time_now_native(native_clock, &now);
		if (status != KB2_CORE_RUNTIME_STATUS_OK || now >= deadline_ns)
			break;
		if (!__atomic_load_n(&core_time_active, __ATOMIC_ACQUIRE)) {
			status = KB2_CORE_RUNTIME_STATUS_CANCELED;
			break;
		}
		thread_lock(thread);
		if (thread->time_wait_interruptible &&
		    thread->stop_requested)
			status = KB2_CORE_RUNTIME_STATUS_CANCELED;
		else if (thread->time_wait_interruptible && thread->interrupted)
			status = KB2_CORE_RUNTIME_STATUS_INTERRUPTED;
		else
			status = KB2_CORE_RUNTIME_STATUS_OK;
		thread_unlock(thread);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			break;
		time_descriptor_drain(thread->wake_descriptor);
	}
	core_lock();
	if (!core_time_sleep_count)
		__builtin_trap();
	core_time_sleep_count--;
	(void)__atomic_add_fetch(&core_time_sleep_word, 1, __ATOMIC_RELEASE);
	sync_futex_wake(&core_time_sleep_word);
	core_unlock();
unregister:
	thread_lock(thread);
	thread->time_waiting = 0;
	thread->time_wait_interruptible = 0;
	thread_unlock(thread);
out:
	if (core_linux_syscall6(__NR_close, timer_descriptor, 0, 0, 0, 0, 0) < 0)
		__builtin_trap();
	return status;
}

static kb2_core_status_t time_busy_wait_until(void *binding_object,
					      uint32_t clock_id,
					      uint64_t deadline_ns)
{
	kb2_core_status_t status;
	uint64_t now;
	int native_clock = time_clock_native(clock_id);

	if (native_clock < 0)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = time_binding_validate(binding_object, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	do {
		status = time_now_native(native_clock, &now);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			return status;
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#endif
	} while (now < deadline_ns);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t time_current_logical_cpu(uint32_t *cpu_id_out)
{
	uint32_t native_cpu;
	uint32_t index;

	if (core_linux_syscall6(__NR_getcpu, (long)(uintptr_t)&native_cpu, 0,
				0, 0, 0, 0) < 0)
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	for (index = 0; index < core_native_cpu_count; index++) {
		if (core_native_cpu_ids[index] == native_cpu) {
			*cpu_id_out = index;
			return KB2_CORE_RUNTIME_STATUS_OK;
		}
	}
	return KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

static kb2_core_status_t time_timer_create(
	void *binding_object, uint32_t clock_id, uint32_t timer_context,
	kb2_core_timer_callback_fn timer_callback, void *argument,
	uint32_t flags, kb2_core_time_timer_t *timer_out)
{
	struct core_binding *binding = binding_object;
	struct kb2_core_time_timer *timer;
	struct core_binding *thread_binding;
	kb2_core_status_t status;
	uint32_t cpu_id = 0;

	if (!timer_out || !timer_callback || time_clock_native(clock_id) < 0 ||
	    (timer_context != KB2_CORE_RUNTIME_TIME_TIMER_CONTEXT_ATOMIC &&
	     timer_context != KB2_CORE_RUNTIME_TIME_TIMER_CONTEXT_THREAD) ||
	    flags & ~(KB2_CORE_RUNTIME_TIME_TIMER_FLAG_DEFERRABLE |
		      KB2_CORE_RUNTIME_TIME_TIMER_FLAG_PINNED))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*timer_out = NULL;
	status = time_binding_validate(binding, 1);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread_binding = time_thread_binding(binding->node_id);
	if (!thread_binding)
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	if (flags & KB2_CORE_RUNTIME_TIME_TIMER_FLAG_PINNED) {
		status = time_current_logical_cpu(&cpu_id);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			return status;
	}
	status = binding_object_begin(binding,
				      KB2_CORE_RUNTIME_INTERFACE_TIME);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	timer = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_TIMER);
	if (!timer) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(timer, CORE_PAGE_SIZE);
	timer->magic = CORE_TIMER_MAGIC;
	timer->binding = binding;
	timer->thread_binding = thread_binding;
	timer->callback = timer_callback;
	timer->argument = argument;
	timer->generation = core_generation;
	timer->clock_id = clock_id;
	timer->callback_context = timer_context;
	timer->flags = flags;
	timer->assigned_cpu = cpu_id;
	time_lock();
	if (!core_time_active || core_timer_count == SIZE_MAX) {
		time_unlock();
		timer->magic = 0;
		(void)kobox_provider_arena_release_owned(
			core_arena, timer, 0, binding, CORE_ALLOCATION_TIMER);
		binding_object_end(binding);
		return core_timer_count == SIZE_MAX ?
			       KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
			       KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	}
	timer->next = core_timers;
	core_timers = timer;
	core_timer_count++;
	time_unlock();
	*timer_out = timer;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t time_timer_cancel_common(
	struct core_binding *binding, struct kb2_core_time_timer *timer,
	uint32_t wait, uint32_t *canceled_out)
{
	kb2_core_status_t status;
	uint32_t cpu_id;
	uint32_t canceled;

	if (!canceled_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*canceled_out = 0;
	status = time_timer_validate(binding, timer);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (wait && core_current_timer == timer)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	time_lock();
	canceled = timer->pending || timer->queued_low || timer->queued_high;
	timer->pending = 0;
	timer->queued_low = 0;
	timer->queued_high = 0;
	timer->arm_generation++;
	cpu_id = timer->assigned_cpu;
	time_unlock();
	time_cpu_signal(cpu_id);
	if (wait) {
		for (;;) {
			uint32_t expected;

			time_lock();
			if (!timer->running) {
				time_unlock();
				break;
			}
			expected = __atomic_load_n(&timer->drain_word,
						   __ATOMIC_ACQUIRE);
			time_unlock();
			(void)sync_futex_wait(&timer->drain_word, expected, 0);
		}
	}
	*canceled_out = !!canceled;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t time_timer_cancel(void *binding_object,
					   kb2_core_time_timer_t timer,
					   uint32_t *canceled_out)
{
	return time_timer_cancel_common(binding_object, timer, 0,
					canceled_out);
}

static kb2_core_status_t time_timer_cancel_sync(
	void *binding_object, kb2_core_time_timer_t timer,
	uint32_t *canceled_out)
{
	return time_timer_cancel_common(binding_object, timer, 1,
					canceled_out);
}

static kb2_core_status_t time_timer_arm(void *binding_object,
					kb2_core_time_timer_t timer,
					uint64_t deadline_ns,
					uint64_t period_ns)
{
	kb2_core_status_t status = time_timer_validate(binding_object, timer);
	uint32_t old_cpu;
	uint32_t new_cpu;

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!__atomic_load_n(&core_time_active, __ATOMIC_ACQUIRE))
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	time_lock();
	if (!core_time_active || timer->closing) {
		time_unlock();
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	}
	old_cpu = timer->assigned_cpu;
	new_cpu = timer->flags & KB2_CORE_RUNTIME_TIME_TIMER_FLAG_PINNED ?
			  old_cpu :
			  core_time_next_cpu++ % core_cpu_count;
	timer->assigned_cpu = new_cpu;
	timer->arm_generation++;
	timer->deadline_ns = deadline_ns;
	timer->period_ns = period_ns;
	timer->queued_low = 0;
	timer->queued_high = 0;
	timer->pending = 1;
	time_unlock();
	time_cpu_signal(old_cpu);
	if (new_cpu != old_cpu)
		time_cpu_signal(new_cpu);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t time_timer_remaining(
	void *binding_object, kb2_core_time_timer_t timer,
	uint64_t *remaining_ns_out)
{
	kb2_core_status_t status;
	uint64_t deadline;
	uint64_t now;
	uint32_t pending;
	uint32_t clock_id;

	if (!remaining_ns_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = time_timer_validate(binding_object, timer);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	time_lock();
	pending = timer->pending;
	deadline = timer->deadline_ns;
	clock_id = timer->clock_id;
	time_unlock();
	if (!pending) {
		*remaining_ns_out = 0;
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	status = time_now_native(time_clock_native(clock_id), &now);
	if (status == KB2_CORE_RUNTIME_STATUS_OK)
		*remaining_ns_out = deadline > now ? deadline - now : 0;
	return status;
}

static kb2_core_status_t time_timer_is_pending(
	void *binding_object, kb2_core_time_timer_t timer, uint32_t *pending_out)
{
	kb2_core_status_t status;

	if (!pending_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = time_timer_validate(binding_object, timer);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	time_lock();
	*pending_out = !!(timer->pending || timer->queued_low ||
			   timer->queued_high);
	time_unlock();
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t time_timer_destroy(void *binding_object,
					    kb2_core_time_timer_t timer)
{
	struct core_binding *binding = binding_object;
	struct kb2_core_time_timer **link;
	kb2_core_status_t status = time_timer_validate(binding, timer);
	uint32_t canceled;
	uint32_t cpu_id;

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (core_current_timer == timer)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	status = time_timer_cancel_common(binding, timer, 1, &canceled);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	time_lock();
	for (link = &core_timers; *link && *link != timer;
	     link = &(*link)->next)
		;
	if (!*link || !core_timer_count) {
		time_unlock();
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	timer->closing = 1;
	cpu_id = timer->assigned_cpu;
	*link = timer->next;
	timer->next = NULL;
	core_timer_count--;
	timer->magic = 0;
	time_unlock();
	time_cpu_signal(cpu_id);
	binding_object_end(binding);
	return kobox_provider_arena_release_owned(
		       core_arena, timer, 0, binding, CORE_ALLOCATION_TIMER) ==
		       KOBOX_PROVIDER_ARENA_OK ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

static int time_lifecycle_init(const struct kobox_module_context *context)
{
	uint32_t cpu_id;
	int failed = 0;

	if (!context || core_time_active || core_time_sleep_count ||
	    core_timers || core_timer_count || core_time_stop)
		return -1;
	bytes_zero(core_time_cpus, sizeof(core_time_cpus));
	for (cpu_id = 0; cpu_id < core_cpu_count; cpu_id++) {
		uint32_t index;

		core_time_cpus[cpu_id].dispatcher_descriptor = -1;
		core_time_cpus[cpu_id].worker_descriptor = -1;
		core_time_cpus[cpu_id].worker_thread.wake_descriptor = -1;
		for (index = 0; index < 3; index++)
			core_time_cpus[cpu_id].timer_descriptors[index] = -1;
	}
	for (cpu_id = 0; cpu_id < core_cpu_count; cpu_id++) {
		struct core_time_cpu *cpu = &core_time_cpus[cpu_id];
		uint64_t native_mask[CORE_NATIVE_CPU_WORDS];
		uint32_t clock_id;

		cpu->logical_cpu = cpu_id;
		cpu->dispatcher_descriptor = -1;
		cpu->worker_descriptor = -1;
		cpu->worker_thread.wake_descriptor = -1;
		for (clock_id = 0; clock_id < 3; clock_id++)
			cpu->timer_descriptors[clock_id] = -1;
		cpu->dispatcher_descriptor = thread_wake_descriptor_create();
		cpu->worker_descriptor = thread_wake_descriptor_create();
		if (cpu->dispatcher_descriptor < 0 ||
		    cpu->worker_descriptor < 0) {
			failed = 1;
			break;
		}
		for (clock_id = KB2_CORE_RUNTIME_TIME_CLOCK_MONOTONIC;
		     clock_id <= KB2_CORE_RUNTIME_TIME_CLOCK_REALTIME;
		     clock_id++) {
			cpu->timer_descriptors[clock_id - 1] =
				(int)core_linux_syscall6(
					__NR_timerfd_create,
					time_clock_native(clock_id),
					TFD_CLOEXEC | TFD_NONBLOCK, 0, 0, 0, 0);
			if (cpu->timer_descriptors[clock_id - 1] < 0) {
				failed = 1;
				break;
			}
		}
		if (failed)
			break;
		bytes_zero(&cpu->worker_thread,
			   sizeof(cpu->worker_thread));
		cpu->worker_thread.wake_descriptor =
			thread_wake_descriptor_create();
		if (cpu->worker_thread.wake_descriptor < 0) {
			failed = 1;
			break;
		}
		cpu->worker_thread.magic = CORE_THREAD_MAGIC;
		cpu->worker_thread.identity = thread_allocate_identity();
		cpu->worker_thread.generation = context->generation;
		cpu->worker_thread.state = CORE_THREAD_RUNNING;
		cpu->worker_thread.borrowed = 1;
		cpu->worker_thread.logical_cpu = cpu_id;
		cpu->worker_thread.priority =
			KB2_CORE_RUNTIME_THREAD_PRIORITY_DEFAULT;
		bytes_zero(native_mask, sizeof(native_mask));
		native_mask[core_native_cpu_ids[cpu_id] / 64] |=
			UINT64_C(1) << (core_native_cpu_ids[cpu_id] % 64);
		if (pthread_create(&cpu->dispatcher, NULL,
				   time_dispatcher_entry, cpu)) {
			failed = 1;
			break;
		}
		cpu->dispatcher_started = 1;
		if (pthread_setaffinity_np(cpu->dispatcher, sizeof(native_mask),
					   (const cpu_set_t *)native_mask) ||
		    pthread_create(&cpu->worker, NULL, time_worker_entry, cpu)) {
			failed = 1;
			break;
		}
		cpu->worker_started = 1;
		if (pthread_setaffinity_np(cpu->worker, sizeof(native_mask),
					   (const cpu_set_t *)native_mask)) {
			failed = 1;
			break;
		}
	}
	if (!failed) {
		core_time_active = 1;
		return 0;
	}
	__atomic_store_n(&core_time_stop, 1, __ATOMIC_RELEASE);
	for (cpu_id = 0; cpu_id < core_cpu_count; cpu_id++) {
		struct core_time_cpu *cpu = &core_time_cpus[cpu_id];

		if (cpu->dispatcher_descriptor >= 0)
			time_descriptor_signal(cpu->dispatcher_descriptor);
		if (cpu->worker_descriptor >= 0)
			time_descriptor_signal(cpu->worker_descriptor);
	}
	for (cpu_id = 0; cpu_id < core_cpu_count; cpu_id++) {
		struct core_time_cpu *cpu = &core_time_cpus[cpu_id];
		uint32_t index;

		if (cpu->dispatcher_started)
			(void)pthread_join(cpu->dispatcher, NULL);
		if (cpu->worker_started)
			(void)pthread_join(cpu->worker, NULL);
		for (index = 0; index < 3; index++) {
			if (cpu->timer_descriptors[index] >= 0)
				(void)core_linux_syscall6(
					__NR_close, cpu->timer_descriptors[index], 0,
					0, 0, 0, 0);
		}
		if (cpu->dispatcher_descriptor >= 0)
			(void)core_linux_syscall6(
				__NR_close, cpu->dispatcher_descriptor, 0, 0, 0,
				0, 0);
		if (cpu->worker_descriptor >= 0)
			(void)core_linux_syscall6(
				__NR_close, cpu->worker_descriptor, 0, 0, 0, 0,
				0);
		thread_wake_descriptor_close(&cpu->worker_thread);
	}
	bytes_zero(core_time_cpus, sizeof(core_time_cpus));
	core_time_stop = 0;
	return -1;
}

static int time_lifecycle_quiesce(const struct kobox_module_context *context)
{
	struct kb2_core_thread *thread;

	(void)context;
	core_lock();
	if (!core_time_active) {
		core_unlock();
		return -1;
	}
	__atomic_store_n(&core_time_active, 0, __ATOMIC_RELEASE);
	time_lock();
	for (struct kb2_core_time_timer *timer = core_timers; timer;
	     timer = timer->next) {
		timer->pending = 0;
		timer->queued_low = 0;
		timer->queued_high = 0;
	}
	time_unlock();
	__atomic_store_n(&core_time_stop, 1, __ATOMIC_RELEASE);
	thread_private_notify(&core_root_thread);
	for (thread = core_threads; thread; thread = thread->next)
		thread_private_notify(thread);
	for (uint32_t cpu_id = 0; cpu_id < core_cpu_count; cpu_id++) {
		struct core_time_cpu *cpu = &core_time_cpus[cpu_id];

		thread_private_notify(&cpu->worker_thread);
		time_descriptor_signal(cpu->dispatcher_descriptor);
		time_descriptor_signal(cpu->worker_descriptor);
	}
	core_unlock();
	for (;;) {
		uint32_t expected;

		core_lock();
		if (!core_time_sleep_count) {
			core_unlock();
			break;
		}
		expected = __atomic_load_n(&core_time_sleep_word,
					   __ATOMIC_ACQUIRE);
		core_unlock();
		(void)sync_futex_wait(&core_time_sleep_word, expected, 0);
	}
	for (uint32_t cpu_id = 0; cpu_id < core_cpu_count; cpu_id++) {
		struct core_time_cpu *cpu = &core_time_cpus[cpu_id];

		if (cpu->dispatcher_started &&
		    pthread_join(cpu->dispatcher, NULL))
			return -1;
		cpu->dispatcher_started = 0;
		if (cpu->worker_started && pthread_join(cpu->worker, NULL))
			return -1;
		cpu->worker_started = 0;
	}
	return 0;
}

static int time_lifecycle_cleanup(const struct kobox_module_context *context)
{
	uint32_t cpu_id;

	(void)context;
	if (core_time_active || core_time_sleep_count || core_timers ||
	    core_timer_count)
		return -1;
	for (cpu_id = 0; cpu_id < core_cpu_count; cpu_id++) {
		struct core_time_cpu *cpu = &core_time_cpus[cpu_id];
		uint32_t index;

		if (cpu->dispatcher_started || cpu->worker_started)
			return -1;
		for (index = 0; index < 3; index++) {
			if (cpu->timer_descriptors[index] >= 0 &&
			    core_linux_syscall6(
				    __NR_close, cpu->timer_descriptors[index], 0,
				    0, 0, 0, 0) < 0)
				return -1;
			cpu->timer_descriptors[index] = -1;
		}
		if (cpu->dispatcher_descriptor >= 0 &&
		    core_linux_syscall6(__NR_close,
					cpu->dispatcher_descriptor, 0, 0, 0, 0,
					0) < 0)
			return -1;
		cpu->dispatcher_descriptor = -1;
		if (cpu->worker_descriptor >= 0 &&
		    core_linux_syscall6(__NR_close, cpu->worker_descriptor, 0, 0,
					0, 0, 0) < 0)
			return -1;
		cpu->worker_descriptor = -1;
		thread_wake_descriptor_close(&cpu->worker_thread);
		bytes_zero(cpu, sizeof(*cpu));
	}
	core_time_stop = 0;
	core_time_next_cpu = 0;
	core_time_sleep_word = 0;
	return 0;
}

static void work_lock(void)
{
	while (__atomic_exchange_n(&core_work_lock_word, 1,
				   __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#endif
	}
}

static void work_unlock(void)
{
	__atomic_store_n(&core_work_lock_word, 0, __ATOMIC_RELEASE);
}

static void work_queue_notify(struct kb2_core_workqueue_queue *queue)
{
	uint32_t domain;

	if (queue->dispatcher_descriptor >= 0)
		time_descriptor_signal(queue->dispatcher_descriptor);
	if (queue->domain_descriptors) {
		for (domain = 0; domain < queue->domain_count; domain++) {
			if (queue->domain_descriptors[domain] >= 0)
				time_descriptor_signal(
					queue->domain_descriptors[domain]);
		}
	}
}

static void work_state_changed(struct kb2_core_workqueue_queue *queue,
			       struct kb2_core_workqueue_work *work)
{
	if (work) {
		(void)__atomic_add_fetch(&work->drain_word, 1,
					 __ATOMIC_RELEASE);
		sync_futex_wake(&work->drain_word);
	}
	if (queue) {
		(void)__atomic_add_fetch(&queue->drain_word, 1,
					 __ATOMIC_RELEASE);
		sync_futex_wake(&queue->drain_word);
		work_queue_notify(queue);
	}
}

static int work_queue_list_contains_locked(
	const struct kb2_core_workqueue_queue *queue)
{
	const struct kb2_core_workqueue_queue *current;

	for (current = core_work_queues; current; current = current->next) {
		if (current == queue)
			return 1;
	}
	return 0;
}

static int work_list_contains_locked(
	const struct kb2_core_workqueue_work *work)
{
	const struct kb2_core_workqueue_work *current;

	for (current = core_works; current; current = current->next) {
		if (current == work)
			return 1;
	}
	return 0;
}

static kb2_core_status_t work_binding_validate(void *binding_object,
						int allocation)
{
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(binding_object,
					 KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE,
					 allocation);
	if (status == KB2_CORE_RUNTIME_STATUS_OK && allocation &&
	    !core_work_active)
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	core_unlock();
	return status;
}

static kb2_core_status_t work_queue_validate(
	struct core_binding *binding, struct kb2_core_workqueue_queue *queue)
{
	kb2_core_status_t status = work_binding_validate(binding, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!queue || !kobox_provider_arena_contains(
			      core_arena, queue, sizeof(*queue)))
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	work_lock();
	if (queue->magic != CORE_WORKQUEUE_MAGIC ||
	    queue->binding != binding || queue->generation != core_generation ||
	    !work_queue_list_contains_locked(queue))
		status = KB2_CORE_RUNTIME_STATUS_OWNER;
	else if (queue->closing)
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	work_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	return kobox_provider_arena_validate_owner(
		       core_arena, queue, 0, binding,
		       CORE_ALLOCATION_WORKQUEUE) ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_OWNER;
}

static kb2_core_status_t work_validate(
	struct core_binding *binding, struct kb2_core_workqueue_work *work)
{
	kb2_core_status_t status = work_binding_validate(binding, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!work || !kobox_provider_arena_contains(
			     core_arena, work, sizeof(*work)))
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	work_lock();
	if (work->magic != CORE_WORK_MAGIC || work->binding != binding ||
	    work->generation != core_generation ||
	    !work_list_contains_locked(work))
		status = KB2_CORE_RUNTIME_STATUS_OWNER;
	else if (work->closing)
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	work_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	return kobox_provider_arena_validate_owner(
		       core_arena, work, 0, binding, CORE_ALLOCATION_WORK) ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_OWNER;
}

static kb2_core_status_t work_submission_cpu(
	struct kb2_core_workqueue_queue *queue, uint32_t requested_cpu,
	uint32_t *cpu_out)
{
	if (queue->flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND) {
		if (requested_cpu != KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY)
			return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
		*cpu_out = 0;
		return KB2_CORE_RUNTIME_STATUS_OK;
	}
	if (requested_cpu == KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY)
		return time_current_logical_cpu(cpu_out);
	if (requested_cpu >= core_cpu_count)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*cpu_out = requested_cpu;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static int work_queue_covered_locked(
	struct kb2_core_workqueue_queue *queue, uint64_t epoch)
{
	struct kb2_core_workqueue_work *work;

	for (work = core_works; work; work = work->next) {
		if ((work->pending_queue == queue && work->pending_epoch &&
		     work->pending_epoch <= epoch) ||
		    (work->running_queue == queue && work->running_epoch &&
		     work->running_epoch <= epoch))
			return 1;
	}
	return 0;
}

static void work_ready_append_locked(
	struct kb2_core_workqueue_queue *queue,
	struct kb2_core_workqueue_work *work)
{
	work->ready_previous = queue->ready_tail;
	work->ready_next = NULL;
	if (queue->ready_tail)
		queue->ready_tail->ready_next = work;
	else
		queue->ready_head = work;
	queue->ready_tail = work;
	work->pending_state = CORE_WORK_READY;
}

static void work_ready_remove_locked(
	struct kb2_core_workqueue_queue *queue,
	struct kb2_core_workqueue_work *work)
{
	if (work->pending_state != CORE_WORK_READY)
		return;
	if (work->ready_previous)
		work->ready_previous->ready_next = work->ready_next;
	else
		queue->ready_head = work->ready_next;
	if (work->ready_next)
		work->ready_next->ready_previous = work->ready_previous;
	else
		queue->ready_tail = work->ready_previous;
	work->ready_previous = NULL;
	work->ready_next = NULL;
}

static int work_domain_ready_locked(
	struct kb2_core_workqueue_queue *queue, uint32_t domain)
{
	struct kb2_core_workqueue_work *work;

	for (work = queue->ready_head; work; work = work->ready_next) {
		if (!work->closing && !work->running &&
		    ((queue->flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND) ||
		     work->pending_cpu == domain))
			return 1;
	}
	return 0;
}

static int work_item_covered_locked(
	struct kb2_core_workqueue_work *work, uint64_t sequence)
{
	return (work->pending_sequence &&
		work->pending_sequence <= sequence) ||
	       (work->running_sequence &&
		work->running_sequence <= sequence);
}

static void work_queue_program_deadline(
	struct kb2_core_workqueue_queue *queue)
{
	struct kb2_core_workqueue_work *work;
	struct __kernel_itimerspec setting;
	uint64_t deadline = 0;
	long result;

	work_lock();
	for (work = core_works; work; work = work->next) {
		if (!work->closing &&
		    work->pending_state == CORE_WORK_DELAYED &&
		    work->pending_queue == queue &&
		    (!deadline || work->deadline_ns < deadline))
			deadline = work->deadline_ns;
	}
	work_unlock();
	bytes_zero(&setting, sizeof(setting));
	if (deadline) {
		setting.it_value.tv_sec = deadline / UINT64_C(1000000000);
		setting.it_value.tv_nsec = deadline % UINT64_C(1000000000);
	}
	result = core_linux_syscall6(
		__NR_timerfd_settime, queue->timer_descriptor,
		deadline ? TFD_TIMER_ABSTIME : 0,
		(long)(uintptr_t)&setting, 0, 0, 0);
	if (result < 0)
		__builtin_trap();
}

static void work_queue_promote_due(
	struct kb2_core_workqueue_queue *queue)
{
	uint64_t now;
	int promoted = 0;

	if (time_now_native(CLOCK_MONOTONIC, &now) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		__builtin_trap();
	work_lock();
	for (;;) {
		struct kb2_core_workqueue_work *candidate = NULL;
		struct kb2_core_workqueue_work *work;

		for (work = core_works; work; work = work->next) {
			if (work->closing ||
			    work->pending_state != CORE_WORK_DELAYED ||
			    work->pending_queue != queue ||
			    work->deadline_ns > now)
				continue;
			if (!candidate ||
			    work->deadline_ns < candidate->deadline_ns ||
			    (work->deadline_ns == candidate->deadline_ns &&
			     work->pending_epoch < candidate->pending_epoch))
				candidate = work;
		}
		if (!candidate)
			break;
		work_ready_append_locked(queue, candidate);
		promoted = 1;
	}
	work_unlock();
	if (promoted)
		work_queue_notify(queue);
}

static void *work_dispatcher_entry(void *argument)
{
	struct kb2_core_workqueue_queue *queue = argument;
	struct core_poll_descriptor descriptors[2];

	for (;;) {
		descriptors[0] = (struct core_poll_descriptor){
			.descriptor = queue->dispatcher_descriptor,
			.events = POLLIN,
		};
		descriptors[1] = (struct core_poll_descriptor){
			.descriptor = queue->timer_descriptor,
			.events = POLLIN,
		};
		if (core_linux_syscall6(__NR_poll,
					(long)(uintptr_t)descriptors, 2, -1,
					0, 0, 0) < 0)
			continue;
		time_descriptor_drain(queue->dispatcher_descriptor);
		time_descriptor_drain(queue->timer_descriptor);
		work_queue_promote_due(queue);
		if (__atomic_load_n(&queue->stop, __ATOMIC_ACQUIRE)) {
			work_queue_notify(queue);
			break;
		}
		work_queue_program_deadline(queue);
	}
	return NULL;
}

static struct kb2_core_workqueue_work *work_worker_take_locked(
	struct core_work_worker *worker)
{
	struct kb2_core_workqueue_queue *queue = worker->queue;
	struct kb2_core_workqueue_work *work;

	if (worker->rescuer) {
		if (queue->active_counts[worker->domain] < queue->maximum_active)
			return NULL;
	} else if (queue->active_counts[worker->domain] >=
		   queue->maximum_active) {
		return NULL;
	}
	for (work = queue->ready_head; work; work = work->ready_next) {
		if (work->closing || work->running ||
		    (!(queue->flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND) &&
		     work->pending_cpu != worker->cpu_id))
			continue;
		break;
	}
	if (!work)
		return NULL;
	work_ready_remove_locked(queue, work);
	work->running = 1;
	work->running_queue = queue;
	work->running_sequence = work->pending_sequence;
	work->running_epoch = work->pending_epoch;
	work->pending_queue = NULL;
	work->pending_sequence = 0;
	work->pending_epoch = 0;
	work->pending_state = CORE_WORK_IDLE;
	work->deadline_ns = 0;
	if (!worker->rescuer)
		queue->active_counts[worker->domain]++;
	return work;
}

static void *work_worker_entry(void *argument)
{
	struct core_work_worker *worker = argument;
	struct kb2_core_workqueue_queue *queue = worker->queue;
	struct kb2_core_thread *thread = &worker->thread;
	struct core_poll_descriptor descriptor;

	core_current_thread = thread;
	core_sync_thread_id = thread->identity;
	core_cpu_id = worker->cpu_id == KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY ?
			      0 : worker->cpu_id;
	core_cpu_context_class = KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD;
	thread->native = pthread_self();
	thread->native_tid = (uint32_t)core_linux_syscall6(
		__NR_gettid, 0, 0, 0, 0, 0, 0);
	(void)__atomic_add_fetch(&worker->startup_word, 1, __ATOMIC_RELEASE);
	sync_futex_wake(&worker->startup_word);
	for (;;) {
		descriptor = (struct core_poll_descriptor){
			.descriptor = queue->domain_descriptors[worker->domain],
			.events = POLLIN,
		};
		if (core_linux_syscall6(__NR_poll, (long)(uintptr_t)&descriptor,
					1, -1, 0, 0, 0) < 0)
			continue;
		time_descriptor_drain(queue->domain_descriptors[worker->domain]);
		for (;;) {
			struct kb2_core_workqueue_work *work;
			int handoff;

			work_lock();
			work = work_worker_take_locked(worker);
			handoff = !work && worker->rescuer &&
				  queue->active_counts[worker->domain] <
					  queue->maximum_active &&
				  work_domain_ready_locked(queue,
						   worker->domain);
			work_unlock();
			if (!work) {
				if (handoff) {
					time_descriptor_signal(
						queue->domain_descriptors[
							worker->domain]);
					(void)core_linux_syscall6(
						__NR_sched_yield, 0, 0, 0, 0, 0, 0);
				}
				break;
			}
			time_descriptor_signal(
				queue->domain_descriptors[worker->domain]);
			core_current_work = work;
			work->callback(work, work->argument);
			core_current_work = NULL;
			if (core_rcu_read_depth || core_cpu_local_states ||
			    thread_owns_lock(thread->identity))
				__builtin_trap();
			work_lock();
			if (!work->running || work->running_queue != queue)
				__builtin_trap();
			work->running = 0;
			work->running_queue = NULL;
			work->running_sequence = 0;
			work->running_epoch = 0;
			if (!worker->rescuer) {
				if (!queue->active_counts[worker->domain])
					__builtin_trap();
				queue->active_counts[worker->domain]--;
			}
			work_state_changed(queue, work);
			work_unlock();
		}
		if (__atomic_load_n(&queue->stop, __ATOMIC_ACQUIRE)) {
			time_descriptor_signal(
				queue->domain_descriptors[worker->domain]);
			break;
		}
	}
	core_current_thread = NULL;
	core_sync_thread_id = 0;
	core_cpu_id = 0;
	core_cpu_context_class = 0;
	return NULL;
}

static void work_native_mask(uint32_t cpu_id,
			     uint64_t mask[CORE_NATIVE_CPU_WORDS])
{
	uint32_t index;

	bytes_zero(mask, sizeof(uint64_t) * CORE_NATIVE_CPU_WORDS);
	if (cpu_id == KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY) {
		for (index = 0; index < core_cpu_count; index++) {
			uint32_t native_cpu = core_native_cpu_ids[index];

			mask[native_cpu / 64] |=
				UINT64_C(1) << (native_cpu % 64);
		}
	} else {
		uint32_t native_cpu = core_native_cpu_ids[cpu_id];

		mask[native_cpu / 64] |= UINT64_C(1) << (native_cpu % 64);
	}
}

static int work_queue_stop(struct kb2_core_workqueue_queue *queue)
{
	size_t index;
	int failed = 0;

	if (!queue->dispatcher_started && !queue->worker_count)
		return 0;
	__atomic_store_n(&queue->stop, 1, __ATOMIC_RELEASE);
	work_queue_notify(queue);
	if (queue->dispatcher_started) {
		if (pthread_join(queue->dispatcher, NULL))
			failed = 1;
		queue->dispatcher_started = 0;
	}
	for (index = 0; queue->workers && index < queue->worker_count;
	     index++) {
		struct core_work_worker *worker = &queue->workers[index];

		if (worker->started && pthread_join(worker->native, NULL))
			failed = 1;
		worker->started = 0;
		thread_wake_descriptor_close(&worker->thread);
	}
	return failed ? -1 : 0;
}

static void work_queue_close_descriptors(
	struct kb2_core_workqueue_queue *queue)
{
	int *descriptors[] = {
		&queue->dispatcher_descriptor,
		&queue->timer_descriptor,
	};
	size_t index;
	uint32_t domain;

	if (queue->domain_descriptors) {
		for (domain = 0; domain < queue->domain_count; domain++) {
			if (queue->domain_descriptors[domain] >= 0) {
				(void)core_linux_syscall6(
					__NR_close,
					queue->domain_descriptors[domain], 0, 0,
					0, 0, 0);
				queue->domain_descriptors[domain] = -1;
			}
		}
	}

	for (index = 0; index < sizeof(descriptors) / sizeof(descriptors[0]);
	     index++) {
		if (*descriptors[index] >= 0) {
			(void)core_linux_syscall6(
				__NR_close, *descriptors[index], 0, 0, 0, 0, 0);
			*descriptors[index] = -1;
		}
	}
}

static void work_queue_release_internal(
	struct kb2_core_workqueue_queue *queue)
{
	if (queue->workers) {
		(void)kobox_provider_arena_release_owned(
			core_arena, queue->workers, queue->worker_order,
			queue->binding, CORE_ALLOCATION_WORK_INTERNAL);
		queue->workers = NULL;
	}
	if (queue->active_counts) {
		(void)kobox_provider_arena_release_owned(
			core_arena, queue->active_counts, queue->active_order,
			queue->binding, CORE_ALLOCATION_WORK_INTERNAL);
		queue->active_counts = NULL;
	}
	if (queue->domain_descriptors) {
		(void)kobox_provider_arena_release_owned(
			core_arena, queue->domain_descriptors,
			queue->domain_order, queue->binding,
			CORE_ALLOCATION_WORK_INTERNAL);
		queue->domain_descriptors = NULL;
	}
}

static kb2_core_status_t work_queue_create(
	void *binding_object, const char *name, size_t name_length,
	uint32_t flags, uint32_t maximum_active,
	kb2_core_workqueue_queue_t *queue_out)
{
	struct core_binding *binding = binding_object;
	struct core_binding *thread_binding;
	struct kb2_core_workqueue_queue *queue;
	kb2_core_status_t status;
	size_t normal_count;
	size_t worker_count;
	size_t index;
	uint32_t domain_count;
	uint32_t worker_order;
	uint32_t active_order;
	uint32_t domain_order;

	if (!queue_out || !thread_name_valid(name, name_length) ||
	    flags & ~(KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND |
		      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_HIGH_PRIORITY |
		      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_MEMORY_RECLAIM |
		      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_FREEZABLE |
		      KB2_CORE_RUNTIME_WORKQUEUE_FLAG_ORDERED) ||
	    ((flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_ORDERED) &&
	     (!(flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND) ||
	      (flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_MEMORY_RECLAIM) ||
	      maximum_active != 1)))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*queue_out = NULL;
	status = work_binding_validate(binding, 1);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	thread_binding = time_thread_binding(binding->node_id);
	if (!thread_binding)
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	if (!maximum_active)
		maximum_active = 1;
	domain_count = flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND ?
			       1 : core_cpu_count;
	if (maximum_active > SIZE_MAX / domain_count)
		return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	normal_count = (size_t)maximum_active * domain_count;
	if (normal_count > SIZE_MAX -
				   ((flags &
				     KB2_CORE_RUNTIME_WORKQUEUE_FLAG_MEMORY_RECLAIM) ?
					    domain_count : 0))
		return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	worker_count = normal_count +
		       ((flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_MEMORY_RECLAIM) ?
				domain_count : 0);
	if (worker_count > SIZE_MAX / sizeof(struct core_work_worker) ||
	    !allocation_order(worker_count * sizeof(struct core_work_worker),
			      &worker_order) ||
	    !allocation_order((size_t)domain_count * sizeof(uint32_t),
			      &active_order) ||
	    !allocation_order((size_t)domain_count * sizeof(int),
			      &domain_order))
		return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	status = binding_object_begin(
		binding, KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	queue = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_WORKQUEUE);
	if (!queue) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(queue, CORE_PAGE_SIZE);
	queue->dispatcher_descriptor = -1;
	queue->timer_descriptor = -1;
	queue->magic = CORE_WORKQUEUE_MAGIC;
	queue->binding = binding;
	queue->thread_binding = thread_binding;
	queue->generation = core_generation;
	queue->flags = flags;
	queue->maximum_active = maximum_active;
	queue->domain_count = domain_count;
	queue->normal_worker_count = normal_count;
	queue->worker_count = worker_count;
	queue->worker_order = worker_order;
	queue->active_order = active_order;
	queue->domain_order = domain_order;
	queue->accepting = 1;
	if (name_length)
		bytes_copy(queue->name, name, name_length);
	queue->workers = kobox_provider_arena_allocate_owned(
		core_arena, worker_order, binding,
		CORE_ALLOCATION_WORK_INTERNAL);
	queue->active_counts = kobox_provider_arena_allocate_owned(
		core_arena, active_order, binding,
		CORE_ALLOCATION_WORK_INTERNAL);
	queue->domain_descriptors = kobox_provider_arena_allocate_owned(
		core_arena, domain_order, binding,
		CORE_ALLOCATION_WORK_INTERNAL);
	if (!queue->workers || !queue->active_counts ||
	    !queue->domain_descriptors) {
		status = KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
		goto fail;
	}
	bytes_zero(queue->workers,
		   ((size_t)CORE_PAGE_SIZE << worker_order));
	for (index = 0; index < worker_count; index++)
		queue->workers[index].thread.wake_descriptor = -1;
	bytes_zero(queue->active_counts,
		   ((size_t)CORE_PAGE_SIZE << active_order));
	for (index = 0; index < domain_count; index++)
		queue->domain_descriptors[index] = -1;
	queue->dispatcher_descriptor = thread_wake_descriptor_create();
	for (index = 0; index < domain_count; index++)
		queue->domain_descriptors[index] =
			thread_wake_descriptor_create();
	queue->timer_descriptor = (int)core_linux_syscall6(
		__NR_timerfd_create, CLOCK_MONOTONIC,
		TFD_CLOEXEC | TFD_NONBLOCK, 0, 0, 0, 0);
	if (queue->dispatcher_descriptor < 0 || queue->timer_descriptor < 0) {
		status = KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
		goto fail;
	}
	for (index = 0; index < domain_count; index++) {
		if (queue->domain_descriptors[index] < 0) {
			status = KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
			goto fail;
		}
	}
	if (pthread_create(&queue->dispatcher, NULL, work_dispatcher_entry,
			   queue)) {
		status = KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
		goto fail;
	}
	queue->dispatcher_started = 1;
	for (index = 0; index < worker_count; index++) {
		struct core_work_worker *worker = &queue->workers[index];
		uint64_t native_mask[CORE_NATIVE_CPU_WORDS];
		uint32_t domain = index < normal_count ?
					  (uint32_t)(index / maximum_active) :
					  (uint32_t)(index - normal_count);
		uint32_t cpu_id = flags &
					  KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND ?
					  KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY :
					  domain;
		uint32_t expected;

		worker->queue = queue;
		worker->cpu_id = cpu_id;
		worker->domain = domain;
		worker->rescuer = index >= normal_count;
		worker->thread.wake_descriptor = -1;
		worker->thread.wake_descriptor =
			thread_wake_descriptor_create();
		if (worker->thread.wake_descriptor < 0) {
			status = KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
			goto fail;
		}
		worker->thread.magic = CORE_THREAD_MAGIC;
		worker->thread.binding = thread_binding;
		worker->thread.identity = thread_allocate_identity();
		worker->thread.generation = core_generation;
		worker->thread.state = CORE_THREAD_RUNNING;
		worker->thread.borrowed = 1;
		worker->thread.logical_cpu =
			cpu_id == KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY ? 0 : cpu_id;
		worker->thread.priority =
			flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_HIGH_PRIORITY ?
				KB2_CORE_RUNTIME_THREAD_PRIORITY_HIGHEST :
				KB2_CORE_RUNTIME_THREAD_PRIORITY_DEFAULT;
		if (pthread_create(&worker->native, NULL, work_worker_entry,
				   worker)) {
			status = KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
			goto fail;
		}
		worker->started = 1;
		while (!__atomic_load_n(&worker->startup_word,
					 __ATOMIC_ACQUIRE)) {
			expected = __atomic_load_n(&worker->startup_word,
						   __ATOMIC_RELAXED);
			(void)sync_futex_wait(&worker->startup_word, expected, 0);
		}
		work_native_mask(cpu_id, native_mask);
		status = thread_native_status(pthread_setaffinity_np(
			worker->native, sizeof(native_mask),
			(const cpu_set_t *)native_mask));
		if (status == KB2_CORE_RUNTIME_STATUS_OK &&
		    (flags & KB2_CORE_RUNTIME_WORKQUEUE_FLAG_HIGH_PRIORITY))
			status = thread_set_native_priority(
				&worker->thread,
				KB2_CORE_RUNTIME_THREAD_PRIORITY_HIGHEST);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			goto fail;
	}
	work_lock();
	if (!core_work_active || core_work_queue_count == SIZE_MAX) {
		status = core_work_queue_count == SIZE_MAX ?
				 KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
				 KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		work_unlock();
		goto fail;
	}
	queue->next = core_work_queues;
	core_work_queues = queue;
	core_work_queue_count++;
	work_unlock();
	*queue_out = queue;
	return KB2_CORE_RUNTIME_STATUS_OK;

fail:
	(void)work_queue_stop(queue);
	work_queue_close_descriptors(queue);
	work_queue_release_internal(queue);
	queue->magic = 0;
	(void)kobox_provider_arena_release_owned(
		core_arena, queue, 0, binding, CORE_ALLOCATION_WORKQUEUE);
	binding_object_end(binding);
	return status;
}

static kb2_core_status_t work_create(
	void *binding_object, kb2_core_work_callback_fn callback, void *argument,
	kb2_core_workqueue_work_t *work_out)
{
	struct core_binding *binding = binding_object;
	struct kb2_core_workqueue_work *work;
	kb2_core_status_t status;

	if (!callback || !work_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*work_out = NULL;
	status = binding_object_begin(
		binding, KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!__atomic_load_n(&core_work_active, __ATOMIC_ACQUIRE)) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	}
	work = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_WORK);
	if (!work) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(work, CORE_PAGE_SIZE);
	work->magic = CORE_WORK_MAGIC;
	work->binding = binding;
	work->callback = callback;
	work->argument = argument;
	work->generation = core_generation;
	work_lock();
	if (!core_work_active || core_work_count == SIZE_MAX) {
		status = core_work_count == SIZE_MAX ?
				 KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
				 KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		work_unlock();
		work->magic = 0;
		(void)kobox_provider_arena_release_owned(
			core_arena, work, 0, binding, CORE_ALLOCATION_WORK);
		binding_object_end(binding);
		return status;
	}
	work->next = core_works;
	core_works = work;
	core_work_count++;
	work_unlock();
	*work_out = work;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t work_submit_common(
	struct core_binding *binding, struct kb2_core_workqueue_queue *queue,
	struct kb2_core_workqueue_work *work, uint32_t requested_cpu,
	uint64_t deadline_ns, int delayed, int replace,
	uint32_t *result_out)
{
	kb2_core_status_t status;
	uint64_t now = 0;
	uint32_t cpu_id;
	int ready = !delayed;

	if (!result_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*result_out = 0;
	status = work_queue_validate(binding, queue);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = work_validate(binding, work);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = work_submission_cpu(queue, requested_cpu, &cpu_id);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (delayed) {
		status = time_now_native(CLOCK_MONOTONIC, &now);
		if (status != KB2_CORE_RUNTIME_STATUS_OK)
			return status;
		ready = !deadline_ns || deadline_ns <= now;
	}
	work_lock();
	if (!core_work_active || !queue->accepting || queue->closing ||
	    work->closing) {
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		goto out;
	}
	if ((work->running_queue && work->running_queue != queue) ||
	    (work->pending_queue && work->pending_queue != queue)) {
		status = KB2_CORE_RUNTIME_STATUS_BUSY;
		goto out;
	}
	if (work->pending_state != CORE_WORK_IDLE) {
		if (!replace) {
			status = KB2_CORE_RUNTIME_STATUS_OK;
			goto out;
		}
		if (work->pending_state == CORE_WORK_READY && !ready) {
			work_ready_remove_locked(queue, work);
			work->pending_state = CORE_WORK_DELAYED;
		} else if (work->pending_state == CORE_WORK_DELAYED && ready) {
			work_ready_append_locked(queue, work);
		}
		work->pending_cpu = cpu_id;
		work->deadline_ns = ready ? 0 : deadline_ns;
		*result_out = 1;
		status = KB2_CORE_RUNTIME_STATUS_OK;
		goto out;
	}
	if (work->sequence == UINT64_MAX ||
	    queue->submission_epoch == UINT64_MAX) {
		status = KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
		goto out;
	}
	work->sequence++;
	queue->submission_epoch++;
	work->pending_queue = queue;
	work->pending_sequence = work->sequence;
	work->pending_epoch = queue->submission_epoch;
	work->pending_cpu = cpu_id;
	work->deadline_ns = ready ? 0 : deadline_ns;
	work->pending_state = CORE_WORK_DELAYED;
	if (ready)
		work_ready_append_locked(queue, work);
	*result_out = replace ? 0 : 1;
	status = KB2_CORE_RUNTIME_STATUS_OK;
out:
	work_unlock();
	if (status == KB2_CORE_RUNTIME_STATUS_OK)
		work_queue_notify(queue);
	return status;
}

static kb2_core_status_t work_submit(
	void *binding_object, kb2_core_workqueue_queue_t queue,
	kb2_core_workqueue_work_t work, uint32_t cpu_id, uint32_t *queued_out)
{
	return work_submit_common(binding_object, queue, work, cpu_id, 0, 0, 0,
				  queued_out);
}

static kb2_core_status_t work_submit_at(
	void *binding_object, kb2_core_workqueue_queue_t queue,
	kb2_core_workqueue_work_t work, uint32_t cpu_id, uint64_t deadline_ns,
	uint32_t *queued_out)
{
	return work_submit_common(binding_object, queue, work, cpu_id,
				  deadline_ns, 1, 0, queued_out);
}

static kb2_core_status_t work_reschedule_at(
	void *binding_object, kb2_core_workqueue_queue_t queue,
	kb2_core_workqueue_work_t work, uint32_t cpu_id, uint64_t deadline_ns,
	uint32_t *pending_was_replaced_out)
{
	return work_submit_common(binding_object, queue, work, cpu_id,
				  deadline_ns, 1, 1,
				  pending_was_replaced_out);
}

static kb2_core_status_t work_cancel_common(
	struct core_binding *binding, struct kb2_core_workqueue_work *work,
	int wait, uint32_t *canceled_out)
{
	kb2_core_status_t status;
	uint32_t canceled = 0;

	if (!canceled_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*canceled_out = 0;
	status = work_validate(binding, work);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (wait && core_current_work == work)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	for (;;) {
		struct kb2_core_workqueue_queue *queue;
		uint32_t expected;
		int running;

		work_lock();
		queue = work->pending_queue;
		if (work->pending_state != CORE_WORK_IDLE) {
			canceled = 1;
			if (work->pending_state == CORE_WORK_READY)
				work_ready_remove_locked(queue, work);
			work->pending_state = CORE_WORK_IDLE;
			work->pending_queue = NULL;
			work->pending_sequence = 0;
			work->pending_epoch = 0;
			work->deadline_ns = 0;
		}
		running = work->running;
		expected = __atomic_load_n(&work->drain_word,
					   __ATOMIC_ACQUIRE);
		work_unlock();
		if (queue)
			work_state_changed(queue, work);
		if (!wait || !running)
			break;
		(void)sync_futex_wait(&work->drain_word, expected, 0);
	}
	*canceled_out = canceled;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t work_cancel(void *binding_object,
				      kb2_core_workqueue_work_t work,
				      uint32_t *canceled_out)
{
	return work_cancel_common(binding_object, work, 0, canceled_out);
}

static kb2_core_status_t work_cancel_sync(void *binding_object,
					   kb2_core_workqueue_work_t work,
					   uint32_t *canceled_out)
{
	return work_cancel_common(binding_object, work, 1, canceled_out);
}

static kb2_core_status_t work_flush_work(
	void *binding_object, kb2_core_workqueue_work_t work)
{
	kb2_core_status_t status = work_validate(binding_object, work);
	uint64_t target;

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (core_current_work == work)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	work_lock();
	target = work->sequence;
	for (;;) {
		uint32_t expected;

		if (!work_item_covered_locked(work, target)) {
			work_unlock();
			return KB2_CORE_RUNTIME_STATUS_OK;
		}
		expected = __atomic_load_n(&work->drain_word, __ATOMIC_ACQUIRE);
		work_unlock();
		(void)sync_futex_wait(&work->drain_word, expected, 0);
		work_lock();
	}
}

static kb2_core_status_t work_flush_queue(
	void *binding_object, kb2_core_workqueue_queue_t queue)
{
	kb2_core_status_t status = work_queue_validate(binding_object, queue);
	uint64_t target;

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (core_current_work && core_current_work->running_queue == queue)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	work_lock();
	target = queue->submission_epoch;
	for (;;) {
		uint32_t expected;

		if (!work_queue_covered_locked(queue, target)) {
			work_unlock();
			return KB2_CORE_RUNTIME_STATUS_OK;
		}
		expected = __atomic_load_n(&queue->drain_word, __ATOMIC_ACQUIRE);
		work_unlock();
		(void)sync_futex_wait(&queue->drain_word, expected, 0);
		work_lock();
	}
}

static kb2_core_status_t work_is_pending(
	void *binding_object, kb2_core_workqueue_work_t work,
	uint32_t *pending_out)
{
	kb2_core_status_t status;

	if (!pending_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = work_validate(binding_object, work);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	work_lock();
	*pending_out = work->pending_state != CORE_WORK_IDLE;
	work_unlock();
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t work_destroy(
	void *binding_object, kb2_core_workqueue_work_t work)
{
	struct core_binding *binding = binding_object;
	struct kb2_core_workqueue_work **link;
	struct kb2_core_workqueue_queue *queue;
	kb2_core_status_t status = work_validate(binding, work);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (core_current_work == work)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	work_lock();
	work->closing = 1;
	queue = work->pending_queue;
	if (work->pending_state == CORE_WORK_READY)
		work_ready_remove_locked(queue, work);
	work->pending_state = CORE_WORK_IDLE;
	work->pending_queue = NULL;
	work->pending_sequence = 0;
	work->pending_epoch = 0;
	work->deadline_ns = 0;
	work_unlock();
	if (queue)
		work_state_changed(queue, work);
	for (;;) {
		uint32_t expected;

		work_lock();
		if (!work->running) {
			work_unlock();
			break;
		}
		expected = __atomic_load_n(&work->drain_word,
					   __ATOMIC_ACQUIRE);
		work_unlock();
		(void)sync_futex_wait(&work->drain_word, expected, 0);
	}
	work_lock();
	for (link = &core_works; *link && *link != work;
	     link = &(*link)->next)
		;
	if (!*link || !core_work_count) {
		work_unlock();
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	*link = work->next;
	work->next = NULL;
	core_work_count--;
	work->magic = 0;
	work_unlock();
	binding_object_end(binding);
	return kobox_provider_arena_release_owned(
		       core_arena, work, 0, binding, CORE_ALLOCATION_WORK) ==
		       KOBOX_PROVIDER_ARENA_OK ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

static kb2_core_status_t work_queue_destroy(
	void *binding_object, kb2_core_workqueue_queue_t queue)
{
	struct core_binding *binding = binding_object;
	struct kb2_core_workqueue_queue **link;
	kb2_core_status_t status = work_queue_validate(binding, queue);
	uint64_t target;

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (core_current_work && core_current_work->running_queue == queue)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	work_lock();
	queue->accepting = 0;
	queue->closing = 1;
	target = queue->submission_epoch;
	for (;;) {
		uint32_t expected;

		if (!work_queue_covered_locked(queue, target))
			break;
		expected = __atomic_load_n(&queue->drain_word, __ATOMIC_ACQUIRE);
		work_unlock();
		work_queue_notify(queue);
		(void)sync_futex_wait(&queue->drain_word, expected, 0);
		work_lock();
	}
	work_unlock();
	if (work_queue_stop(queue))
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	work_lock();
	for (link = &core_work_queues; *link && *link != queue;
	     link = &(*link)->next)
		;
	if (!*link || !core_work_queue_count) {
		work_unlock();
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	*link = queue->next;
	queue->next = NULL;
	core_work_queue_count--;
	queue->magic = 0;
	work_unlock();
	work_queue_close_descriptors(queue);
	work_queue_release_internal(queue);
	binding_object_end(binding);
	return kobox_provider_arena_release_owned(
		       core_arena, queue, 0, binding,
		       CORE_ALLOCATION_WORKQUEUE) == KOBOX_PROVIDER_ARENA_OK ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

static int work_lifecycle_init(const struct kobox_module_context *context)
{
	(void)context;
	if (core_work_active || core_work_queues || core_works ||
	    core_work_queue_count || core_work_count)
		return -1;
	__atomic_store_n(&core_work_active, 1, __ATOMIC_RELEASE);
	return 0;
}

static int work_lifecycle_quiesce(const struct kobox_module_context *context)
{
	struct kb2_core_workqueue_queue *queue;
	struct kb2_core_workqueue_work *work;

	(void)context;
	if (!__atomic_exchange_n(&core_work_active, 0, __ATOMIC_ACQ_REL))
		return -1;
	work_lock();
	for (queue = core_work_queues; queue; queue = queue->next)
		queue->accepting = 0;
	for (work = core_works; work; work = work->next) {
		queue = work->pending_queue;
		if (!queue || !(queue->flags &
				 KB2_CORE_RUNTIME_WORKQUEUE_FLAG_FREEZABLE))
			continue;
		if (work->pending_state == CORE_WORK_READY)
			work_ready_remove_locked(queue, work);
		work->pending_state = CORE_WORK_IDLE;
		work->pending_queue = NULL;
		work->pending_sequence = 0;
		work->pending_epoch = 0;
		work->deadline_ns = 0;
		(void)__atomic_add_fetch(&work->drain_word, 1,
					 __ATOMIC_RELEASE);
		sync_futex_wake(&work->drain_word);
		(void)__atomic_add_fetch(&queue->drain_word, 1,
					 __ATOMIC_RELEASE);
		sync_futex_wake(&queue->drain_word);
	}
	work_unlock();
	for (queue = core_work_queues; queue; queue = queue->next)
		work_queue_notify(queue);
	for (queue = core_work_queues; queue; queue = queue->next) {
		uint64_t target;

		work_lock();
		target = queue->submission_epoch;
		for (;;) {
			uint32_t expected;

			if (!work_queue_covered_locked(queue, target))
				break;
			expected = __atomic_load_n(&queue->drain_word,
						   __ATOMIC_ACQUIRE);
			work_unlock();
			work_queue_notify(queue);
			(void)sync_futex_wait(&queue->drain_word, expected, 0);
			work_lock();
		}
		work_unlock();
		if (work_queue_stop(queue))
			return -1;
	}
	return 0;
}

static int work_lifecycle_cleanup(const struct kobox_module_context *context)
{
	int empty;

	(void)context;
	work_lock();
	empty = !core_work_active && !core_work_queues && !core_works &&
		!core_work_queue_count && !core_work_count;
	work_unlock();
	return empty ? 0 : -1;
}

static void rcu_lock(void)
{
	while (__atomic_exchange_n(&core_rcu_lock_word, 1,
				   __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#endif
	}
}

static void rcu_unlock(void)
{
	__atomic_store_n(&core_rcu_lock_word, 0, __ATOMIC_RELEASE);
}

static void rcu_notify(struct kb2_core_rcu_domain *domain)
{
	if (domain) {
		(void)__atomic_add_fetch(&domain->wait_word, 1,
					 __ATOMIC_RELEASE);
		sync_futex_wake(&domain->wait_word);
	}
	if (core_rcu_descriptor >= 0)
		time_descriptor_signal(core_rcu_descriptor);
}

static kb2_core_status_t rcu_binding_validate(void *binding_object,
					       int allocation)
{
	kb2_core_status_t status;

	core_lock();
	status = binding_validate_locked(binding_object,
					 KB2_CORE_RUNTIME_INTERFACE_RCU,
					 allocation);
	if (status == KB2_CORE_RUNTIME_STATUS_OK && allocation &&
	    !__atomic_load_n(&core_rcu_active, __ATOMIC_ACQUIRE))
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	core_unlock();
	return status;
}

static int rcu_domain_contains_locked(
	const struct kb2_core_rcu_domain *domain)
{
	const struct kb2_core_rcu_domain *current;

	for (current = core_rcu_domains; current; current = current->next) {
		if (current == domain)
			return 1;
	}
	return 0;
}

static kb2_core_status_t rcu_domain_validate(
	struct core_binding *binding, struct kb2_core_rcu_domain *domain,
	int allow_closing)
{
	kb2_core_status_t status = rcu_binding_validate(binding, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!domain)
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	if (domain != &core_rcu_default_domain &&
	    !kobox_provider_arena_contains(core_arena, domain, sizeof(*domain)))
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	rcu_lock();
	if (domain->magic != CORE_RCU_DOMAIN_MAGIC ||
	    domain->generation != core_generation ||
	    !rcu_domain_contains_locked(domain) ||
	    (!domain->default_domain && domain->binding != binding))
		status = KB2_CORE_RUNTIME_STATUS_OWNER;
	else if (domain->closing && !allow_closing)
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	rcu_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK || domain->default_domain)
		return status;
	return kobox_provider_arena_validate_owner(
		       core_arena, domain, 0, binding,
		       CORE_ALLOCATION_RCU_DOMAIN) ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_OWNER;
}

static int rcu_readers_covered_locked(
	struct kb2_core_rcu_domain *domain, uint64_t target)
{
	struct kb2_core_rcu_read_token *token;

	for (token = domain->readers; token; token = token->next) {
		if (token->sequence <= target)
			return 1;
	}
	return 0;
}

static int rcu_owner_reader_covered_locked(
	struct kb2_core_rcu_domain *domain, uint64_t owner, uint64_t target)
{
	struct kb2_core_rcu_read_token *token;

	for (token = domain->readers; token; token = token->next) {
		if (token->owner == owner && token->sequence <= target)
			return 1;
	}
	return 0;
}

static void rcu_wait_readers(struct kb2_core_rcu_domain *domain,
			     uint64_t target)
{
	for (;;) {
		uint32_t expected;

		rcu_lock();
		if (!rcu_readers_covered_locked(domain, target)) {
			rcu_unlock();
			break;
		}
		expected = __atomic_load_n(&domain->wait_word,
					   __ATOMIC_ACQUIRE);
		rcu_unlock();
		(void)sync_futex_wait(&domain->wait_word, expected, 0);
	}
}

static void rcu_wait_callbacks(struct kb2_core_rcu_domain *domain,
			       uint64_t target)
{
	for (;;) {
		uint32_t expected;

		rcu_lock();
		if (domain->callback_completed >= target) {
			rcu_unlock();
			break;
		}
		expected = __atomic_load_n(&domain->wait_word,
					   __ATOMIC_ACQUIRE);
		rcu_unlock();
		if (core_rcu_descriptor >= 0)
			time_descriptor_signal(core_rcu_descriptor);
		(void)sync_futex_wait(&domain->wait_word, expected, 0);
	}
}

static struct core_rcu_callback *rcu_callback_take_locked(void)
{
	struct kb2_core_rcu_domain *domain;

	for (domain = core_rcu_domains; domain; domain = domain->next) {
		struct core_rcu_callback *callback = domain->callback_head;

		if (!callback || domain->running_callback_sequence ||
		    rcu_readers_covered_locked(domain,
					       callback->reader_target))
			continue;
		domain->callback_head = callback->next;
		if (!domain->callback_head)
			domain->callback_tail = NULL;
		callback->next = NULL;
		domain->running_callback_sequence = callback->sequence;
		domain->running_reader_target = callback->reader_target;
		return callback;
	}
	return NULL;
}

static void *rcu_worker_entry(void *argument)
{
	struct core_poll_descriptor descriptor;
	struct kb2_core_thread *thread = &core_rcu_worker_thread;

	(void)argument;
	core_current_thread = thread;
	core_sync_thread_id = thread->identity;
	thread->native = pthread_self();
	thread->native_tid = (uint32_t)core_linux_syscall6(
		__NR_gettid, 0, 0, 0, 0, 0, 0);
	(void)__atomic_add_fetch(&core_rcu_worker_startup_word, 1,
				 __ATOMIC_RELEASE);
	sync_futex_wake(&core_rcu_worker_startup_word);
	for (;;) {
		descriptor = (struct core_poll_descriptor){
			.descriptor = core_rcu_descriptor,
			.events = POLLIN,
		};
		if (core_linux_syscall6(__NR_poll,
					(long)(uintptr_t)&descriptor, 1, -1,
					0, 0, 0) < 0)
			continue;
		time_descriptor_drain(core_rcu_descriptor);
		for (;;) {
			struct core_binding *thread_binding;
			struct core_rcu_callback *callback;
			struct kb2_core_rcu_domain *domain;
			uint32_t cpu_id;

			rcu_lock();
			callback = rcu_callback_take_locked();
			rcu_unlock();
			if (!callback)
				break;
			domain = callback->domain;
			if (callback->magic != CORE_RCU_CALLBACK_MAGIC ||
			    callback->generation != core_generation ||
			    !callback->binding || !callback->callback ||
			    domain->magic != CORE_RCU_DOMAIN_MAGIC)
				__builtin_trap();
			thread_binding = time_thread_binding(
				callback->binding->node_id);
			if (!thread_binding ||
			    time_current_logical_cpu(&cpu_id) !=
				    KB2_CORE_RUNTIME_STATUS_OK)
				__builtin_trap();
			thread->binding = thread_binding;
			thread->logical_cpu = cpu_id;
			core_cpu_id = cpu_id;
			core_cpu_context_class =
				KB2_CORE_RUNTIME_CPU_CONTEXT_THREAD;
			core_current_rcu_callback = callback;
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			callback->callback(callback->argument);
			core_current_rcu_callback = NULL;
			if (core_rcu_read_depth || core_cpu_local_states ||
			    thread_owns_lock(thread->identity))
				__builtin_trap();
			thread->binding = NULL;
			__atomic_thread_fence(__ATOMIC_RELEASE);
			rcu_lock();
			if (domain->running_callback_sequence !=
				    callback->sequence ||
			    domain->callback_completed + 1 != callback->sequence ||
			    !core_rcu_callback_count)
				__builtin_trap();
			domain->callback_completed = callback->sequence;
			domain->running_callback_sequence = 0;
			domain->running_reader_target = 0;
			core_rcu_callback_count--;
			rcu_notify(domain);
			rcu_unlock();
			callback->magic = 0;
			binding_object_end(callback->binding);
			if (kobox_provider_arena_release_owned(
				    core_arena, callback, 0, callback->binding,
				    CORE_ALLOCATION_RCU_CALLBACK) !=
			    KOBOX_PROVIDER_ARENA_OK)
				__builtin_trap();
		}
		if (__atomic_load_n(&core_rcu_stop, __ATOMIC_ACQUIRE))
			break;
	}
	core_current_thread = NULL;
	core_sync_thread_id = 0;
	core_cpu_id = 0;
	core_cpu_context_class = 0;
	return NULL;
}

static kb2_core_status_t rcu_default_domain(
	void *binding_object, kb2_core_rcu_domain_t *domain_out)
{
	kb2_core_status_t status;

	if (!domain_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*domain_out = NULL;
	status = rcu_binding_validate(binding_object, 1);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	*domain_out = &core_rcu_default_domain;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t rcu_domain_create(
	void *binding_object, uint32_t domain_class,
	kb2_core_rcu_domain_t *domain_out)
{
	struct core_binding *binding = binding_object;
	struct kb2_core_rcu_domain *domain;
	kb2_core_status_t status;

	if (!domain_out ||
	    (domain_class != KB2_CORE_RUNTIME_RCU_DOMAIN_CLASSIC &&
	     domain_class != KB2_CORE_RUNTIME_RCU_DOMAIN_SRCU))
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*domain_out = NULL;
	status = binding_object_begin(binding, KB2_CORE_RUNTIME_INTERFACE_RCU);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	domain = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_RCU_DOMAIN);
	if (!domain) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(domain, CORE_PAGE_SIZE);
	domain->magic = CORE_RCU_DOMAIN_MAGIC;
	domain->binding = binding;
	domain->generation = core_generation;
	domain->domain_class = domain_class;
	rcu_lock();
	if (!__atomic_load_n(&core_rcu_active, __ATOMIC_ACQUIRE) ||
	    core_rcu_domain_count == SIZE_MAX) {
		status = core_rcu_domain_count == SIZE_MAX ?
				 KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
				 KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		rcu_unlock();
		domain->magic = 0;
		(void)kobox_provider_arena_release_owned(
			core_arena, domain, 0, binding,
			CORE_ALLOCATION_RCU_DOMAIN);
		binding_object_end(binding);
		return status;
	}
	domain->next = core_rcu_domains;
	core_rcu_domains = domain;
	core_rcu_domain_count++;
	rcu_unlock();
	*domain_out = domain;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t rcu_read_lock(
	void *binding_object, kb2_core_rcu_domain_t domain,
	kb2_core_rcu_read_token_t *token_out)
{
	struct core_binding *binding = binding_object;
	struct kb2_core_rcu_read_token *token;
	kb2_core_status_t status;

	if (!token_out)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	*token_out = NULL;
	if (core_rcu_read_depth == UINT32_MAX)
		return KB2_CORE_RUNTIME_STATUS_EXHAUSTED;
	status = rcu_domain_validate(binding, domain, 0);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = binding_object_begin(binding, KB2_CORE_RUNTIME_INTERFACE_RCU);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	token = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_RCU_TOKEN);
	if (!token) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(token, CORE_PAGE_SIZE);
	token->magic = CORE_RCU_TOKEN_MAGIC;
	token->binding = binding;
	token->domain = domain;
	token->generation = core_generation;
	token->owner = sync_current_thread();
	rcu_lock();
	if (!__atomic_load_n(&core_rcu_active, __ATOMIC_ACQUIRE) ||
	    domain->closing ||
	    domain->reader_sequence == UINT64_MAX ||
	    core_rcu_token_count == SIZE_MAX) {
		status = domain->reader_sequence == UINT64_MAX ||
				 core_rcu_token_count == SIZE_MAX ?
				 KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
				 KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		rcu_unlock();
		token->magic = 0;
		(void)kobox_provider_arena_release_owned(
			core_arena, token, 0, binding,
			CORE_ALLOCATION_RCU_TOKEN);
		binding_object_end(binding);
		return status;
	}
	token->sequence = ++domain->reader_sequence;
	token->next = domain->readers;
	domain->readers = token;
	core_rcu_token_count++;
	rcu_unlock();
	core_rcu_read_depth++;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	*token_out = token;
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t rcu_read_unlock(
	void *binding_object, kb2_core_rcu_domain_t domain,
	kb2_core_rcu_read_token_t token)
{
	struct core_binding *binding = binding_object;
	struct kb2_core_rcu_read_token **link;
	kb2_core_status_t status = rcu_domain_validate(binding, domain, 1);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (!token || !kobox_provider_arena_contains(
			      core_arena, token, sizeof(*token)))
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	rcu_lock();
	for (link = &domain->readers; *link && *link != token;
	     link = &(*link)->next)
		;
	if (!*link || token->magic != CORE_RCU_TOKEN_MAGIC ||
	    token->binding != binding || token->domain != domain ||
	    token->generation != core_generation ||
	    token->owner != sync_current_thread() || !core_rcu_token_count ||
	    !kobox_provider_arena_validate_owner(
		    core_arena, token, 0, binding,
		    CORE_ALLOCATION_RCU_TOKEN)) {
		rcu_unlock();
		return KB2_CORE_RUNTIME_STATUS_OWNER;
	}
	*link = token->next;
	token->next = NULL;
	token->magic = 0;
	core_rcu_token_count--;
	rcu_notify(domain);
	rcu_unlock();
	if (!core_rcu_read_depth)
		__builtin_trap();
	core_rcu_read_depth--;
	binding_object_end(binding);
	return kobox_provider_arena_release_owned(
		       core_arena, token, 0, binding,
		       CORE_ALLOCATION_RCU_TOKEN) == KOBOX_PROVIDER_ARENA_OK ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

static kb2_core_status_t rcu_call(
	void *binding_object, kb2_core_rcu_domain_t domain,
	kb2_core_rcu_callback_fn callback_function, void *argument)
{
	struct core_binding *binding = binding_object;
	struct core_rcu_callback *callback;
	kb2_core_status_t status = rcu_domain_validate(binding, domain, 0);

	if (!callback_function)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	status = binding_object_begin(binding, KB2_CORE_RUNTIME_INTERFACE_RCU);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	callback = kobox_provider_arena_allocate_owned(
		core_arena, 0, binding, CORE_ALLOCATION_RCU_CALLBACK);
	if (!callback) {
		binding_object_end(binding);
		return KB2_CORE_RUNTIME_STATUS_NO_MEMORY;
	}
	bytes_zero(callback, CORE_PAGE_SIZE);
	callback->magic = CORE_RCU_CALLBACK_MAGIC;
	callback->binding = binding;
	callback->domain = domain;
	callback->callback = callback_function;
	callback->argument = argument;
	callback->generation = core_generation;
	rcu_lock();
	if (!__atomic_load_n(&core_rcu_active, __ATOMIC_ACQUIRE) ||
	    domain->closing ||
	    domain->callback_sequence == UINT64_MAX ||
	    core_rcu_callback_count == SIZE_MAX) {
		status = domain->callback_sequence == UINT64_MAX ||
				 core_rcu_callback_count == SIZE_MAX ?
				 KB2_CORE_RUNTIME_STATUS_EXHAUSTED :
				 KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
		rcu_unlock();
		callback->magic = 0;
		(void)kobox_provider_arena_release_owned(
			core_arena, callback, 0, binding,
			CORE_ALLOCATION_RCU_CALLBACK);
		binding_object_end(binding);
		return status;
	}
	callback->sequence = ++domain->callback_sequence;
	callback->reader_target = domain->reader_sequence;
	if (domain->callback_tail)
		domain->callback_tail->next = callback;
	else
		domain->callback_head = callback;
	domain->callback_tail = callback;
	core_rcu_callback_count++;
	rcu_unlock();
	rcu_notify(domain);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t rcu_synchronize(
	void *binding_object, kb2_core_rcu_domain_t domain, uint32_t flags)
{
	uint64_t owner;
	uint64_t target;
	kb2_core_status_t status;

	if (flags & ~KB2_CORE_RUNTIME_RCU_SYNC_FLAG_EXPEDITED)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	status = rcu_domain_validate(binding_object, domain, 1);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (core_current_rcu_callback)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	owner = sync_current_thread();
	rcu_lock();
	target = domain->reader_sequence;
	if (rcu_owner_reader_covered_locked(domain, owner, target))
		status = KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	rcu_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	rcu_wait_readers(domain, target);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t rcu_barrier(
	void *binding_object, kb2_core_rcu_domain_t domain)
{
	struct core_rcu_callback *callback;
	uint64_t owner;
	uint64_t reader_target = 0;
	uint64_t target;
	kb2_core_status_t status = rcu_domain_validate(
		binding_object, domain, 1);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (core_current_rcu_callback)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	owner = sync_current_thread();
	rcu_lock();
	target = domain->callback_sequence;
	if (domain->running_callback_sequence &&
	    domain->running_callback_sequence <= target)
		reader_target = domain->running_reader_target;
	for (callback = domain->callback_head; callback &&
						callback->sequence <= target;
	     callback = callback->next)
		reader_target = callback->reader_target;
	if (rcu_owner_reader_covered_locked(domain, owner, reader_target))
		status = KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	rcu_unlock();
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	rcu_wait_callbacks(domain, target);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return KB2_CORE_RUNTIME_STATUS_OK;
}

static kb2_core_status_t rcu_quiescent_state(
	void *binding_object, kb2_core_rcu_domain_t domain)
{
	uint64_t owner;
	kb2_core_status_t status = rcu_domain_validate(
		binding_object, domain, 0);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (domain->domain_class != KB2_CORE_RUNTIME_RCU_DOMAIN_CLASSIC)
		return KB2_CORE_RUNTIME_STATUS_INVALID_ARGUMENT;
	owner = sync_current_thread();
	rcu_lock();
	if (rcu_owner_reader_covered_locked(
		    domain, owner, domain->reader_sequence))
		status = KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	rcu_unlock();
	if (status == KB2_CORE_RUNTIME_STATUS_OK) {
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		rcu_notify(domain);
	}
	return status;
}

static kb2_core_status_t rcu_domain_destroy(
	void *binding_object, kb2_core_rcu_domain_t domain)
{
	struct core_binding *binding = binding_object;
	struct kb2_core_rcu_domain **link;
	uint64_t callback_target;
	uint64_t reader_target;
	uint64_t owner;
	kb2_core_status_t status = rcu_domain_validate(binding, domain, 1);

	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return status;
	if (domain->default_domain)
		return KB2_CORE_RUNTIME_STATUS_INVALID_STATE;
	if (core_current_rcu_callback)
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	owner = sync_current_thread();
	rcu_lock();
	reader_target = domain->reader_sequence;
	callback_target = domain->callback_sequence;
	if (rcu_owner_reader_covered_locked(domain, owner, reader_target)) {
		rcu_unlock();
		return KB2_CORE_RUNTIME_STATUS_DEADLOCK;
	}
	domain->closing = 1;
	rcu_unlock();
	rcu_notify(domain);
	rcu_wait_readers(domain, reader_target);
	rcu_wait_callbacks(domain, callback_target);
	rcu_lock();
	for (link = &core_rcu_domains; *link && *link != domain;
	     link = &(*link)->next)
		;
	if (!*link || domain->readers || domain->callback_head ||
	    domain->running_callback_sequence || core_rcu_domain_count <= 1) {
		rcu_unlock();
		return KB2_CORE_RUNTIME_STATUS_CORRUPT;
	}
	*link = domain->next;
	domain->next = NULL;
	domain->magic = 0;
	core_rcu_domain_count--;
	rcu_unlock();
	binding_object_end(binding);
	return kobox_provider_arena_release_owned(
		       core_arena, domain, 0, binding,
		       CORE_ALLOCATION_RCU_DOMAIN) == KOBOX_PROVIDER_ARENA_OK ?
		       KB2_CORE_RUNTIME_STATUS_OK :
		       KB2_CORE_RUNTIME_STATUS_CORRUPT;
}

static int rcu_lifecycle_init(const struct kobox_module_context *context)
{
	uint64_t native_mask[CORE_NATIVE_CPU_WORDS];
	uint32_t expected;

	(void)context;
	if (__atomic_load_n(&core_rcu_active, __ATOMIC_ACQUIRE) ||
	    core_rcu_domains || core_rcu_domain_count ||
	    core_rcu_token_count || core_rcu_callback_count ||
	    core_rcu_worker_started || core_rcu_descriptor >= 0)
		return -1;
	bytes_zero(&core_rcu_default_domain,
		   sizeof(core_rcu_default_domain));
	core_rcu_default_domain.magic = CORE_RCU_DOMAIN_MAGIC;
	core_rcu_default_domain.generation = context->generation;
	core_rcu_default_domain.domain_class =
		KB2_CORE_RUNTIME_RCU_DOMAIN_CLASSIC;
	core_rcu_default_domain.default_domain = 1;
	core_rcu_domains = &core_rcu_default_domain;
	core_rcu_domain_count = 1;
	core_rcu_descriptor = thread_wake_descriptor_create();
	if (core_rcu_descriptor < 0)
		goto fail;
	bytes_zero(&core_rcu_worker_thread,
		   sizeof(core_rcu_worker_thread));
	core_rcu_worker_thread.wake_descriptor = -1;
	core_rcu_worker_thread.wake_descriptor =
		thread_wake_descriptor_create();
	if (core_rcu_worker_thread.wake_descriptor < 0)
		goto fail;
	core_rcu_worker_thread.magic = CORE_THREAD_MAGIC;
	core_rcu_worker_thread.identity = thread_allocate_identity();
	core_rcu_worker_thread.generation = context->generation;
	core_rcu_worker_thread.state = CORE_THREAD_RUNNING;
	core_rcu_worker_thread.borrowed = 1;
	core_rcu_worker_thread.priority =
		KB2_CORE_RUNTIME_THREAD_PRIORITY_DEFAULT;
	core_rcu_stop = 0;
	core_rcu_worker_startup_word = 0;
	__atomic_store_n(&core_rcu_active, 1, __ATOMIC_RELEASE);
	if (pthread_create(&core_rcu_worker, NULL, rcu_worker_entry, NULL))
		goto fail_active;
	core_rcu_worker_started = 1;
	while (!__atomic_load_n(&core_rcu_worker_startup_word,
				 __ATOMIC_ACQUIRE)) {
		expected = __atomic_load_n(&core_rcu_worker_startup_word,
					   __ATOMIC_RELAXED);
		(void)sync_futex_wait(&core_rcu_worker_startup_word, expected, 0);
	}
	work_native_mask(KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY, native_mask);
	if (pthread_setaffinity_np(core_rcu_worker, sizeof(native_mask),
				   (const cpu_set_t *)native_mask))
		goto fail_worker;
	return 0;

fail_worker:
	__atomic_store_n(&core_rcu_stop, 1, __ATOMIC_RELEASE);
	rcu_notify(&core_rcu_default_domain);
	(void)pthread_join(core_rcu_worker, NULL);
	core_rcu_worker_started = 0;
fail_active:
	__atomic_store_n(&core_rcu_active, 0, __ATOMIC_RELEASE);
	thread_wake_descriptor_close(&core_rcu_worker_thread);
fail:
	if (core_rcu_descriptor >= 0) {
		(void)core_linux_syscall6(__NR_close, core_rcu_descriptor,
					  0, 0, 0, 0, 0);
		core_rcu_descriptor = -1;
	}
	bytes_zero(&core_rcu_default_domain,
		   sizeof(core_rcu_default_domain));
	core_rcu_domains = NULL;
	core_rcu_domain_count = 0;
	return -1;
}

static int rcu_lifecycle_quiesce(const struct kobox_module_context *context)
{
	struct kb2_core_rcu_domain *domain;

	(void)context;
	if (!__atomic_exchange_n(&core_rcu_active, 0, __ATOMIC_ACQ_REL))
		return -1;
	rcu_lock();
	for (domain = core_rcu_domains; domain; domain = domain->next)
		domain->closing = 1;
	rcu_unlock();
	for (domain = core_rcu_domains; domain; domain = domain->next) {
		uint64_t callback_target;
		uint64_t reader_target;

		rcu_lock();
		reader_target = domain->reader_sequence;
		callback_target = domain->callback_sequence;
		rcu_unlock();
		rcu_notify(domain);
		rcu_wait_readers(domain, reader_target);
		rcu_wait_callbacks(domain, callback_target);
	}
	__atomic_store_n(&core_rcu_stop, 1, __ATOMIC_RELEASE);
	rcu_notify(&core_rcu_default_domain);
	if (core_rcu_worker_started &&
	    pthread_join(core_rcu_worker, NULL))
		return -1;
	core_rcu_worker_started = 0;
	return 0;
}

static int rcu_lifecycle_cleanup(const struct kobox_module_context *context)
{
	int empty;

	(void)context;
	rcu_lock();
	empty = !__atomic_load_n(&core_rcu_active, __ATOMIC_ACQUIRE) &&
		!core_rcu_worker_started &&
		core_rcu_domains == &core_rcu_default_domain &&
		!core_rcu_default_domain.next &&
		!core_rcu_default_domain.readers &&
		!core_rcu_default_domain.callback_head &&
		!core_rcu_default_domain.running_callback_sequence &&
		core_rcu_domain_count == 1 && !core_rcu_token_count &&
		!core_rcu_callback_count;
	rcu_unlock();
	if (!empty)
		return -1;
	thread_wake_descriptor_close(&core_rcu_worker_thread);
	if (core_rcu_descriptor >= 0 &&
	    core_linux_syscall6(__NR_close, core_rcu_descriptor,
				0, 0, 0, 0, 0) < 0)
		return -1;
	core_rcu_descriptor = -1;
	core_rcu_default_domain.magic = 0;
	bytes_zero(&core_rcu_default_domain,
		   sizeof(core_rcu_default_domain));
	bytes_zero(&core_rcu_worker_thread,
		   sizeof(core_rcu_worker_thread));
	core_rcu_domains = NULL;
	core_rcu_domain_count = 0;
	core_rcu_stop = 0;
	core_rcu_worker_startup_word = 0;
	return 0;
}

static const struct kb2_core_memory_operations core_memory_operations = {
	.header = {
		.size = sizeof(core_memory_operations),
		.identity = KB2_CORE_RUNTIME_ABI_IDENTITY_BYTES,
		.interface_id = KB2_CORE_RUNTIME_INTERFACE_MEMORY,
		.schema_digest = KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES,
	},
	.page_allocate = memory_page_allocate,
	.page_release = memory_page_release,
	.allocate = memory_allocate,
	.reallocate = memory_reallocate,
	.release = memory_release,
	.usable_size = memory_usable_size,
	.cache_create = memory_cache_create,
	.cache_destroy = memory_cache_destroy,
	.cache_allocate = memory_cache_allocate,
	.cache_release = memory_cache_release,
	.statistics = memory_statistics,
};

static const struct kb2_core_cpu_operations core_cpu_operations = {
	.header = {
		.size = sizeof(core_cpu_operations),
		.identity = KB2_CORE_RUNTIME_ABI_IDENTITY_BYTES,
		.interface_id = KB2_CORE_RUNTIME_INTERFACE_CPU,
		.schema_digest = KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES,
	},
	.possible_count = cpu_topology_count,
	.online_count = cpu_topology_count,
	.current = cpu_current,
	.is_online = cpu_is_online,
	.context_class = cpu_context_class,
	.preempt_disable = cpu_preempt_disable,
	.preempt_enable = cpu_preempt_enable,
	.preempt_count = cpu_preempt_count,
	.migrate_disable = cpu_migrate_disable,
	.migrate_enable = cpu_migrate_enable,
	.migrate_count = cpu_migrate_count,
	.local_irq_disable = cpu_local_irq_disable,
	.local_irq_enable = cpu_local_irq_enable,
	.local_irq_save = cpu_local_irq_save,
	.local_irq_restore = cpu_local_irq_restore,
	.bottom_half_disable = cpu_bottom_half_disable,
	.bottom_half_enable = cpu_bottom_half_enable,
	.percpu_allocate = cpu_percpu_allocate,
	.percpu_release = cpu_percpu_release,
	.percpu_address = cpu_percpu_address,
};

static const struct kb2_core_sync_operations core_sync_operations = {
	.header = {
		.size = sizeof(core_sync_operations),
		.identity = KB2_CORE_RUNTIME_ABI_IDENTITY_BYTES,
		.interface_id = KB2_CORE_RUNTIME_INTERFACE_SYNC,
		.schema_digest = KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES,
	},
	.spin_create = sync_spin_create,
	.spin_destroy = sync_spin_destroy,
	.spin_lock = sync_spin_lock,
	.spin_try_lock = sync_spin_try_lock,
	.spin_unlock = sync_spin_unlock,
	.mutex_create = sync_mutex_create,
	.mutex_destroy = sync_mutex_destroy,
	.mutex_lock = sync_mutex_lock,
	.mutex_try_lock = sync_mutex_try_lock,
	.mutex_lock_until = sync_mutex_lock_until,
	.mutex_unlock = sync_mutex_unlock,
	.rwlock_create = sync_rwlock_create,
	.rwlock_destroy = sync_rwlock_destroy,
	.rwlock_read_lock = sync_rwlock_read_lock,
	.rwlock_read_try_lock = sync_rwlock_read_try_lock,
	.rwlock_read_lock_until = sync_rwlock_read_lock_until,
	.rwlock_read_unlock = sync_rwlock_read_unlock,
	.rwlock_write_lock = sync_rwlock_write_lock,
	.rwlock_write_try_lock = sync_rwlock_write_try_lock,
	.rwlock_write_lock_until = sync_rwlock_write_lock_until,
	.rwlock_write_unlock = sync_rwlock_write_unlock,
	.semaphore_create = sync_semaphore_create,
	.semaphore_destroy = sync_semaphore_destroy,
	.semaphore_down = sync_semaphore_down,
	.semaphore_try_down = sync_semaphore_try_down,
	.semaphore_down_until = sync_semaphore_down_until,
	.semaphore_up = sync_semaphore_up,
	.event_create = sync_event_create,
	.event_destroy = sync_event_destroy,
	.event_wait = sync_event_wait,
	.event_try_wait = sync_event_try_wait,
	.event_wait_until = sync_event_wait_until,
	.event_signal = sync_event_signal,
	.event_reset = sync_event_reset,
	.completion_create = sync_completion_create,
	.completion_destroy = sync_completion_destroy,
	.completion_wait = sync_completion_wait,
	.completion_try_wait = sync_completion_try_wait,
	.completion_wait_until = sync_completion_wait_until,
	.completion_complete = sync_completion_complete,
	.completion_complete_all = sync_completion_complete_all,
	.completion_reinit = sync_completion_reinit,
};

static const struct kb2_core_thread_operations core_thread_operations = {
	.header = {
		.size = sizeof(core_thread_operations),
		.identity = KB2_CORE_RUNTIME_ABI_IDENTITY_BYTES,
		.interface_id = KB2_CORE_RUNTIME_INTERFACE_THREAD,
		.schema_digest = KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES,
	},
	.current = thread_current,
	.create = thread_create,
	.join = thread_join,
	.detach = thread_detach,
	.request_stop = thread_request_stop,
	.stop_requested = thread_stop_requested,
	.interrupt = thread_interrupt,
	.is_interrupted = thread_is_interrupted,
	.clear_interrupt = thread_clear_interrupt,
	.park = thread_park,
	.unpark = thread_unpark,
	.wake = thread_wake,
	.yield = thread_yield,
	.set_name = thread_set_name,
	.set_affinity = thread_set_affinity,
	.set_priority = thread_set_priority,
};

static const struct kb2_core_time_operations core_time_operations = {
	.header = {
		.size = sizeof(core_time_operations),
		.identity = KB2_CORE_RUNTIME_ABI_IDENTITY_BYTES,
		.interface_id = KB2_CORE_RUNTIME_INTERFACE_TIME,
		.schema_digest = KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES,
	},
	.monotonic_ns = time_monotonic_ns,
	.boottime_ns = time_boottime_ns,
	.realtime_ns = time_realtime_ns,
	.sleep_until = time_sleep_until,
	.busy_wait_until = time_busy_wait_until,
	.timer_create = time_timer_create,
	.timer_destroy = time_timer_destroy,
	.timer_arm = time_timer_arm,
	.timer_cancel = time_timer_cancel,
	.timer_cancel_sync = time_timer_cancel_sync,
	.timer_remaining = time_timer_remaining,
	.timer_is_pending = time_timer_is_pending,
};

static const struct kb2_core_workqueue_operations core_workqueue_operations = {
	.header = {
		.size = sizeof(core_workqueue_operations),
		.identity = KB2_CORE_RUNTIME_ABI_IDENTITY_BYTES,
		.interface_id = KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE,
		.schema_digest = KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES,
	},
	.queue_create = work_queue_create,
	.queue_destroy = work_queue_destroy,
	.work_create = work_create,
	.work_destroy = work_destroy,
	.submit = work_submit,
	.submit_at = work_submit_at,
	.reschedule_at = work_reschedule_at,
	.cancel = work_cancel,
	.cancel_sync = work_cancel_sync,
	.flush_work = work_flush_work,
	.flush_queue = work_flush_queue,
	.is_pending = work_is_pending,
};

static const struct kb2_core_rcu_operations core_rcu_operations = {
	.header = {
		.size = sizeof(core_rcu_operations),
		.identity = KB2_CORE_RUNTIME_ABI_IDENTITY_BYTES,
		.interface_id = KB2_CORE_RUNTIME_INTERFACE_RCU,
		.schema_digest = KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES,
	},
	.default_domain = rcu_default_domain,
	.domain_create = rcu_domain_create,
	.domain_destroy = rcu_domain_destroy,
	.read_lock = rcu_read_lock,
	.read_unlock = rcu_read_unlock,
	.call = rcu_call,
	.synchronize = rcu_synchronize,
	.barrier = rcu_barrier,
	.quiescent_state = rcu_quiescent_state,
};

const struct kb2_core_directory kobox_linux_core_directory = {
	.size = sizeof(kobox_linux_core_directory),
	.identity = KB2_CORE_RUNTIME_ABI_IDENTITY_BYTES,
	.schema_digest = KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES,
	.bind = core_bind,
	.unbind = core_unbind,
};

int kobox_linux_core_init(const struct kobox_module_context *context)
{
	if (!context_valid(context) || core_state != CORE_UNBOUND ||
	    core_bindings || core_retired_bindings ||
	    (core_lifecycle.state == KOBOX_PROVIDER_UNBOUND &&
	     kobox_provider_lifecycle_bind(
		     &core_lifecycle, core_entries,
		     sizeof(core_entries) / sizeof(core_entries[0])) !=
		     KOBOX_PROVIDER_LIFECYCLE_OK))
		return -1;
	if (kobox_provider_lifecycle_init(&core_lifecycle, context) !=
	    KOBOX_PROVIDER_LIFECYCLE_OK)
		return -1;
	core_generation = context->generation;
	core_node_id = context->node_id;
	core_next_binding_id = 1;
	core_state = CORE_ACTIVE;
	return 0;
}

int kobox_linux_core_quiesce(const struct kobox_module_context *context)
{
	if (!context_valid(context) || context->generation != core_generation ||
	    context->node_id != core_node_id ||
	    context->logical_cpu_count != core_cpu_count ||
	    core_state != CORE_ACTIVE)
		return -1;
	if (kobox_provider_lifecycle_quiesce(&core_lifecycle, context) !=
	    KOBOX_PROVIDER_LIFECYCLE_OK)
		return -1;
	core_state = CORE_QUIESCED;
	return 0;
}

int kobox_linux_core_cleanup(const struct kobox_module_context *context)
{
	int quiesce_status = 0;
	int cleanup_status;

	if (!context_valid(context) || context->generation != core_generation ||
	    context->node_id != core_node_id ||
	    context->logical_cpu_count != core_cpu_count)
		return -1;
	if (core_lifecycle.state == KOBOX_PROVIDER_ACTIVE) {
		quiesce_status = kobox_provider_lifecycle_quiesce(
			&core_lifecycle, context);
		if (quiesce_status == KOBOX_PROVIDER_LIFECYCLE_OK)
			core_state = CORE_QUIESCED;
	}
	cleanup_status = kobox_provider_lifecycle_cleanup(&core_lifecycle,
							 context);
	if (quiesce_status != KOBOX_PROVIDER_LIFECYCLE_OK ||
	    cleanup_status != KOBOX_PROVIDER_LIFECYCLE_OK)
		return -1;
	core_generation = 0;
	core_node_id = 0;
	core_next_binding_id = 0;
	core_state = CORE_UNBOUND;
	return 0;
}
