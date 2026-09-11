// SPDX-License-Identifier: GPL-2.0-only

#include "vm_lifetime.h"
#include "vm_gate.h"

#include <linux/completion.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/pageblock-flags.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/task_work.h>
#include <linux/vmalloc.h>

#include "../../mm/slab.h"

struct retired_slot {
	struct kmem_cache *cache;
	unsigned long address;
};

struct lazy_drain {
	struct task_struct *task;
	struct completion switched, release;
};

struct kobox_vm_lifetime {
	struct kobox_linux_vm_report *report;
	struct file *file;
	struct inode *inode;
	struct mm_struct *mms[2];
	struct task_struct *tasks[2];
	struct retired_slot mm_slots[2], task_slots[2], file_slot, inode_slot;
	struct lazy_drain drains[2];
	unsigned long pfns[2];
	unsigned int nr_pfns;
	void **probes;
	unsigned long capacity;
};

static struct retired_slot remember_slot(void *object)
{
	return (struct retired_slot) {
		.cache = virt_to_slab(object)->slab_cache,
		.address = (unsigned long)object,
	};
}

static struct kobox_vm_lifetime *begin(struct file *file,
	struct mm_struct *mms[2], struct task_struct *tasks[2],
	struct kobox_linux_vm_report *report)
{
	struct kobox_vm_lifetime *audit;
	unsigned int index, smallest = PAGE_SIZE;

	if (num_online_cpus() != 2)
		return ERR_PTR(-EINVAL);
	audit = kzalloc(sizeof(*audit), GFP_KERNEL);
	if (!audit)
		return ERR_PTR(-ENOMEM);
	audit->report = report;
	audit->file = file;
	if (file) {
		audit->inode = file_inode(file);
		ihold(audit->inode);
		audit->file_slot = remember_slot(file);
		audit->inode_slot = remember_slot(SHMEM_I(audit->inode));
		smallest = min(smallest, audit->file_slot.cache->size);
		smallest = min(smallest, audit->inode_slot.cache->size);
	}
	for (index = 0; index < 2; index++) {
		if (file) {
			struct folio *folio = filemap_get_folio(file->f_mapping, index);

			/* Numeric PFNs only: no observer pins a cache page. */
			if (!IS_ERR(folio)) {
				audit->pfns[audit->nr_pfns++] = folio_pfn(folio);
				folio_put(folio);
			}
		}
		audit->mms[index] = mms[index];
		mmgrab(mms[index]);
		audit->mm_slots[index] = remember_slot(mms[index]);
		audit->tasks[index] = tasks[index];
		get_task_struct(tasks[index]);
		audit->task_slots[index] = remember_slot(tasks[index]);
		smallest = min(smallest, audit->mm_slots[index].cache->size);
		smallest = min(smallest, audit->task_slots[index].cache->size);
	}
	/* Allocate before retirement. Bound the oracle by real RAM and native
	 * cache sizes; pressure can return entire slabs to buddy, not just
	 * populate a nearby partial slab's free objects.
	 */
	audit->capacity = totalram_pages() * DIV_ROUND_UP(PAGE_SIZE, smallest);
	audit->probes = kvcalloc(audit->capacity, sizeof(void *), GFP_KERNEL);
	/* The fixture is quiescent and its two-page file uses order-0 folios.
	 * A failed lookup must not silently turn a required PFN check into zero.
	 */
	if (!audit->probes || (file && file->f_mapping->nrpages != audit->nr_pfns)) {
		int result = audit->probes ? -EINVAL : -ENOMEM;

		for (index = 0; index < 2; index++) {
			mmdrop(audit->mms[index]);
			put_task_struct(audit->tasks[index]);
		}
		if (audit->inode)
			iput(audit->inode);
		kvfree(audit->probes);
		kfree(audit);
		return ERR_PTR(result);
	}
	if (is_vmalloc_addr(audit->probes)) {
		unsigned long start = (unsigned long)audit->probes;
		unsigned long end = start + audit->capacity * sizeof(void *);

		for (; start < end; start += PAGE_SIZE) {
			unsigned int level;

			if (lookup_address(start, &level) && level == PG_LEVEL_2M)
				report->large_alias_pages++;
		}
	}
	return audit;
}

struct kobox_vm_lifetime *kobox_vm_lifetime_begin(struct file *file,
	struct mm_struct *mms[2], struct task_struct *tasks[2],
	struct kobox_linux_vm_report *report)
{
	if (!file)
		return ERR_PTR(-EINVAL);
	return begin(file, mms, tasks, report);
}

