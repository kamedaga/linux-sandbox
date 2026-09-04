// SPDX-License-Identifier: GPL-2.0-only

#include "../runtime/module_context.h"

#include <kobox2/core_runtime.h>

#include <linux/completion.h>
#include <linux/gfp_types.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/stdarg.h>
#include <linux/workqueue.h>

#undef current
#undef local_irq_disable
#undef local_irq_enable
#undef local_irq_restore
#undef local_irq_save

#define KOBOX_LINUX_CACHE_MAGIC 0x6b62326c63616368ULL
#define KOBOX_LINUX_PERCPU_UNIT_SIZE (1024u * 1024u)
#define KOBOX_LINUX_SRCU_READ_MAXIMUM 64u
#if defined(__x86_64__)
#define KOBOX_LINUX_ENTRY __attribute__((force_align_arg_pointer))
#else
#define KOBOX_LINUX_ENTRY
#endif

enum kobox_linux_kernel_state {
	KOBOX_LINUX_KERNEL_CLEAN = 0,
	KOBOX_LINUX_KERNEL_INITIALIZING,
	KOBOX_LINUX_KERNEL_ACTIVE,
};

struct kobox_linux_cache {
	u64 magic;
	size_t object_size;
	size_t alignment;
	void (*constructor)(void *object);
	slab_flags_t flags;
	unsigned long live_objects;
};

struct kobox_linux_rcu_free {
	void *allocation;
};

struct kobox_linux_srcu_domain {
	struct kobox_linux_srcu_domain *next;
	const struct srcu_struct *identity;
	kb2_core_rcu_domain_t domain;
};

struct kobox_linux_srcu_read {
	const struct srcu_struct *identity;
	kb2_core_rcu_domain_t domain;
	kb2_core_rcu_read_token_t token;
};

struct kobox_linux_mutex {
	struct kobox_linux_mutex *next;
	const struct mutex *identity;
	kb2_core_sync_mutex_t mutex;
};

struct kobox_linux_completion {
	struct kobox_linux_completion *next;
	const struct completion *identity;
	kb2_core_sync_completion_t completion;
};

struct workqueue_struct {
	kb2_core_workqueue_queue_t core_queue;
	unsigned int flags;
	u32 destroying;
};

struct kobox_linux_work {
	struct work_struct *linux_work;
	kb2_core_workqueue_work_t core_work;
	struct kobox_linux_work *next;
};

struct kobox_linux_resource {
	struct resource resource;
	struct resource *requested_parent;
	struct kobox_linux_resource *next;
};

struct kobox_linux_percpu_record {
	struct kobox_linux_percpu_record *next;
	size_t offset;
	size_t length;
};

static const struct kobox_module_context *linux_context;
static const struct kb2_core_directory *linux_directory;
static const struct kb2_core_memory_operations *linux_memory;
static const struct kb2_core_cpu_operations *linux_cpu;
static const struct kb2_core_sync_operations *linux_sync;
static const struct kb2_core_thread_operations *linux_thread;
static const struct kb2_core_time_operations *linux_time;
static const struct kb2_core_workqueue_operations *linux_workqueue;
static const struct kb2_core_rcu_operations *linux_rcu;
static struct kb2_core_binding linux_memory_binding;
static struct kb2_core_binding linux_cpu_binding;
static struct kb2_core_binding linux_sync_binding;
static struct kb2_core_binding linux_thread_binding;
static struct kb2_core_binding linux_time_binding;
static struct kb2_core_binding linux_workqueue_binding;
static struct kb2_core_binding linux_rcu_binding;
static kb2_core_rcu_domain_t linux_rcu_domain;
static kb2_core_sync_event_t linux_scheduler_event;
static struct kobox_linux_cache
	linux_kmalloc_caches[NR_KMALLOC_TYPES][KMALLOC_SHIFT_HIGH + 1];
static u32 linux_kernel_state;
static u32 linux_work_lock_word;
static struct kobox_linux_work *linux_works;
static u32 linux_resource_lock_word;
static struct kobox_linux_resource *linux_resources;
static kb2_core_cpu_percpu_allocation_t linux_percpu_allocation;
static struct kobox_linux_percpu_record *linux_percpu_records;
static size_t linux_percpu_dynamic_start;
static u32 linux_percpu_cpu_count;
static u32 linux_percpu_lock_word;
static u32 linux_srcu_lock_word;
static struct kobox_linux_srcu_domain *linux_srcu_domains;
static __thread struct kobox_linux_srcu_read
	linux_srcu_reads[KOBOX_LINUX_SRCU_READ_MAXIMUM];
static u32 linux_mutex_lock_word;
static struct kobox_linux_mutex *linux_mutexes;
static u32 linux_completion_lock_word;
static struct kobox_linux_completion *linux_completions;
static __thread struct work_struct *linux_current_work;

extern char __per_cpu_start[];
extern char __per_cpu_end[];
extern unsigned long __per_cpu_offset[];

struct workqueue_struct *system_wq;
struct workqueue_struct *system_percpu_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_dfl_wq;
struct workqueue_struct *system_freezable_wq;
struct workqueue_struct *system_power_efficient_wq;
struct workqueue_struct *system_freezable_power_efficient_wq;
struct workqueue_struct *system_bh_wq;
struct workqueue_struct *system_bh_highpri_wq;

kmem_buckets kmalloc_caches[NR_KMALLOC_TYPES];

extern int devices_init(void);
extern int buses_init(void);
extern int classes_init(void);
extern void maple_tree_init(void);
extern void radix_tree_init(void);
extern void wait_bit_init(void);
extern void vfs_caches_init_early(void);
extern void vfs_caches_init(void);

static size_t bounded_string_length(const char *string, size_t maximum)
{
	size_t length;

	if (!string)
		return 0;
	for (length = 0; length < maximum && string[length]; length++)
		;
	return length;
}

static void *heap_allocate(size_t size, size_t alignment, gfp_t flags);