struct kobox_vm_lifetime *kobox_vm_lifetime_tasks_begin(
	struct mm_struct *mms[2], struct task_struct *tasks[2],
	struct kobox_linux_vm_report *report)
{
	return begin(NULL, mms, tasks, report);
}

static int switch_lazy_mm(void *argument)
{
	struct lazy_drain *drain = argument;

	/* Native lazy-TLB references are legitimate until a real mm switch. */
	kthread_use_mm(&init_mm);
	kthread_unuse_mm(&init_mm);
	complete(&drain->switched);
	wait_for_completion(&drain->release);
	return 0;
}

static int drain_lazy_mms(struct kobox_vm_lifetime *audit)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		struct lazy_drain *drain = &audit->drains[cpu];

		init_completion(&drain->switched);
		init_completion(&drain->release);
		drain->task = kthread_create(switch_lazy_mm, drain,
					     "vm-lazy/%u", cpu);
		if (IS_ERR(drain->task))
			return PTR_ERR(drain->task);
		kthread_bind(drain->task, cpu);
		wake_up_process(drain->task);
		if (!wait_for_completion_timeout(&drain->switched, 5 * HZ))
			return -ETIMEDOUT;
	}
	return 0;
}

static void drain_deferred(void)
{
	task_work_run();
	flush_delayed_fput();
	rcu_barrier();
}

static int reuse_folio(struct kobox_vm_lifetime *audit, unsigned long pfn);

static int reuse_slot(struct kobox_vm_lifetime *audit,
		      const struct retired_slot *slot)
{
	cpumask_t saved = *current->cpus_ptr;
	unsigned int cpu;
	unsigned long index, count = 0;
	int result = -ENOENT;

	/* Raw native cache allocations are an allocator oracle, not initialized
	 * mm/task/file/inode objects. Never inspect a retired pointer or give an
	 * uninitialized probe to a subsystem API; free it to the same cache.
	 */
	for_each_online_cpu(cpu) {
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu))) {
			result = -EINVAL;
			break;
		}
		while (count < audit->capacity) {
			void *probe = kmem_cache_alloc(slot->cache,
					GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN);

			if (!probe)
				break;
			audit->probes[count++] = probe;
			if ((unsigned long)probe == slot->address) {
				result = 0;
				goto out;
			}
		}
	}
out:
	if (result) {
		struct kobox_linux_vm_report *report = audit->report;
		struct page *page = virt_to_page((void *)slot->address);
		struct slab *slab = virt_to_slab((void *)slot->address);

		report->target_pfn = page_to_pfn(page);
		report->target_flags = READ_ONCE(page->flags.f);
		report->target_type = READ_ONCE(page->page_type);
		report->target_refs = page_ref_count(page);
		report->target_migrate = get_pageblock_migratetype(page);
		report->target_free = zone_page_state(page_zone(page), NR_FREE_PAGES);
		report->target_min = min_wmark_pages(page_zone(page));
		report->search_pages = count;
		if (slab) {
			report->target_slab_size = slab->slab_cache->size;
			report->target_cache_match = slab->slab_cache == slot->cache;
		}
	}
	for (index = 0; index < count; index++)
		kmem_cache_free(slot->cache, audit->probes[index]);
	if (set_cpus_allowed_ptr(current, &saved))
		__builtin_trap();
	/* Pressure may have returned the entire slab to buddy. Reacquiring and
	 * overwriting its original PFN is also direct allocator evidence that
	 * this exact retired slot is no longer owned by the subsystem.
	 */
	if (result == -ENOENT &&
	    !reuse_folio(audit, page_to_pfn(virt_to_page((void *)slot->address)))) {
		audit->report->buddy_slot_reclaims++;
		result = 0;
	}
	return result;
}

static int reuse_folio(struct kobox_vm_lifetime *audit, unsigned long pfn)
{
	cpumask_t saved = *current->cpus_ptr;
	gfp_t classes[] = {
		GFP_HIGHUSER_MOVABLE, GFP_KERNEL, GFP_KERNEL | __GFP_MEMALLOC,
	};
	unsigned int cpu, kind;
	unsigned long index, count = 0;
	int result = -ENOENT;

	/* The allocator oracle must also reach retired pages below normal
	 * watermarks, including backing returned by an entire freed slab.
	 * Reserve access is confined to this post-release test probe; success
	 * still requires allocating and overwriting the exact retired PFN.
	 */
	for (kind = 0; kind < ARRAY_SIZE(classes); kind++) {
		for_each_online_cpu(cpu) {
			for (index = 0; index < count; index++)
				__free_page(audit->probes[index]);
			count = 0;
			drain_all_pages(NULL);
			if (set_cpus_allowed_ptr(current, cpumask_of(cpu))) {
				result = -EINVAL;
				goto out;
			}
			while (count < audit->capacity) {
				struct page *page = alloc_page(classes[kind] |
							      __GFP_NORETRY | __GFP_NOWARN);

				if (!page)
					break;
				audit->probes[count++] = page;
				if (page_to_pfn(page) == pfn) {
					memset(page_address(page), 0xe3, PAGE_SIZE);
					result = 0;
					goto out;
				}
			}
		}
	}
out:
	for (index = 0; index < count; index++)
		__free_page(audit->probes[index]);
	if (set_cpus_allowed_ptr(current, &saved))
		__builtin_trap();
	return result;
}

int kobox_vm_lifetime_finish(struct kobox_vm_lifetime *audit)
{
	struct kobox_linux_vm_report *report = audit->report;
	unsigned int index;
	int result;

#define REQUIRE(expression) do { \
	if (!(expression)) { \
		report->line = __LINE__; \
		result = -EINVAL; \
		goto out; \
	} \
} while (0)
	report->phase = 40;
	result = drain_lazy_mms(audit);
	if (result)
		goto out;
	drain_deferred();
	for (index = 0; index < 2; index++) {
		struct mm_struct *mm = audit->mms[index];
		struct task_struct *task = audit->tasks[index];

		pr_info("MM lifetime: mm[%u] users=%d count=%d task_usage=%d\n",
			index, atomic_read(&mm->mm_users),
			atomic_read(&mm->mm_count), refcount_read(&task->usage));
		REQUIRE(!atomic_read(&mm->mm_users));
		REQUIRE(atomic_read(&mm->mm_count) == 1);
		REQUIRE(!rcu_access_pointer(mm->mm_mt.ma_root));
		REQUIRE(!get_mm_rss(mm) && !mm_pgtables_bytes(mm));
		REQUIRE(refcount_read(&task->usage) == 1);
		REQUIRE(READ_ONCE(task->__state) == TASK_DEAD &&
			!READ_ONCE(task->on_cpu) && !task->mm && !task->active_mm);
		report->mm_drains++;
		report->task_drains++;
		mmdrop(mm);
		audit->mms[index] = NULL;
		put_task_struct(task);
		audit->tasks[index] = NULL;
	}
	/* Native task free also joins/destroys its pthread at the arch boundary. */
	rcu_barrier();
	report->phase = 41;
	for (index = 0; index < 2; index++) {
		REQUIRE(!reuse_slot(audit, &audit->mm_slots[index]));
		report->mm_reclaims++;
		REQUIRE(!reuse_slot(audit, &audit->task_slots[index]));
		report->task_reclaims++;
	}
	if (!audit->file) {
		result = 0;
		goto out;
	}
	report->phase = 42;
	REQUIRE(file_count(audit->file) == 1);
	fput(audit->file);
	audit->file = NULL;
	drain_deferred();
	REQUIRE(atomic_read(&audit->inode->i_count) == 1);
	iput(audit->inode);
	audit->inode = NULL;
	lru_add_drain_all();
	rcu_barrier();
	report->phase = 43;
	for (index = 0; index < audit->nr_pfns; index++) {
		REQUIRE(!reuse_folio(audit, audit->pfns[index]));
		report->folio_reclaims++;
	}
	report->phase = 44;
	REQUIRE(!reuse_slot(audit, &audit->file_slot));
	report->file_reclaims++;
	REQUIRE(!reuse_slot(audit, &audit->inode_slot));
	report->inode_reclaims++;
	result = 0;
out:
	/* Release only references owned by the observer, including on failure. */
	for (index = 0; index < 2; index++) {
		struct lazy_drain *drain = &audit->drains[index];

		if (audit->mms[index])
			mmdrop(audit->mms[index]);
		if (audit->tasks[index])
			put_task_struct(audit->tasks[index]);
		if (!IS_ERR_OR_NULL(drain->task)) {
			complete(&drain->release);
			if (kthread_stop(drain->task))
				__builtin_trap();
		}
	}
	if (audit->file)
		fput(audit->file);
	drain_deferred();
	if (audit->inode)
		iput(audit->inode);
	rcu_barrier();
	kvfree(audit->probes);
	kfree(audit);
	return result;
#undef REQUIRE
}