void *alloc_large_system_hash(const char *tablename,
			      unsigned long bucket_size,
			      unsigned long entry_count, int scale,
			      int flags, unsigned int *hash_shift_out,
			      unsigned int *hash_mask_out,
			      unsigned long low_limit,
			      unsigned long high_limit)
{
	size_t total_pages = 0;
	size_t free_pages;
	size_t maximum_entries;
	unsigned long allocation_size;
	unsigned int largest_order;
	unsigned int shift;
	void *table = NULL;

	(void)tablename;
	if (!bucket_size || !linux_memory ||
	    linux_memory->statistics(linux_memory_binding.object, &total_pages,
				     &free_pages, &largest_order) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		return NULL;
	(void)free_pages;
	(void)largest_order;
	if (!entry_count) {
		entry_count = total_pages;
		if (scale > PAGE_SHIFT)
			entry_count >>= min_t(int, scale - PAGE_SHIFT,
					      BITS_PER_LONG - 1);
		else
			entry_count <<= min_t(int, PAGE_SHIFT - scale,
					      BITS_PER_LONG - 1);
	}
	entry_count = max(entry_count, low_limit);
	maximum_entries = total_pages > SIZE_MAX / PAGE_SIZE ? SIZE_MAX :
		(total_pages * PAGE_SIZE) / 16;
	maximum_entries /= bucket_size;
	if (high_limit)
		maximum_entries = min_t(size_t, maximum_entries, high_limit);
	maximum_entries = min_t(size_t, maximum_entries, 0x80000000UL);
	if (maximum_entries < low_limit)
		maximum_entries = low_limit;
	entry_count = min_t(unsigned long, entry_count, maximum_entries);
	if (!entry_count)
		entry_count = 1;
	shift = ilog2(roundup_pow_of_two(entry_count));
	for (;;) {
		if (bucket_size > ULONG_MAX >> shift)
			return NULL;
		allocation_size = bucket_size << shift;
		table = heap_allocate(allocation_size, SMP_CACHE_BYTES,
				      (flags & HASH_ZERO) ? __GFP_ZERO : 0);
		if (table || allocation_size <= PAGE_SIZE || !shift)
			break;
		shift--;
	}
	if (!table)
		return NULL;
	if (hash_shift_out)
		*hash_shift_out = shift;
	if (hash_mask_out)
		*hash_mask_out = shift >= 32 ? UINT_MAX : (1U << shift) - 1;
	return table;
}

static void srcu_map_lock(void)
{
	while (__atomic_exchange_n(&linux_srcu_lock_word, 1, __ATOMIC_ACQUIRE))
		cpu_relax();
}

static void srcu_map_unlock(void)
{
	__atomic_store_n(&linux_srcu_lock_word, 0, __ATOMIC_RELEASE);
}

static void mutex_map_lock(void)
{
	while (__atomic_exchange_n(&linux_mutex_lock_word, 1, __ATOMIC_ACQUIRE))
		cpu_relax();
}

static void mutex_map_unlock(void)
{
	__atomic_store_n(&linux_mutex_lock_word, 0, __ATOMIC_RELEASE);
}

static void completion_map_lock(void)
{
	while (__atomic_exchange_n(&linux_completion_lock_word, 1,
				   __ATOMIC_ACQUIRE))
		cpu_relax();
}

static void completion_map_unlock(void)
{
	__atomic_store_n(&linux_completion_lock_word, 0, __ATOMIC_RELEASE);
}

static void work_map_lock(void)
{
	while (__atomic_exchange_n(&linux_work_lock_word, 1, __ATOMIC_ACQUIRE))
		cpu_relax();
}

static void work_map_unlock(void)
{
	__atomic_store_n(&linux_work_lock_word, 0, __ATOMIC_RELEASE);
}

static void percpu_map_lock(void)
{
	while (__atomic_exchange_n(&linux_percpu_lock_word, 1,
				   __ATOMIC_ACQUIRE))
		cpu_relax();
}

static void percpu_map_unlock(void)
{
	__atomic_store_n(&linux_percpu_lock_word, 0, __ATOMIC_RELEASE);
}

static int size_align_up(size_t value, size_t alignment, size_t *result_out)
{
	size_t mask;

	if (!result_out || !alignment || !is_power_of_2(alignment))
		return -1;
	mask = alignment - 1;
	if (value > SIZE_MAX - mask)
		return -1;
	*result_out = (value + mask) & ~mask;
	return 0;
}

static int linux_percpu_init(void)
{
	size_t static_size = (size_t)(__per_cpu_end - __per_cpu_start);
	void *address;
	u32 cpu_count;
	u32 cpu;

	if (!static_size || static_size >= KOBOX_LINUX_PERCPU_UNIT_SIZE ||
	    size_align_up(static_size, SMP_CACHE_BYTES,
			  &linux_percpu_dynamic_start) ||
	    linux_cpu->possible_count(linux_cpu_binding.object, &cpu_count) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    !cpu_count ||
	    linux_cpu->percpu_allocate(
		    linux_cpu_binding.object, KOBOX_LINUX_PERCPU_UNIT_SIZE,
		    PAGE_SIZE, KB2_CORE_RUNTIME_CPU_PERCPU_FLAG_ZERO,
		    &linux_percpu_allocation) != KB2_CORE_RUNTIME_STATUS_OK)
		return -1;
	for (cpu = 0; cpu < cpu_count; cpu++) {
		if (linux_cpu->percpu_address(
			    linux_cpu_binding.object, linux_percpu_allocation, cpu,
			    &address) != KB2_CORE_RUNTIME_STATUS_OK) {
			(void)linux_cpu->percpu_release(
				linux_cpu_binding.object, linux_percpu_allocation);
			linux_percpu_allocation = NULL;
			return -1;
		}
		memcpy(address, __per_cpu_start, static_size);
		__per_cpu_offset[cpu] =
			(unsigned long)((char *)address - __per_cpu_start);
	}
	linux_percpu_cpu_count = cpu_count;
	return 0;
}

unsigned long kobox_provider_current_percpu_offset(void)
{
	u32 cpu;

	if (!linux_cpu || !linux_percpu_allocation ||
	    (linux_cpu->current)(linux_cpu_binding.object, &cpu) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    cpu >= linux_percpu_cpu_count)
		return 0;
	return __per_cpu_offset[cpu];
}

static u32 memory_flags(gfp_t flags)
{
	u32 result = 0;

	if (flags & __GFP_ZERO)
		result |= KB2_CORE_RUNTIME_MEMORY_FLAG_ZERO;
	if (!(flags & __GFP_DIRECT_RECLAIM))
		result |= KB2_CORE_RUNTIME_MEMORY_FLAG_ATOMIC;
	if (flags & __GFP_RECLAIMABLE)
		result |= KB2_CORE_RUNTIME_MEMORY_FLAG_RECLAIMABLE;
	return result;
}

static void *heap_allocate(size_t size, size_t alignment, gfp_t flags)
{
	void *allocation = NULL;

	if (!size)
		return ZERO_SIZE_PTR;
	if (!linux_memory || !alignment || !is_power_of_2(alignment) ||
	    linux_memory->allocate(linux_memory_binding.object, size, alignment,
				   memory_flags(flags), &allocation) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		return NULL;
	return allocation;
}

static kb2_core_rcu_domain_t
linux_srcu_domain(const struct srcu_struct *identity)
{
	struct kobox_linux_srcu_domain *record;
	struct kobox_linux_srcu_domain *candidate;
	kb2_core_rcu_domain_t domain;

	if (!identity || !linux_rcu)
		return NULL;
	srcu_map_lock();
	for (record = linux_srcu_domains; record; record = record->next) {
		if (record->identity == identity) {
			domain = record->domain;
			srcu_map_unlock();
			return domain;
		}
	}
	srcu_map_unlock();
	candidate = heap_allocate(sizeof(*candidate), __alignof__(*candidate),
				  GFP_KERNEL);
	if (!candidate ||
	    linux_rcu->domain_create(linux_rcu_binding.object,
				     KB2_CORE_RUNTIME_RCU_DOMAIN_SRCU,
				     &candidate->domain) !=
		    KB2_CORE_RUNTIME_STATUS_OK) {
		kfree(candidate);
		return NULL;
	}
	candidate->identity = identity;
	srcu_map_lock();
	for (record = linux_srcu_domains; record; record = record->next) {
		if (record->identity == identity) {
			domain = record->domain;
			srcu_map_unlock();
			(void)linux_rcu->domain_destroy(linux_rcu_binding.object,
						candidate->domain);
			kfree(candidate);
			return domain;
		}
	}
	candidate->next = linux_srcu_domains;
	linux_srcu_domains = candidate;
	domain = candidate->domain;
	srcu_map_unlock();
	return domain;
}

int __srcu_read_lock(struct srcu_struct *identity)
{
	kb2_core_rcu_domain_t domain = linux_srcu_domain(identity);
	unsigned int index;

	if (!domain)
		BUG();
	for (index = 0; index < ARRAY_SIZE(linux_srcu_reads); index++) {
		if (linux_srcu_reads[index].identity)
			continue;
		if ((linux_rcu->read_lock)(linux_rcu_binding.object, domain,
					   &linux_srcu_reads[index].token) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
			BUG();
		linux_srcu_reads[index].domain = domain;
		linux_srcu_reads[index].identity = identity;
		return (int)index;
	}
	BUG();
}

void __srcu_read_unlock(struct srcu_struct *identity, int index)
{
	struct kobox_linux_srcu_read *read;

	if (index < 0 || index >= ARRAY_SIZE(linux_srcu_reads))
		BUG();
	read = &linux_srcu_reads[index];
	if (read->identity != identity ||
	    (linux_rcu->read_unlock)(linux_rcu_binding.object, read->domain,
				     read->token) != KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	*read = (struct kobox_linux_srcu_read){};
}

void synchronize_srcu(struct srcu_struct *identity)
{
	kb2_core_rcu_domain_t domain = linux_srcu_domain(identity);

	if (!domain ||
	    linux_rcu->synchronize(linux_rcu_binding.object, domain, 0) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

static kb2_core_sync_mutex_t linux_mutex(const struct mutex *identity)
{
	struct kobox_linux_mutex *record;
	struct kobox_linux_mutex *candidate;
	kb2_core_sync_mutex_t mutex;

	if (!identity || !linux_sync)
		return NULL;
	mutex_map_lock();
	for (record = linux_mutexes; record; record = record->next) {
		if (record->identity == identity) {
			mutex = record->mutex;
			mutex_map_unlock();
			return mutex;
		}
	}
	mutex_map_unlock();
	candidate = heap_allocate(sizeof(*candidate), __alignof__(*candidate),
				  GFP_KERNEL);
	if (!candidate ||
	    linux_sync->mutex_create(linux_sync_binding.object,
				     &candidate->mutex) !=
		    KB2_CORE_RUNTIME_STATUS_OK) {
		kfree(candidate);
		return NULL;
	}
	candidate->identity = identity;
	mutex_map_lock();
	for (record = linux_mutexes; record; record = record->next) {
		if (record->identity == identity) {
			mutex = record->mutex;
			mutex_map_unlock();
			(void)linux_sync->mutex_destroy(linux_sync_binding.object,
						 candidate->mutex);
			kfree(candidate);
			return mutex;
		}
	}
	candidate->next = linux_mutexes;
	linux_mutexes = candidate;
	mutex = candidate->mutex;
	mutex_map_unlock();
	return mutex;
}

KOBOX_LINUX_ENTRY void __mutex_init(struct mutex *identity, const char *name,
				    struct lock_class_key *key)
{
	(void)name;
	(void)key;
	if (!linux_mutex(identity))
		BUG();
}

KOBOX_LINUX_ENTRY void mutex_lock(struct mutex *identity)
{
	kb2_core_sync_mutex_t mutex = linux_mutex(identity);

	if (!mutex ||
	    (linux_sync->mutex_lock)(linux_sync_binding.object, mutex, 0) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

KOBOX_LINUX_ENTRY int mutex_lock_interruptible(struct mutex *identity)
{
	kb2_core_sync_mutex_t mutex = linux_mutex(identity);
	kb2_core_status_t status;

	if (!mutex)
		return -ENOMEM;
	status = (linux_sync->mutex_lock)(
		linux_sync_binding.object, mutex,
		KB2_CORE_RUNTIME_SYNC_WAIT_FLAG_INTERRUPTIBLE);
	if (status == KB2_CORE_RUNTIME_STATUS_INTERRUPTED)
		return -EINTR;
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	return 0;
}

KOBOX_LINUX_ENTRY int mutex_trylock(struct mutex *identity)
{
	kb2_core_sync_mutex_t mutex = linux_mutex(identity);
	u32 acquired = 0;

	if (!mutex ||
	    (linux_sync->mutex_try_lock)(linux_sync_binding.object, mutex,
					 &acquired) != KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	return acquired;
}

KOBOX_LINUX_ENTRY void mutex_unlock(struct mutex *identity)
{
	kb2_core_sync_mutex_t mutex = linux_mutex(identity);

	if (!mutex ||
	    (linux_sync->mutex_unlock)(linux_sync_binding.object, mutex) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

KOBOX_LINUX_ENTRY bool mutex_is_locked(struct mutex *identity)
{
	if (mutex_trylock(identity)) {
		mutex_unlock(identity);
		return false;
	}
	return true;
}

static kb2_core_sync_completion_t
linux_completion(const struct completion *identity)
{
	struct kobox_linux_completion *record;
	struct kobox_linux_completion *candidate;
	kb2_core_sync_completion_t completion;

	if (!identity || !linux_sync)
		return NULL;
	completion_map_lock();
	for (record = linux_completions; record; record = record->next) {
		if (record->identity == identity) {
			completion = record->completion;
			completion_map_unlock();
			return completion;
		}
	}
	completion_map_unlock();
	candidate = heap_allocate(sizeof(*candidate), __alignof__(*candidate),
				  GFP_KERNEL);
	if (!candidate ||
	    linux_sync->completion_create(linux_sync_binding.object,
					  &candidate->completion) !=
		    KB2_CORE_RUNTIME_STATUS_OK) {
		kfree(candidate);
		return NULL;
	}
	candidate->identity = identity;
	completion_map_lock();
	for (record = linux_completions; record; record = record->next) {
		if (record->identity == identity) {
			completion = record->completion;
			completion_map_unlock();
			(void)linux_sync->completion_destroy(
				linux_sync_binding.object, candidate->completion);
			kfree(candidate);
			return completion;
		}
	}
	candidate->next = linux_completions;
	linux_completions = candidate;
	completion = candidate->completion;
	completion_map_unlock();
	return completion;
}

static void linux_completion_consume(struct completion *identity)
{
	unsigned int done = __atomic_load_n(&identity->done, __ATOMIC_ACQUIRE);

	while (done != UINT_MAX && done &&
	       !__atomic_compare_exchange_n(&identity->done, &done, done - 1,
					    false, __ATOMIC_RELEASE,
					    __ATOMIC_RELAXED))
		;
}

KOBOX_LINUX_ENTRY void wait_for_completion(struct completion *identity)
{
	kb2_core_sync_completion_t completion = linux_completion(identity);

	if (!completion)
		BUG();
	if (!__atomic_load_n(&identity->done, __ATOMIC_ACQUIRE) &&
	    linux_sync->completion_reinit(linux_sync_binding.object,
					  completion) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	if (linux_sync->completion_wait(linux_sync_binding.object, completion,
					0) != KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	linux_completion_consume(identity);
}

KOBOX_LINUX_ENTRY unsigned long
wait_for_completion_timeout(struct completion *identity,
			    unsigned long timeout)
{
	kb2_core_sync_completion_t completion = linux_completion(identity);
	uint64_t now;
	uint64_t duration;
	kb2_core_status_t status;

	if (!completion ||
	    linux_time->monotonic_ns(linux_time_binding.object, &now) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	if (!__atomic_load_n(&identity->done, __ATOMIC_ACQUIRE) &&
	    linux_sync->completion_reinit(linux_sync_binding.object,
					  completion) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	duration = timeout > U64_MAX / (NSEC_PER_SEC / HZ) ? U64_MAX :
		timeout * (NSEC_PER_SEC / HZ);
	status = linux_sync->completion_wait_until(
		linux_sync_binding.object, completion,
		duration > U64_MAX - now ? U64_MAX : now + duration, 0);
	if (status == KB2_CORE_RUNTIME_STATUS_TIMED_OUT)
		return 0;
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	linux_completion_consume(identity);
	return timeout ?: 1;
}

KOBOX_LINUX_ENTRY void complete(struct completion *identity)
{
	kb2_core_sync_completion_t completion = linux_completion(identity);
	unsigned int done;

	if (!completion)
		BUG();
	done = __atomic_load_n(&identity->done, __ATOMIC_RELAXED);
	while (done != UINT_MAX &&
	       !__atomic_compare_exchange_n(&identity->done, &done, done + 1,
					    false, __ATOMIC_RELEASE,
					    __ATOMIC_RELAXED))
		;
	if (linux_sync->completion_complete(linux_sync_binding.object,
					    completion) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

KOBOX_LINUX_ENTRY void complete_all(struct completion *identity)
{
	kb2_core_sync_completion_t completion = linux_completion(identity);

	if (!completion)
		BUG();
	__atomic_store_n(&identity->done, UINT_MAX, __ATOMIC_RELEASE);
	if (linux_sync->completion_complete_all(linux_sync_binding.object,
						completion) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

void kobox_linux_scheduler_notify(void)
{
	if (!linux_scheduler_event ||
	    linux_sync->event_signal(linux_sync_binding.object,
				     linux_scheduler_event) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

KOBOX_LINUX_ENTRY void schedule(void)
{
	if (!linux_scheduler_event ||
	    linux_sync->event_wait(linux_sync_binding.object,
				   linux_scheduler_event, 0) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

KOBOX_LINUX_ENTRY signed long schedule_timeout(signed long timeout)
{
	uint64_t now;
	uint64_t duration;
	kb2_core_status_t status;

	if (timeout <= 0)
		return 0;
	if (!linux_scheduler_event ||
	    linux_time->monotonic_ns(linux_time_binding.object, &now) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	duration = (uint64_t)timeout > U64_MAX / (NSEC_PER_SEC / HZ) ?
		U64_MAX : (uint64_t)timeout * (NSEC_PER_SEC / HZ);
	status = linux_sync->event_wait_until(
		linux_sync_binding.object, linux_scheduler_event,
		duration > U64_MAX - now ? U64_MAX : now + duration, 0);
	if (status == KB2_CORE_RUNTIME_STATUS_TIMED_OUT)
		return 0;
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	return timeout;
}

static struct kobox_linux_cache *cache_from_linux(struct kmem_cache *cache)
{
	struct kobox_linux_cache *candidate = (void *)cache;
	size_t type;
	size_t index;

	if (!candidate)
		return NULL;
	for (type = 0; type < NR_KMALLOC_TYPES; type++) {
		for (index = 0; index <= KMALLOC_SHIFT_HIGH; index++) {
			if (candidate == &linux_kmalloc_caches[type][index])
				return candidate;
		}
	}
	return candidate->magic == KOBOX_LINUX_CACHE_MAGIC ? candidate : NULL;
}

static size_t kmalloc_cache_size(unsigned int index)
{
	if (index == 1)
		return 96;
	if (index == 2)
		return 192;
	return index < sizeof(size_t) * 8 ? (size_t)1 << index : 0;
}

static int heap_bind(const struct kobox_module_context *context)
{
	static const u8 digest[KB2_CORE_RUNTIME_SCHEMA_DIGEST_SIZE] =
		KB2_CORE_RUNTIME_SCHEMA_SHA256_BYTES;
	size_t type;
	size_t index;

	if (!context || !context->core_operations)
		return -EINVAL;
	linux_directory = context->core_operations;
	if (linux_directory->size != sizeof(*linux_directory) ||
	    linux_directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_MEMORY,
				  digest, &linux_memory_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		return -EINVAL;
	linux_memory = linux_memory_binding.operations;
	if (!linux_memory || linux_memory->header.size != sizeof(*linux_memory))
		goto fail;
	if (linux_directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_CPU,
				  digest, &linux_cpu_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto fail;
	linux_cpu = linux_cpu_binding.operations;
	if (!linux_cpu || linux_cpu->header.size != sizeof(*linux_cpu))
		goto fail_cpu;
	if (linux_directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_SYNC,
				  digest, &linux_sync_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto fail_cpu;
	linux_sync = linux_sync_binding.operations;
	if (!linux_sync || linux_sync->header.size != sizeof(*linux_sync))
		goto fail_sync;
	if (linux_sync->event_create(linux_sync_binding.object, 0,
				     &linux_scheduler_event) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto fail_sync;
	if (linux_directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_THREAD,
				  digest, &linux_thread_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto fail_sync;
	linux_thread = linux_thread_binding.operations;
	if (!linux_thread || linux_thread->header.size != sizeof(*linux_thread))
		goto fail_thread;
	if (linux_directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_TIME,
				  digest, &linux_time_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto fail_thread;
	linux_time = linux_time_binding.operations;
	if (!linux_time || linux_time->header.size != sizeof(*linux_time))
		goto fail_time;
	if (linux_directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_WORKQUEUE,
				  digest, &linux_workqueue_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto fail_time;
	linux_workqueue = linux_workqueue_binding.operations;
	if (!linux_workqueue ||
	    linux_workqueue->header.size != sizeof(*linux_workqueue))
		goto fail_workqueue;
	if (linux_directory->bind(context, KB2_CORE_RUNTIME_INTERFACE_RCU,
				  digest, &linux_rcu_binding) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto fail_workqueue;
	linux_rcu = linux_rcu_binding.operations;
	if (!linux_rcu || linux_rcu->header.size != sizeof(*linux_rcu) ||
	    linux_rcu->default_domain(linux_rcu_binding.object,
				      &linux_rcu_domain) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		goto fail_rcu;
	if (linux_percpu_init())
		goto fail_rcu;
	for (type = 0; type < NR_KMALLOC_TYPES; type++) {
		for (index = 0; index <= KMALLOC_SHIFT_HIGH; index++) {
			struct kobox_linux_cache *cache =
				&linux_kmalloc_caches[type][index];

			cache->magic = KOBOX_LINUX_CACHE_MAGIC;
			cache->object_size = kmalloc_cache_size(index);
			cache->alignment = ARCH_KMALLOC_MINALIGN;
			cache->constructor = NULL;
			cache->flags = 0;
			cache->live_objects = 0;
			kmalloc_caches[type][index] = (void *)cache;
		}
	}
	linux_context = context;
	return 0;

fail_rcu:
	(void)linux_directory->unbind(context, &linux_rcu_binding);
	linux_rcu = NULL;
	linux_rcu_domain = NULL;
fail_workqueue:
	(void)linux_directory->unbind(context, &linux_workqueue_binding);
	linux_workqueue = NULL;
fail_time:
	(void)linux_directory->unbind(context, &linux_time_binding);
	linux_time = NULL;
fail_thread:
	(void)linux_directory->unbind(context, &linux_thread_binding);
	linux_thread = NULL;
fail_sync:
	if (linux_scheduler_event) {
		(void)linux_sync->event_destroy(linux_sync_binding.object,
					linux_scheduler_event);
		linux_scheduler_event = NULL;
	}
	(void)linux_directory->unbind(context, &linux_sync_binding);
	linux_sync = NULL;
fail_cpu:
	(void)linux_directory->unbind(context, &linux_cpu_binding);
	linux_cpu = NULL;
fail:
	(void)linux_directory->unbind(context, &linux_memory_binding);
	linux_directory = NULL;
	linux_memory = NULL;
	return -EINVAL;
}

void *__kmalloc_noprof(size_t size, gfp_t flags)
{
	return heap_allocate(size, ARCH_KMALLOC_MINALIGN, flags);
}

void __percpu *pcpu_alloc_noprof(size_t size, size_t alignment, bool reserved,
				 gfp_t flags)
{
	struct kobox_linux_percpu_record *record;
	struct kobox_linux_percpu_record **link;
	size_t candidate;
	u32 cpu;

	(void)reserved;
	if (!size || !linux_percpu_allocation || !alignment ||
	    !is_power_of_2(alignment) || alignment > PAGE_SIZE)
		return NULL;
	record = heap_allocate(sizeof(*record), __alignof__(*record), flags);
	if (!record)
		return NULL;
	percpu_map_lock();
	candidate = linux_percpu_dynamic_start;
	link = &linux_percpu_records;
	while (*link) {
		if (size_align_up(candidate, alignment, &candidate) ||
		    (candidate <= (*link)->offset &&
		     size <= (*link)->offset - candidate))
			break;
		candidate = (*link)->offset + (*link)->length;
		link = &(*link)->next;
	}
	if (size_align_up(candidate, alignment, &candidate) ||
	    candidate > KOBOX_LINUX_PERCPU_UNIT_SIZE ||
	    size > KOBOX_LINUX_PERCPU_UNIT_SIZE - candidate) {
		percpu_map_unlock();
		kfree(record);
		return NULL;
	}
	*record = (struct kobox_linux_percpu_record){
		.next = *link,
		.offset = candidate,
		.length = size,
	};
	*link = record;
	percpu_map_unlock();
	for (cpu = 0; cpu < linux_percpu_cpu_count; cpu++) {
		void *unit;

		if (linux_cpu->percpu_address(
			    linux_cpu_binding.object, linux_percpu_allocation, cpu,
			    &unit) != KB2_CORE_RUNTIME_STATUS_OK)
			BUG();
		memset((char *)unit + candidate, 0, size);
	}
	return (void __percpu *)(__per_cpu_start + candidate);
}

void free_percpu(void __percpu *pointer)
{
	struct kobox_linux_percpu_record *record;
	struct kobox_linux_percpu_record **link;
	size_t offset;

	if (!pointer)
		return;
	if ((char __force *)pointer < __per_cpu_start)
		BUG();
	offset = (size_t)((char __force *)pointer - __per_cpu_start);
	percpu_map_lock();
	for (link = &linux_percpu_records; *link; link = &(*link)->next) {
		if ((*link)->offset != offset)
			continue;
		record = *link;
		*link = record->next;
		percpu_map_unlock();
		kfree(record);
		return;
	}
	percpu_map_unlock();
	BUG();
}

void *__kmalloc_cache_noprof(struct kmem_cache *cache, gfp_t flags,
			     size_t size)
{
	struct kobox_linux_cache *linux_cache = cache_from_linux(cache);

	if (!linux_cache || size > linux_cache->object_size)
		return NULL;
	return heap_allocate(size, linux_cache->alignment, flags);
}

void *__kmalloc_cache_node_noprof(struct kmem_cache *cache, gfp_t flags,
				   int node, size_t size)
{
	(void)node;
	return __kmalloc_cache_noprof(cache, flags, size);
}

void *__kmalloc_node_noprof(DECL_BUCKET_PARAMS(size, buckets), gfp_t flags,
			     int node)
{
	(void)node;
#ifdef CONFIG_SLAB_BUCKETS
	(void)buckets;
#endif
	return __kmalloc_noprof(size, flags);
}

void *__kmalloc_node_track_caller_noprof(DECL_BUCKET_PARAMS(size, buckets),
					 gfp_t flags, int node,
					 unsigned long caller)
{
	(void)node;
	(void)caller;
#ifdef CONFIG_SLAB_BUCKETS
	(void)buckets;
#endif
	return __kmalloc_noprof(size, flags);
}

void *__kvmalloc_node_noprof(DECL_BUCKET_PARAMS(size, buckets),
			     unsigned long alignment, gfp_t flags, int node)
{
	(void)node;
#ifdef CONFIG_SLAB_BUCKETS
	(void)buckets;
#endif
	if (!alignment)
		alignment = ARCH_KMALLOC_MINALIGN;
	return heap_allocate(size, alignment, flags);
}

void *krealloc_node_align_noprof(const void *allocation, size_t size,
				 unsigned long alignment, gfp_t flags, int node)
{
	void *replacement = NULL;

	(void)node;
	if (ZERO_OR_NULL_PTR(allocation))
		return heap_allocate(size, alignment ? alignment : 1, flags);
	if (!size) {
		kfree(allocation);
		return ZERO_SIZE_PTR;
	}
	if (!linux_memory || !alignment || !is_power_of_2(alignment) ||
	    linux_memory->reallocate(linux_memory_binding.object,
				     (void *)allocation, size, alignment,
				     memory_flags(flags), &replacement) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		return NULL;
	return replacement;
}

void kfree(const void *allocation)
{
	if (ZERO_OR_NULL_PTR(allocation))
		return;
	if (!linux_memory ||
	    linux_memory->release(linux_memory_binding.object,
				  (void *)allocation) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

void kvfree(const void *allocation)
{
	kfree(allocation);
}

size_t ksize(const void *allocation)
{
	size_t size = 0;

	if (ZERO_OR_NULL_PTR(allocation))
		return 0;
	if (!linux_memory ||
	    linux_memory->usable_size(linux_memory_binding.object,
				      (void *)allocation, &size) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	return size;
}

static void resource_map_lock(void)
{
	while (__atomic_exchange_n(&linux_resource_lock_word, 1,
				   __ATOMIC_ACQUIRE))
		cpu_relax();
}

static void resource_map_unlock(void)
{
	__atomic_store_n(&linux_resource_lock_word, 0, __ATOMIC_RELEASE);
}

struct resource *__request_region(struct resource *parent,
				  resource_size_t start, resource_size_t count,
				  const char *name, int flags)
{
	struct kobox_linux_resource *record;
	struct resource *insertion_parent;
	resource_size_t end;

	if (!parent || !count || start > RESOURCE_SIZE_MAX - (count - 1) ||
	    WARN_ON_ONCE(flags & IORESOURCE_MUXED))
		return NULL;
	end = start + count - 1;
	record = heap_allocate(sizeof(*record), __alignof__(*record), GFP_KERNEL);
	if (!record)
		return NULL;
	*record = (struct kobox_linux_resource){
		.resource = {
			.name = name,
			.start = start,
			.end = end,
		},
		.requested_parent = parent,
	};

	resource_map_lock();
	insertion_parent = parent;
	for (;;) {
		struct resource *conflict;

		record->resource.flags = resource_type(insertion_parent) |
			resource_ext_type(insertion_parent) | IORESOURCE_BUSY |
			flags;
		record->resource.desc = insertion_parent->desc;
		conflict = request_resource_conflict(insertion_parent,
						     &record->resource);
		if (!conflict)
			break;
		if (conflict == insertion_parent ||
		    (conflict->flags & IORESOURCE_BUSY)) {
			resource_map_unlock();
			kfree(record);
			return NULL;
		}
		insertion_parent = conflict;
	}
	record->next = linux_resources;
	linux_resources = record;
	resource_map_unlock();
	return &record->resource;
}

void __release_region(struct resource *parent, resource_size_t start,
		      resource_size_t count)
{
	struct kobox_linux_resource **link;
	struct kobox_linux_resource *record = NULL;
	resource_size_t end;

	if (!parent || !count || start > RESOURCE_SIZE_MAX - (count - 1))
		return;
	end = start + count - 1;
	resource_map_lock();
	for (link = &linux_resources; *link; link = &(*link)->next) {
		if ((*link)->requested_parent != parent ||
		    (*link)->resource.start != start ||
		    (*link)->resource.end != end)
			continue;
		record = *link;
		if (release_resource(&record->resource))
			BUG();
		*link = record->next;
		break;
	}
	resource_map_unlock();
	if (WARN_ON_ONCE(!record))
		return;
	kfree(record);
}

struct kmem_cache *__kmem_cache_create_args(const char *name,
					    unsigned int object_size,
					    struct kmem_cache_args *args,
					    slab_flags_t flags)
{
	struct kobox_linux_cache *cache;
	size_t name_length = bounded_string_length(
		name, KB2_CORE_RUNTIME_NAME_MAXIMUM_BYTES + 1);
	size_t alignment = args && args->align ? args->align :
						   ARCH_KMALLOC_MINALIGN;
	if (!name || !object_size || !is_power_of_2(alignment) ||
	    name_length > KB2_CORE_RUNTIME_NAME_MAXIMUM_BYTES)
		return NULL;
	cache = heap_allocate(sizeof(*cache), __alignof__(*cache), GFP_KERNEL);
	if (!cache)
		return NULL;
	*cache = (struct kobox_linux_cache){
		.magic = KOBOX_LINUX_CACHE_MAGIC,
		.object_size = object_size,
		.alignment = alignment,
		.constructor = args ? args->ctor : NULL,
		.flags = flags,
	};
	return (void *)cache;
}

void kmem_cache_destroy(struct kmem_cache *cache)
{
	struct kobox_linux_cache *linux_cache = cache_from_linux(cache);

	if (!linux_cache || linux_cache->live_objects)
		BUG();
	linux_cache->magic = 0;
	kfree(linux_cache);
}

void *kmem_cache_alloc_noprof(struct kmem_cache *cache, gfp_t flags)
{
	struct kobox_linux_cache *linux_cache = cache_from_linux(cache);
	void *allocation;

	if (!linux_cache)
		return NULL;
	allocation = heap_allocate(linux_cache->object_size,
				   linux_cache->alignment, flags);
	if (!allocation)
		return NULL;
	__atomic_add_fetch(&linux_cache->live_objects, 1, __ATOMIC_RELAXED);
	if (linux_cache->constructor)
		linux_cache->constructor(allocation);
	return allocation;
}

void *kmem_cache_alloc_node_noprof(struct kmem_cache *cache, gfp_t flags,
				    int node)
{
	(void)node;
	return kmem_cache_alloc_noprof(cache, flags);
}

void *kmem_cache_alloc_lru_noprof(struct kmem_cache *cache,
				   struct list_lru *lru, gfp_t flags)
{
	(void)lru;
	return kmem_cache_alloc_noprof(cache, flags);
}

static void kobox_linux_rcu_free_callback(void *argument)
{
	struct kobox_linux_rcu_free *deferred = argument;

	kfree(deferred->allocation);
	kfree(deferred);
}

void kmem_cache_free(struct kmem_cache *cache, void *allocation)
{
	struct kobox_linux_cache *linux_cache = cache_from_linux(cache);
	struct kobox_linux_rcu_free *deferred;
	unsigned long live;

	if (!linux_cache || ZERO_OR_NULL_PTR(allocation))
		BUG();
	live = __atomic_load_n(&linux_cache->live_objects, __ATOMIC_RELAXED);
	do {
		if (!live)
			BUG();
	} while (!__atomic_compare_exchange_n(&linux_cache->live_objects, &live,
					      live - 1, false,
					      __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED));
	if (linux_cache->flags & SLAB_TYPESAFE_BY_RCU) {
		deferred = heap_allocate(sizeof(*deferred),
					 __alignof__(*deferred), GFP_ATOMIC);
		if (deferred) {
			deferred->allocation = allocation;
			if (linux_rcu->call(linux_rcu_binding.object,
					    linux_rcu_domain,
					    kobox_linux_rcu_free_callback,
					    deferred) ==
			    KB2_CORE_RUNTIME_STATUS_OK)
				return;
			kfree(deferred);
		}
		if (linux_rcu->synchronize(linux_rcu_binding.object,
					   linux_rcu_domain, 0) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
			BUG();
	}
	kfree(allocation);
}

static void linux_work_callback(kb2_core_workqueue_work_t core_work,
				void *argument)
{
	struct kobox_linux_work *record = argument;
	struct work_struct *previous = linux_current_work;

	(void)core_work;
	__atomic_fetch_and(&record->linux_work->data.counter,
			   ~(long)WORK_STRUCT_PENDING, __ATOMIC_RELEASE);
	linux_current_work = record->linux_work;
	record->linux_work->func(record->linux_work);
	linux_current_work = previous;
}

static struct kobox_linux_work *linux_work_record(struct work_struct *work)
{
	struct kobox_linux_work *record;
	struct kobox_linux_work *candidate;

	if (!work || !work->func || !linux_workqueue)
		return NULL;
	work_map_lock();
	for (record = linux_works; record; record = record->next) {
		if (record->linux_work == work) {
			work_map_unlock();
			return record;
		}
	}
	work_map_unlock();
	candidate = heap_allocate(sizeof(*candidate), __alignof__(*candidate),
				  GFP_KERNEL);
	if (!candidate)
		return NULL;
	*candidate = (struct kobox_linux_work){ .linux_work = work };
	if (linux_workqueue->work_create(linux_workqueue_binding.object,
					 linux_work_callback, candidate,
					 &candidate->core_work) !=
	    KB2_CORE_RUNTIME_STATUS_OK) {
		kfree(candidate);
		return NULL;
	}
	work_map_lock();
	for (record = linux_works; record; record = record->next) {
		if (record->linux_work == work) {
			work_map_unlock();
			(void)linux_workqueue->work_destroy(
				linux_workqueue_binding.object, candidate->core_work);
			kfree(candidate);
			return record;
		}
	}
	candidate->next = linux_works;
	linux_works = candidate;
	work_map_unlock();
	return candidate;
}

static u32 core_workqueue_flags(unsigned int flags)
{
	u32 result = 0;

	if (flags & WQ_UNBOUND)
		result |= KB2_CORE_RUNTIME_WORKQUEUE_FLAG_UNBOUND;
	if (flags & WQ_HIGHPRI)
		result |= KB2_CORE_RUNTIME_WORKQUEUE_FLAG_HIGH_PRIORITY;
	if (flags & WQ_MEM_RECLAIM)
		result |= KB2_CORE_RUNTIME_WORKQUEUE_FLAG_MEMORY_RECLAIM;
	if (flags & WQ_FREEZABLE)
		result |= KB2_CORE_RUNTIME_WORKQUEUE_FLAG_FREEZABLE;
	if (flags & __WQ_ORDERED)
		result |= KB2_CORE_RUNTIME_WORKQUEUE_FLAG_ORDERED;
	return result;
}

__attribute__((force_align_arg_pointer)) struct workqueue_struct *
alloc_workqueue_noprof(const char *format, unsigned int flags,
			 int maximum_active, ...)
{
	char name[KB2_CORE_RUNTIME_NAME_MAXIMUM_BYTES + 1];
	struct workqueue_struct *queue;
	unsigned int accepted = WQ_UNBOUND | WQ_FREEZABLE | WQ_MEM_RECLAIM |
		WQ_HIGHPRI | WQ_CPU_INTENSIVE | WQ_POWER_EFFICIENT | WQ_PERCPU |
		__WQ_ORDERED | __WQ_LEGACY;
	va_list arguments;
	int length;

	if (!linux_workqueue || !format || maximum_active < 0 ||
	    flags & ~accepted || flags & WQ_BH ||
	    ((flags & __WQ_ORDERED) &&
	     (!(flags & WQ_UNBOUND) || maximum_active != 1)))
		return NULL;
	va_start(arguments, maximum_active);
	length = vsnprintf(name, sizeof(name), format, arguments);
	va_end(arguments);
	if (length < 0 || length > KB2_CORE_RUNTIME_NAME_MAXIMUM_BYTES)
		return NULL;
	queue = heap_allocate(sizeof(*queue), __alignof__(*queue), GFP_KERNEL);
	if (!queue)
		return NULL;
	*queue = (struct workqueue_struct){ .flags = flags };
	if (linux_workqueue->queue_create(
			linux_workqueue_binding.object, name, length,
			core_workqueue_flags(flags), maximum_active,
			&queue->core_queue) != KB2_CORE_RUNTIME_STATUS_OK) {
		kfree(queue);
		return NULL;
	}
	return queue;
}

void destroy_workqueue(struct workqueue_struct *queue)
{
	if (!queue || __atomic_exchange_n(&queue->destroying, 1,
					  __ATOMIC_ACQ_REL))
		BUG();
	if (linux_workqueue->flush_queue(linux_workqueue_binding.object,
					 queue->core_queue) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    linux_workqueue->queue_destroy(linux_workqueue_binding.object,
					  queue->core_queue) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	queue->core_queue = NULL;
	kfree(queue);
}

static u32 core_work_cpu(int cpu)
{
	return cpu == WORK_CPU_UNBOUND ? KB2_CORE_RUNTIME_WORKQUEUE_CPU_ANY :
				       (u32)cpu;
}

static bool queue_work_common(int cpu, struct workqueue_struct *queue,
			      struct work_struct *work)
{
	struct kobox_linux_work *record;
	u32 queued = 0;
	long old;

	if (!queue || !queue->core_queue || cpu < 0 ||
	    (cpu != WORK_CPU_UNBOUND &&
	     (u32)cpu >= linux_context->logical_cpu_count))
		return false;
	record = linux_work_record(work);
	if (!record)
		return false;
	old = __atomic_fetch_or(&work->data.counter, WORK_STRUCT_PENDING,
				__ATOMIC_ACQ_REL);
	if (old & WORK_STRUCT_PENDING)
		return false;
	if (linux_workqueue->submit(linux_workqueue_binding.object,
				    queue->core_queue, record->core_work,
				    core_work_cpu(cpu), &queued) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    !queued) {
		__atomic_fetch_and(&work->data.counter,
				   ~(long)WORK_STRUCT_PENDING, __ATOMIC_RELEASE);
		return false;
	}
	return true;
}

bool queue_work_on(int cpu, struct workqueue_struct *queue,
		   struct work_struct *work)
{
	return queue_work_common(cpu, queue, work);
}

static int work_deadline(unsigned long delay, u64 *deadline_out)
{
	u64 now;
	u64 delta;

	if (!deadline_out || !linux_time ||
	    delay > U64_MAX / (u64)TICK_NSEC ||
	    linux_time->monotonic_ns(linux_time_binding.object, &now) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		return -EINVAL;
	delta = (u64)delay * (u64)TICK_NSEC;
	if (now > U64_MAX - delta)
		return -EINVAL;
	*deadline_out = now + delta;
	return 0;
}

bool queue_delayed_work_on(int cpu, struct workqueue_struct *queue,
			   struct delayed_work *delayed, unsigned long delay)
{
	struct kobox_linux_work *record;
	u64 deadline;
	u32 queued = 0;
	long old;

	if (!delayed || !queue || !queue->core_queue || cpu < 0 ||
	    (cpu != WORK_CPU_UNBOUND &&
	     (u32)cpu >= linux_context->logical_cpu_count) ||
	    work_deadline(delay, &deadline))
		return false;
	record = linux_work_record(&delayed->work);
	if (!record)
		return false;
	old = __atomic_fetch_or(&delayed->work.data.counter,
				WORK_STRUCT_PENDING, __ATOMIC_ACQ_REL);
	if (old & WORK_STRUCT_PENDING)
		return false;
	delayed->wq = queue;
	delayed->cpu = cpu;
	if (linux_workqueue->submit_at(
			linux_workqueue_binding.object, queue->core_queue,
			record->core_work, core_work_cpu(cpu), deadline,
			&queued) != KB2_CORE_RUNTIME_STATUS_OK ||
	    !queued) {
		__atomic_fetch_and(&delayed->work.data.counter,
				   ~(long)WORK_STRUCT_PENDING, __ATOMIC_RELEASE);
		return false;
	}
	return true;
}

bool mod_delayed_work_on(int cpu, struct workqueue_struct *queue,
			 struct delayed_work *delayed, unsigned long delay)
{
	struct kobox_linux_work *record;
	u64 deadline;
	u32 replaced = 0;
	long old;

	if (!delayed || !queue || !queue->core_queue || cpu < 0 ||
	    (cpu != WORK_CPU_UNBOUND &&
	     (u32)cpu >= linux_context->logical_cpu_count) ||
	    work_deadline(delay, &deadline))
		return false;
	record = linux_work_record(&delayed->work);
	if (!record)
		return false;
	old = __atomic_fetch_or(&delayed->work.data.counter,
				WORK_STRUCT_PENDING, __ATOMIC_ACQ_REL);
	delayed->wq = queue;
	delayed->cpu = cpu;
	if (linux_workqueue->reschedule_at(
			linux_workqueue_binding.object, queue->core_queue,
			record->core_work, core_work_cpu(cpu), deadline,
			&replaced) != KB2_CORE_RUNTIME_STATUS_OK) {
		if (!(old & WORK_STRUCT_PENDING))
			__atomic_fetch_and(&delayed->work.data.counter,
					   ~(long)WORK_STRUCT_PENDING,
					   __ATOMIC_RELEASE);
		return false;
	}
	return replaced != 0;
}

static bool cancel_linux_work(struct work_struct *work, bool synchronous)
{
	struct kobox_linux_work *record;
	u32 canceled = 0;
	kb2_core_status_t status;

	record = linux_work_record(work);
	if (!record)
		return false;
	status = synchronous ?
		linux_workqueue->cancel_sync(linux_workqueue_binding.object,
					      record->core_work, &canceled) :
		linux_workqueue->cancel(linux_workqueue_binding.object,
					 record->core_work, &canceled);
	if (status != KB2_CORE_RUNTIME_STATUS_OK)
		return false;
	if (canceled)
		__atomic_fetch_and(&work->data.counter,
				   ~(long)WORK_STRUCT_PENDING, __ATOMIC_RELEASE);
	return canceled != 0;
}

bool cancel_work(struct work_struct *work)
{
	return cancel_linux_work(work, false);
}

bool cancel_work_sync(struct work_struct *work)
{
	return cancel_linux_work(work, true);
}

bool cancel_delayed_work(struct delayed_work *delayed)
{
	return delayed && cancel_linux_work(&delayed->work, false);
}

bool cancel_delayed_work_sync(struct delayed_work *delayed)
{
	return delayed && cancel_linux_work(&delayed->work, true);
}

bool flush_work(struct work_struct *work)
{
	struct kobox_linux_work *record;
	bool pending;

	record = linux_work_record(work);
	if (!record)
		return false;
	pending = (__atomic_load_n(&work->data.counter, __ATOMIC_ACQUIRE) &
		   WORK_STRUCT_PENDING) != 0;
	if (linux_workqueue->flush_work(linux_workqueue_binding.object,
					record->core_work) !=
	    KB2_CORE_RUNTIME_STATUS_OK)
		return false;
	return pending;
}

bool flush_delayed_work(struct delayed_work *delayed)
{
	return delayed && flush_work(&delayed->work);
}

void __flush_workqueue(struct workqueue_struct *queue)
{
	if (!queue || !queue->core_queue ||
	    linux_workqueue->flush_queue(linux_workqueue_binding.object,
					 queue->core_queue) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

void drain_workqueue(struct workqueue_struct *queue)
{
	__flush_workqueue(queue);
}

struct work_struct *current_work(void)
{
	return linux_current_work;
}

void delayed_work_timer_fn(struct timer_list *timer)
{
	struct delayed_work *delayed;

	if (!timer)
		BUG();
	delayed = container_of(timer, struct delayed_work, timer);
	if (!delayed->wq ||
	    !queue_work_common(delayed->cpu, delayed->wq, &delayed->work))
		BUG();
}

static int system_workqueues_init(void)
{
	system_percpu_wq = alloc_workqueue_noprof("events", WQ_PERCPU, 0);
	system_long_wq = alloc_workqueue_noprof("events_long", WQ_PERCPU, 0);
	system_unbound_wq = alloc_workqueue_noprof(
		"events_unbound", WQ_UNBOUND, 0);
	system_dfl_wq = alloc_workqueue_noprof(
		"events_dfl", WQ_UNBOUND, 0);
	system_freezable_wq = alloc_workqueue_noprof(
		"events_freezable", WQ_FREEZABLE | WQ_PERCPU, 0);
	if (!system_percpu_wq || !system_long_wq ||
	    !system_unbound_wq || !system_dfl_wq || !system_freezable_wq)
		return -ENOMEM;
	system_wq = system_percpu_wq;
	system_power_efficient_wq = system_percpu_wq;
	system_freezable_power_efficient_wq = system_freezable_wq;
	return 0;
}

void _raw_spin_lock(raw_spinlock_t *lock)
{
	arch_spin_lock(&lock->raw_lock);
}

unsigned long kobox_provider_irq_save_flags(void)
{
	u64 state;

	if (!linux_cpu ||
	    linux_cpu->local_irq_save(linux_cpu_binding.object, &state) !=
		    KB2_CORE_RUNTIME_STATUS_OK ||
	    linux_cpu->local_irq_restore(linux_cpu_binding.object, state) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	return (unsigned long)state;
}

void kobox_provider_cpu_idle(void)
{
	if (!linux_thread ||
	    linux_thread->yield(linux_thread_binding.object) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

void kobox_provider_irq_disable(void)
{
	if (!linux_cpu ||
	    linux_cpu->local_irq_disable(linux_cpu_binding.object) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

void kobox_provider_irq_enable(void)
{
	if (!linux_cpu ||
	    linux_cpu->local_irq_enable(linux_cpu_binding.object) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

unsigned long kobox_provider_irq_save(void)
{
	u64 state;

	if (!linux_cpu ||
	    linux_cpu->local_irq_save(linux_cpu_binding.object, &state) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
	return (unsigned long)state;
}

void kobox_provider_irq_restore(unsigned long flags)
{
	if (!linux_cpu ||
	    linux_cpu->local_irq_restore(linux_cpu_binding.object, flags) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

unsigned long _raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
	unsigned long flags = kobox_provider_irq_save();

	arch_spin_lock(&lock->raw_lock);
	return flags;
}

void _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags)
{
	arch_spin_unlock(&lock->raw_lock);
	kobox_provider_irq_restore(flags);
}

void _raw_spin_lock_irq(raw_spinlock_t *lock)
{
	kobox_provider_irq_disable();
	arch_spin_lock(&lock->raw_lock);
}

void __local_bh_disable_ip(unsigned long instruction, unsigned int count)
{
	(void)instruction;
	(void)count;
	if (!linux_cpu ||
	    linux_cpu->bottom_half_disable(linux_cpu_binding.object) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

void __local_bh_enable_ip(unsigned long instruction, unsigned int count)
{
	(void)instruction;
	(void)count;
	if (!linux_cpu ||
	    linux_cpu->bottom_half_enable(linux_cpu_binding.object) !=
		    KB2_CORE_RUNTIME_STATUS_OK)
		BUG();
}

int kobox_linux_core_kernel_init(
	const struct kobox_module_context *context)
{
	int status;

	if (!context || linux_kernel_state != KOBOX_LINUX_KERNEL_CLEAN)
		return -EINVAL;
	linux_kernel_state = KOBOX_LINUX_KERNEL_INITIALIZING;
	if (heap_bind(context)) {
		linux_kernel_state = KOBOX_LINUX_KERNEL_CLEAN;
		return -EINVAL;
	}
	maple_tree_init();
	radix_tree_init();
	wait_bit_init();
	vfs_caches_init_early();
	status = system_workqueues_init();
	if (status)
		return status;
	vfs_caches_init();
	status = devices_init();
	if (status)
		return status;
	status = buses_init();
	if (status)
		return status;
	status = classes_init();
	if (status)
		return status;
	linux_kernel_state = KOBOX_LINUX_KERNEL_ACTIVE;
	return 0;
}

int kobox_linux_core_kernel_active(
	const struct kobox_module_context *context)
{
	return context && context == linux_context &&
	       linux_kernel_state == KOBOX_LINUX_KERNEL_ACTIVE;
}
