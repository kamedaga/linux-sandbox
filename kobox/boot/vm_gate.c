// SPDX-License-Identifier: GPL-2.0-only

#include "vm_gate.h"
#include "vm_lifetime.h"
#include "exception.h"
#include "../mm/port.h"

#include <linux/completion.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/pageblock-flags.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/timekeeping.h>
#include <asm/ptrace.h>

long __x64_sys_mprotect(const struct pt_regs *regs);

#define GATE_WAIT (5 * HZ)
#define REUSE_BATCH 4096
#define HELD_ALLOCATIONS 512

enum action {
	MAP_BACKING, ACCESS, PROTECT_READONLY, UNMAP,
	TRUNCATE, EXIT_TRUNCATE, FLUSH_ALL, WAIT_EXIT, REJECT_MAPPING,
};

struct publication_race;

struct reset_hold {
	atomic_t remaining;
	bool entered;
	bool released;
};

struct vm_worker {
	const struct kobox_linux_vm_test *host;
	struct kobox_linux_vm_report *report;
	struct kobox_vm_space *space;
	void *native;
	struct reset_hold *hold;
	struct publication_race *race;
	struct file *file;
	struct task_struct *task;
	struct completion ready, go, done;
	enum action action;
	unsigned long address, length;
	u64 value, observed, sequence;
	unsigned int write, signal, faults, checks, accesses;
	int result;
	bool finish;
};

struct publication_race {
	struct vm_worker *publisher;
	struct vm_worker *invalidator;
	wait_queue_entry_t irq_probe;
	struct completion mutated;
	struct completion resolved;
	struct completion resume;
	struct completion received;
	atomic_t irqs;
	atomic_t errors;
	u64 publications;
	unsigned int kind;
	bool armed;
	bool irq_queued;
	bool delay_resume;
	bool started;
};

static int publication_irq(wait_queue_entry_t *entry, unsigned int mode,
			   int flags, void *key)
{
	struct publication_race *race = container_of(entry,
				struct publication_race, irq_probe);

	if (raw_smp_processor_id() != 1)
		return 0;
	if (!in_hardirq() || current != race->publisher->task ||
	    race->publisher->space->publications <= race->publications)
		atomic_inc(&race->errors);
	atomic_inc(&race->irqs);
	return 0;
}

static int start_publication_race(struct vm_worker *worker)
{
	struct publication_race *race = worker->race;
	u64 deadline = ktime_get_mono_fast_ns() + 2 * NSEC_PER_SEC;

	race->armed = false;
	race->delay_resume = true;
	race->started = true;
	complete(&race->invalidator->go);
	for (;;) {
		if (race->kind == KOBOX_VM_RACE_IRQ) {
			/* Pair with reset's publication after queuing the VM IRQ. */
			if (smp_load_acquire(&race->irq_queued))
				break;
		} else if (!i_size_read(file_inode(worker->file))) {
			/* Native setattr changes i_size before unmapping our PTE. */
			break;
		}
		if (ktime_get_mono_fast_ns() >= deadline)
			return -ETIMEDOUT;
		cpu_relax();
	}
	if (completion_done(&race->invalidator->done))
		return -EINVAL;
	worker->report->publication_races++;
	return 0;
}

static int test_map(void *context, u64 address, u64 physical, size_t size,
		    unsigned int protection)
{
	struct vm_worker *worker = context;
	int result;

	if (worker->race && worker->race->publisher == worker &&
	    worker->race->armed) {
		result = start_publication_race(worker);
		if (result)
			return result;
	}
	return worker->host->operations->map(worker->native, address, physical,
					     size, protection);
}

static int test_reset(void *context, u64 address, size_t size)
{
	struct vm_worker *worker = context;
	struct reset_hold *hold = worker->hold;
	u64 deadline;
	int result;

	result = worker->host->operations->reset(worker->native, address, size);
	if (!result && worker->race &&
	    worker->race->kind == KOBOX_VM_RACE_IRQ &&
	    worker->race->invalidator == worker &&
	    !worker->race->irq_queued) {
		/* The first binding's reset runs with the registry lock held;
		 * the second binding's publication still holds its translation lock.
		 */
		result = worker->host->notify(1);
		/* Let the publisher proceed only after the IRQ has been queued. */
		smp_store_release(&worker->race->irq_queued, true);
	}
	if (result || !hold || !atomic_dec_and_test(&hold->remaining))
		return result;
	/*
	 * Delay the real machine acknowledgement, not Linux's page references.
	 * This leaf boundary holds Linux locks: the observer runs on the other
	 * CPU with IRQs masked and releases us without guest waitqueue services.
	 */
	deadline = ktime_get_mono_fast_ns() + 2 * NSEC_PER_SEC;
	/* Publish completed host reset before the observer allocates pages. */
	smp_store_release(&hold->entered, true);
	/* Pair with the observer's release after its allocation checks. */
	while (!smp_load_acquire(&hold->released)) {
		if (ktime_get_mono_fast_ns() >= deadline)
			return -ETIMEDOUT;
		cpu_relax();
	}
	return 0;
}

static int test_close(void *context)
{
	struct vm_worker *worker = context;

	return worker->host->operations->close(worker->native);
}

static int test_resume(void *context, u64 sequence)
{
	struct vm_worker *worker = context;
	struct publication_race *race = worker->race;
	int result;

	if (race && race->publisher == worker && race->delay_resume) {
		race->delay_resume = false;
		if (!wait_for_completion_timeout(&race->mutated, GATE_WAIT))
			return -ETIMEDOUT;
		complete(&race->resolved);
		if (!wait_for_completion_timeout(&race->resume, GATE_WAIT))
			return -ETIMEDOUT;
		result = worker->host->operations->resume(worker->native,
							sequence - 1);
		if (result != (race->kind == KOBOX_VM_RACE_EXIT ?
				-ESRCH : -ESTALE))
			return -EINVAL;
		worker->report->stale_resumes++;
	}
	return worker->host->operations->resume(worker->native, sequence);
}

static int test_event(void *context, struct kobox_linux_vm_event *event)
{
	struct vm_worker *worker = context;

	return worker->host->operations->event(worker->native, event);
}

static const struct kobox_linux_vm_host_operations test_operations = {
	.size = sizeof(test_operations),
	.map = test_map,
	.reset = test_reset,
	.close = test_close,
	.resume = test_resume,
	.event = test_event,
};

static int access_memory(struct vm_worker *worker)
{
	struct kobox_vm_space *space = worker->space;
	const struct kobox_linux_vm_host_operations *ops = space->operations;
	struct kobox_linux_vm_event event;
	int result;
	long waited;

	worker->signal = 0;
	result = worker->host->probe(worker->native, worker->address,
		worker->write, worker->value, worker->sequence);
	if (result)
		return result;
	for (;;) {
		waited = wait_event_timeout(space->events,
			(result = ops->event(space->host_space, &event)) != -EAGAIN, GATE_WAIT);
		if (!waited || result || event.error)
			return !waited ? -ETIMEDOUT : result ? result : event.error;
		if (event.sequence != worker->sequence + 1 || current->mm != space->mm ||
		    current->active_mm != space->mm || current != raw_cpu_read(current_task))
			return -EINVAL;
		worker->sequence = event.sequence;
		worker->checks++;
		if (event.kind == KOBOX_VM_EVENT_STOP) {
			worker->observed = event.value;
			worker->accesses++;
			return 0;
		}
		if (event.kind != KOBOX_VM_EVENT_FAULT || event.fault.address != worker->address)
			return -EFAULT;
		worker->faults++;
		if (worker->race &&
		    (worker->race->kind == KOBOX_VM_RACE_LATE_FAULT ||
		     worker->race->kind == KOBOX_VM_RACE_PRESSURE_FAULT) &&
		    worker->race->publisher == worker) {
			/* Hold a real fault packet, not an invented Linux fault. */
			complete(&worker->race->received);
			if (!wait_for_completion_timeout(&worker->race->resume, 20 * HZ))
				return -ETIMEDOUT;
		}
		result = kobox_vm_resolve_fault(space, &event.fault);
		if (result && result != -EAGAIN) {
			if (worker->race &&
			    worker->race->kind == KOBOX_VM_RACE_EXIT_PUBLISH &&
			    worker->race->publisher == worker && result == -ESRCH) {
				/* The failed publication has released all MM locks. */
				if (!wait_for_completion_timeout(&worker->race->mutated,
							 GATE_WAIT))
					return -ETIMEDOUT;
				complete(&worker->race->resolved);
				if (!wait_for_completion_timeout(&worker->race->resume,
							 GATE_WAIT))
					return -ETIMEDOUT;
			}
			return result;
		}
		if (event.fault.signal) {
			worker->signal = event.fault.signal;
			return 0;
		}
		result = ops->resume(space->host_space, worker->sequence);
		if (result)
			return result;
	}
}

static int wait_exit(struct vm_worker *worker)
{
	struct kobox_vm_space *space = worker->space;
	struct kobox_linux_vm_event event;
	int result;
	long waited;

	waited = wait_event_timeout(space->events,
		(result = test_event(worker, &event)) != -EAGAIN, GATE_WAIT);
	if (!waited || result)
		return !waited ? -ETIMEDOUT : result;
	if (event.kind != KOBOX_VM_EVENT_EXIT || event.error ||
	    event.sequence != worker->sequence ||
	    (event.exit_status & 0x7f) != SIGKILL ||
	    current->mm != space->mm || current->active_mm != space->mm ||
	    current != raw_cpu_read(current_task))
		return -EINVAL;
	worker->checks++;
	worker->report->async_exits++;
	return test_event(worker, &event) == -ESRCH ? 0 : -EINVAL;
}

static int worker_main(void *argument)
{
	struct vm_worker *worker = argument;
	struct pt_regs regs = {0};
	unsigned long address;

	kthread_use_mm(worker->space->mm);
	complete(&worker->ready);
	for (;;) {
		wait_for_completion(&worker->go);
		if (worker->finish)
			break;
		switch (worker->action) {
		case MAP_BACKING:
			address = vm_mmap(worker->file, worker->address, worker->length,
				PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, 0);
			worker->result = address == worker->address ? 0 : (long)address;
			break;
		case REJECT_MAPPING:
			/* Shmem does not implement FOP_MMAP_SYNC: real mmap rollback. */
			address = vm_mmap(worker->file, worker->address,
				worker->length, PROT_READ | PROT_WRITE,
				MAP_SHARED_VALIDATE | MAP_FIXED | MAP_SYNC, 0);
			worker->result = (long)address == -EOPNOTSUPP &&
				!current->mm->map_count ? 0 : -EINVAL;
			break;
		case WAIT_EXIT:
			worker->result = wait_exit(worker);
			break;
		case ACCESS:
			worker->result = access_memory(worker);
			break;
		case PROTECT_READONLY:
			regs.di = worker->address;
			regs.si = worker->length;
			regs.dx = PROT_READ;
			worker->result = __x64_sys_mprotect(&regs);
			break;
		case UNMAP:
			worker->result = vm_munmap(worker->address, worker->length);
			break;
		case TRUNCATE:
			worker->result = vfs_truncate(&worker->file->f_path, 0);
			break;
		case EXIT_TRUNCATE:
			worker->result = worker->host->operations->close(
						worker->race->publisher->native);
			if (!worker->result) {
				worker->report->dead_spaces++;
				worker->result = vfs_truncate(&worker->file->f_path, 0);
			}
			break;
		case FLUSH_ALL:
			/* Architecture broadcast, also used by native batched unmap. */
			kobox_vm_flush_all();
			worker->result = 0;
			break;
		}
		if (worker->race && worker->race->invalidator == worker)
			complete_all(&worker->race->mutated);
		complete(&worker->done);
	}
	kthread_unuse_mm(worker->space->mm);
	return 0;
}

static int run(struct vm_worker *worker, enum action action)
{
	worker->action = action;
	reinit_completion(&worker->done);
	complete(&worker->go);
	if (!wait_for_completion_timeout(&worker->done, GATE_WAIT))
		return -ETIMEDOUT;
	worker->report->result = worker->result;
	return worker->result;
}

static int unsolicited_exit(struct vm_worker *workers)
{
	struct vm_worker *dead = &workers[1];
	u64 deadline = ktime_get_mono_fast_ns() + 2 * NSEC_PER_SEC;
	int result;

	if (!dead->host->terminate)
		return -EINVAL;
	dead->report->phase = 30;
	dead->action = WAIT_EXIT;
	reinit_completion(&dead->done);
	complete(&dead->go);
	/* Prove the event wakes a sleeping Linux task, not just a later poll. */
	while (!waitqueue_active(&dead->space->events) ||
	       READ_ONCE(dead->task->__state) != TASK_UNINTERRUPTIBLE ||
	       READ_ONCE(dead->task->on_cpu)) {
		if (ktime_get_mono_fast_ns() >= deadline)
			return -ETIMEDOUT;
		cond_resched();
	}
	result = dead->host->terminate(dead->host->pids[1]);
	if (result)
		return result;
	if (!wait_for_completion_timeout(&dead->done, GATE_WAIT))
		return -ETIMEDOUT;
	if (dead->result)
		return dead->result;
	workers[0].write = 0;
	result = run(&workers[0], ACCESS);
	if (!result && (workers[0].signal ||
		       workers[0].observed != dead->value))
		result = -EINVAL;
	return result;
}

/*
 * No test-owned folio reference survives into truncate. Pages allocated while
 * its second host acknowledgement is held remain allocated through the later
 * search, which must obtain the very same numeric PFN from upstream buddy.
 */
static int reuse_after_invalidation(struct vm_worker *workers)
{
	struct kobox_linux_vm_report *report = workers[0].report;
	struct address_space *mapping = workers[0].file->f_mapping;
	struct reset_hold hold = {};
	cpumask_t saved_mask = *current->cpus_ptr;
	struct page **pages, *reclaimed = NULL;
	struct folio *folio;
	unsigned long pfn, flags;
	unsigned int count = 0, index, cpu;
	gfp_t gfp = mapping_gfp_mask(mapping);
	u64 deadline;
	int result = -EINVAL;

	if (num_online_cpus() != 2)
		return -EOPNOTSUPP;
	pages = kcalloc(HELD_ALLOCATIONS + 2 * REUSE_BATCH, sizeof(*pages),
			GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	lru_add_drain_all();
	folio = filemap_get_folio(mapping, 0);
	if (IS_ERR(folio)) {
		result = PTR_ERR(folio);
		goto free_pages;
	}
	pfn = folio_pfn(folio);
	folio_put(folio);
	result = set_cpus_allowed_ptr(current, cpumask_of(0));
	if (result)
		goto free_pages;
	report->phase = 10;
	atomic_set(&hold.remaining, 2);
	workers[0].hold = &hold;
	workers[1].hold = &hold;
	workers[1].action = TRUNCATE;
	reinit_completion(&workers[1].done);
	/* No IRQ may wait on the registry lock held across the delayed reset. */
	local_irq_save(flags);
	complete(&workers[1].go);
	deadline = ktime_get_mono_fast_ns() + NSEC_PER_SEC;
	/* Pair with reset's announcement, without borrowing a folio reference. */
	while (!smp_load_acquire(&hold.entered)) {
		if (ktime_get_mono_fast_ns() >= deadline) {
			result = -ETIMEDOUT;
			goto release_ack;
		}
		cpu_relax();
	}
	for (index = 0; index < HELD_ALLOCATIONS; index++) {
		struct page *page;

		page = alloc_page((gfp & ~__GFP_RECLAIM) | __GFP_NOWARN);
		if (!page) {
			result = -ENOMEM;
			goto release_ack;
		}
		pages[count++] = page;
		/*
		 * PFN metadata is permanent in this fixed-RAM boot. Observe its
		 * upstream refcount without acquiring a test reference: a free
		 * page stranded on the other CPU's PCP list must also fail.
		 */
		if (page_ref_count(pfn_to_page(pfn)) <= 0 ||
		    page_to_pfn(page) == pfn ||
		    completion_done(&workers[1].done)) {
			result = -EINVAL;
			goto release_ack;
		}
		report->held_allocations++;
	}
release_ack:
	/* Complete every observation before allowing Linux's flush to return. */
	smp_store_release(&hold.released, true);
	local_irq_restore(flags);
	if (!wait_for_completion_timeout(&workers[1].done, GATE_WAIT))
		__builtin_trap();
	workers[0].hold = NULL;
	workers[1].hold = NULL;
	if (!result)
		result = workers[1].result;
	if (result)
		goto restore_affinity;
	report->phase = 11;
	if (mapping->nrpages || i_size_read(file_inode(workers[0].file))) {
		result = -EINVAL;
		goto restore_affinity;
	}
	lru_add_drain_all();
	for_each_online_cpu(cpu) {
		result = set_cpus_allowed_ptr(current, cpumask_of(cpu));
		if (result)
			goto restore_affinity;
		for (index = 0; index < REUSE_BATCH && !reclaimed; index++) {
			struct page *page = alloc_page(gfp);

			if (!page) {
				result = -ENOMEM;
				goto restore_affinity;
			}
			pages[count++] = page;
			if (page_to_pfn(page) == pfn)
				reclaimed = page;
		}
	}
	if (!reclaimed) {
		result = -ENOENT;
		goto restore_affinity;
	}
	report->reclaimed_pages++;
	memset(page_address(reclaimed), 0x9d, PAGE_SIZE);
	report->phase = 12;
	for (index = 0; index < 2; index++) {
		unsigned int before = workers[index].faults;

		workers[index].write = 0;
		result = run(&workers[index], ACCESS);
		if (result)
			goto restore_affinity;
		if (workers[index].signal != SIGBUS ||
		    workers[index].faults != before + 1 ||
		    memchr_inv(page_address(reclaimed), 0x9d, PAGE_SIZE)) {
			result = -EINVAL;
			goto restore_affinity;
		}
		report->revoked_aliases++;
	}
	report->phase = 13;
restore_affinity:
	if (set_cpus_allowed_ptr(current, &saved_mask))
		__builtin_trap();
free_pages:
	for (index = 0; index < count; index++)
		__free_page(pages[index]);
	kfree(pages);
	report->result = result;
	return result;
}

static struct page *reacquire_pfn(unsigned long pfn, gfp_t gfp,
				struct page **pages, unsigned int capacity,
				struct kobox_linux_vm_report *report)
{
	cpumask_t saved_mask = *current->cpus_ptr;
	struct page *found = ERR_PTR(-ENOENT);
	gfp_t classes[] = { gfp, GFP_KERNEL, GFP_KERNEL | __GFP_MEMALLOC };
	unsigned int kind, cpu, index, count = 0;

	/* Scratch predates retirement, so it cannot consume the target PFN.
	 * Memory pressure can reorder all of buddy, not just one small batch.
	 * The final test-only pass may use allocator reserves: an arbitrary
	 * retired PFN can remain below the normal watermark even with no live
	 * references. Require a real alloc_page of that PFN, then return all
	 * scratch pages before resuming the delayed access. This does not
	 * change the pressure workload's GFP flags or any runtime allocator.
	 */
	lru_add_drain_all();
	for (kind = 0; kind < ARRAY_SIZE(classes); kind++) {
		for_each_online_cpu(cpu) {
			for (index = 0; index < count; index++)
				__free_page(pages[index]);
			count = 0;
			/* Do not let another CPU's free PCP pages sit below the
			 * watermark while this search holds all ordinary RAM.
			 */
			drain_all_pages(NULL);
			if (set_cpus_allowed_ptr(current, cpumask_of(cpu))) {
				found = ERR_PTR(-EINVAL);
				goto out;
			}
			while (count < capacity) {
				struct page *page = alloc_page(classes[kind] |
							      __GFP_NORETRY | __GFP_NOWARN);

				if (!page)
					break;
				if (page_to_pfn(page) == pfn) {
					found = page;
					goto out;
				}
				pages[count++] = page;
			}
		}
	}
out:
	if (IS_ERR(found)) {
		struct page *page = pfn_to_page(pfn);

		report->target_pfn = pfn;
		report->target_flags = READ_ONCE(page->flags.f);
		report->target_type = READ_ONCE(page->page_type);
		report->target_refs = page_ref_count(page);
		report->target_migrate = get_pageblock_migratetype(page);
		report->target_free = zone_page_state(page_zone(page), NR_FREE_PAGES);
		report->target_min = min_wmark_pages(page_zone(page));
		report->search_pages = count;
	}
	if (set_cpus_allowed_ptr(current, &saved_mask))
		__builtin_trap();
	for (index = 0; index < count; index++)
		__free_page(pages[index]);
	return found;
}

static int truncate_under_pressure(void *argument)
{
	struct vm_worker *workers = argument;
	struct inode *inode = file_inode(workers[0].file);
	long refs = file_count(workers[0].file);
	int result;

	/* A single instantiated VM_NORESERVE page accounts exactly one page.
	 * Keep both VMA/file references: truncate invalidates, not close.
	 */
	if (inode->i_mapping->nrpages != 1 || SHMEM_I(inode)->alloced != 1 ||
	    SHMEM_I(inode)->swapped || inode->i_blocks != PAGE_SIZE / 512)
		return -EINVAL;
	result = run(&workers[0], TRUNCATE);
	if (result)
		return result;
	if (inode->i_mapping->nrpages || SHMEM_I(inode)->alloced ||
	    SHMEM_I(inode)->swapped || inode->i_blocks || i_size_read(inode) ||
	    file_count(workers[0].file) != refs)
		return -EINVAL;
	return 0;
}

static int delayed_fault_vs_truncate(struct vm_worker *workers)
{
	struct vm_worker *publisher = &workers[1];
	struct kobox_linux_vm_report *report = publisher->report;
	struct address_space *mapping = workers[0].file->f_mapping;
	struct publication_race race = {
		.publisher = publisher, .kind = publisher->host->race_case,
	};
	struct folio *folio;
	struct page *reclaimed = NULL;
	struct page **probes;
	unsigned int capacity = totalram_pages();
	unsigned long pfn;
	u64 publications;
	unsigned int faults;
	int result;

	/* Remove the translation so the client must produce a new fault. */
	result = run(publisher, UNMAP) ?: run(publisher, MAP_BACKING);
	if (result)
		return result;
	folio = filemap_get_folio(mapping, 0);
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	pfn = folio_pfn(folio);
	folio_put(folio);
	probes = kvcalloc(capacity, sizeof(*probes), GFP_KERNEL);
	if (!probes)
		return -ENOMEM;
	init_completion(&race.received);
	init_completion(&race.resume);
	publisher->race = &race;
	publisher->action = ACCESS;
	publisher->write = 1;
	publisher->value = 0xabadcafe;
	faults = publisher->faults;
	publications = publisher->space->publications;
	reinit_completion(&publisher->done);
	complete(&publisher->go);
	report->phase = 50;
	if (!wait_for_completion_timeout(&race.received, GATE_WAIT)) {
		result = -ETIMEDOUT;
		goto release;
	}
	if (race.kind == KOBOX_VM_RACE_PRESSURE_FAULT) {
		report->pressure.size = sizeof(report->pressure);
		result = kobox_linux_pressure_mutate(&report->pressure,
				truncate_under_pressure, workers, -1);
	} else {
		result = truncate_under_pressure(workers);
	}
	if (result)
		goto release;
	if (mapping->nrpages || i_size_read(mapping->host)) {
		result = -EINVAL;
		goto release;
	}
	reclaimed = reacquire_pfn(pfn, mapping_gfp_mask(mapping), probes, capacity, report);
	if (IS_ERR(reclaimed)) {
		result = PTR_ERR(reclaimed);
		reclaimed = NULL;
		goto release;
	}
	memset(page_address(reclaimed), 0xb6, PAGE_SIZE);
	report->reclaimed_pages++;
release:
	/* A delayed write must neither publish nor touch the reused page. */
	complete_all(&race.resume);
	if (!wait_for_completion_timeout(&publisher->done, GATE_WAIT))
		__builtin_trap();
	publisher->race = NULL;
	if (!result && (publisher->result || publisher->signal != SIGBUS ||
			publisher->faults != faults + 1 ||
			publisher->space->publications != publications))
		result = -EINVAL;
	if (!result) {
		report->delayed_faults++;
		report->revoked_aliases++;
		workers[0].write = 0;
		faults = workers[0].faults;
		result = run(&workers[0], ACCESS);
		if (!result && (workers[0].signal != SIGBUS ||
			       workers[0].faults != faults + 1))
			result = -EINVAL;
		if (!result)
			report->revoked_aliases++;
	}
	if (reclaimed) {
		if (memchr_inv(page_address(reclaimed), 0xb6, PAGE_SIZE))
			result = -EINVAL;
		__free_page(reclaimed);
	}
	kvfree(probes);
	report->result = result;
	return result;
}

static int publication_vs_invalidation(struct vm_worker *workers)
{
	struct kobox_linux_vm_report *report = workers[0].report;
	struct publication_race race = {
		.publisher = &workers[1], .invalidator = &workers[0],
		.kind = workers[0].host->race_case, .armed = true,
	};
	struct address_space *mapping = workers[0].file->f_mapping;
	struct page *reclaimed = NULL;
	struct page **probes;
	unsigned int capacity = totalram_pages();
	struct folio *folio;
	unsigned long pfn;
	unsigned int before;
	bool exiting = race.kind == KOBOX_VM_RACE_EXIT ||
		       race.kind == KOBOX_VM_RACE_EXIT_PUBLISH;
	int result;

	if (num_online_cpus() != 2 || !workers[0].host->notify)
		return -EINVAL;
	report->phase = 20;
	/* Discard the host alias through real Linux VMA operations. */
	result = run(&workers[1], UNMAP);
	if (!result)
		result = run(&workers[1], MAP_BACKING);
	if (result)
		return result;
	lru_add_drain_all();
	folio = filemap_get_folio(mapping, 0);
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	pfn = folio_pfn(folio);
	folio_put(folio);
	probes = kvcalloc(capacity, sizeof(*probes), GFP_KERNEL);
	if (!probes)
		return -ENOMEM;
	init_completion(&race.mutated);
	init_completion(&race.resolved);
	init_completion(&race.resume);
	init_waitqueue_func_entry(&race.irq_probe, publication_irq);
	atomic_set(&race.irqs, 0);
	atomic_set(&race.errors, 0);
	race.publications = workers[1].space->publications;
	workers[0].race = &race;
	workers[1].race = &race;
	workers[0].action = race.kind == KOBOX_VM_RACE_IRQ ? FLUSH_ALL : TRUNCATE;
	if (race.kind == KOBOX_VM_RACE_EXIT_PUBLISH)
		workers[0].action = EXIT_TRUNCATE;
	workers[1].action = ACCESS;
	workers[1].write = 0;
	before = workers[1].faults;
	reinit_completion(&workers[0].done);
	reinit_completion(&workers[1].done);
	if (race.kind == KOBOX_VM_RACE_IRQ)
		add_wait_queue(&workers[1].space->events, &race.irq_probe);
	complete(&workers[1].go);
	report->phase = 21;
	if (!wait_for_completion_timeout(&race.resolved, GATE_WAIT)) {
		result = -ETIMEDOUT;
		goto release;
	}
	result = workers[0].result;
	if (result)
		goto release;
	report->phase = 22;
	if (race.kind != KOBOX_VM_RACE_IRQ) {
		if (mapping->nrpages || i_size_read(file_inode(workers[0].file))) {
			result = -EINVAL;
			goto release;
		}
		reclaimed = reacquire_pfn(pfn, mapping_gfp_mask(mapping), probes, capacity, report);
		if (IS_ERR(reclaimed)) {
			result = PTR_ERR(reclaimed);
			reclaimed = NULL;
			goto release;
		}
		memset(page_address(reclaimed), 0xb6, PAGE_SIZE);
		report->reclaimed_pages++;
	}
	if (race.kind == KOBOX_VM_RACE_EXIT) {
		result = workers[1].host->operations->close(workers[1].native);
		if (result)
			goto release;
		report->dead_spaces++;
	}
	if (race.kind == KOBOX_VM_RACE_EXIT_PUBLISH) {
		result = workers[1].host->operations->resume(workers[1].native,
							  workers[1].sequence);
		if (result != -ESRCH ||
		    workers[1].space->publications != race.publications) {
			result = -EINVAL;
			goto release;
		}
		report->stale_resumes++;
		result = 0;
	}
release:
	complete_all(&race.resume);
	if (!wait_for_completion_timeout(&workers[1].done, GATE_WAIT))
		__builtin_trap();
	if (race.started &&
	    !wait_for_completion_timeout(&workers[0].done, GATE_WAIT))
		__builtin_trap();
	if (race.kind == KOBOX_VM_RACE_IRQ)
		remove_wait_queue(&workers[1].space->events, &race.irq_probe);
	workers[0].race = NULL;
	workers[1].race = NULL;
	if (result)
		goto out;
	report->phase = 23;
	if (workers[1].result != (exiting ? -ESRCH : 0) ||
	    workers[1].faults < before + (exiting ? 1 : 2) ||
	    report->publication_races != 1 || report->stale_resumes != 1) {
		result = -EINVAL;
		goto out;
	}
	if (race.kind == KOBOX_VM_RACE_IRQ) {
		report->publication_irqs = atomic_read(&race.irqs);
		if (!report->publication_irqs || atomic_read(&race.errors) ||
		    workers[1].signal || workers[1].observed != workers[1].value)
			result = -EINVAL;
		goto out;
	}
	if (race.kind == KOBOX_VM_RACE_TRUNCATE) {
		if (workers[1].signal != SIGBUS) {
			result = -EINVAL;
			goto out;
		}
		report->revoked_aliases++;
	} else {
		/* A dead binding must not accept a delayed PTE publication either. */
		result = workers[1].host->operations->map(workers[1].native,
			workers[1].address, PFN_PHYS(pfn), PAGE_SIZE, KOBOX_VM_READ);
		if (result != -ESRCH) {
			result = -EINVAL;
			goto out;
		}
	}
	report->phase = 24;
	workers[0].write = 0;
	before = workers[0].faults;
	result = run(&workers[0], ACCESS);
	if (!result && (workers[0].signal != SIGBUS ||
		       workers[0].faults != before + 1))
		result = -EINVAL;
	if (!result)
		report->revoked_aliases++;
out:
	if (reclaimed) {
		if (memchr_inv(page_address(reclaimed), 0xb6, PAGE_SIZE))
			result = -EINVAL;
		__free_page(reclaimed);
	}
	kvfree(probes);
	report->result = result;
	return result;
}

/* The MM Gate requires all cases, not a passing return from one probe.
 * Every case also verifies final reclamation after its users have joined.
 */
int kobox_linux_vm_probe(const struct kobox_linux_vm_test *host,
	struct kobox_linux_vm_report *report)
{
	struct vm_worker *workers;
	struct kobox_vm_lifetime *audit = NULL;
	struct file *file;
	loff_t position = 0;
	u64 initial = 0x1234abcd;
	int index, result = 0;

#define REQUIRE(expression) do { \
	if (!(expression)) { \
		report->line = __LINE__; \
		result = -EINVAL; \
		goto out; \
	} \
} while (0)
	if (!host || host->size != sizeof(*host) || !report || report->size != sizeof(*report) ||
	    !host->operations || !host->probe || !host->pids[0] || !host->pids[1] ||
	    host->pids[0] == host->pids[1] || host->spaces[0] == host->spaces[1])
		return -EINVAL;
	if (host->operations->size != sizeof(*host->operations) ||
	    !host->operations->map || !host->operations->reset ||
	    !host->operations->close || !host->operations->resume ||
	    !host->operations->event || !host->spaces[0] || !host->spaces[1])
		return -EINVAL;
	workers = kcalloc(2, sizeof(*workers), GFP_KERNEL);
	if (!workers)
		return -ENOMEM;
	file = shmem_file_setup("vm-integration", 2 * PAGE_SIZE, VM_NORESERVE);
	if (IS_ERR(file)) {
		kfree(workers);
		return PTR_ERR(file);
	}
	REQUIRE(kernel_write(file, &initial, sizeof(initial), &position) == sizeof(initial));
	report->phase = 1;
	for (index = 0; index < 2; index++) {
		struct vm_worker *worker = &workers[index];

		worker->host = host;
		worker->report = report;
		worker->file = file;
		worker->native = host->spaces[index];
		worker->space = kobox_vm_space_create(worker, &test_operations,
			host->start, host->length);
		REQUIRE(!IS_ERR(worker->space));
		init_completion(&worker->ready);
		init_completion(&worker->go);
		init_completion(&worker->done);
		worker->task = kthread_create(worker_main, worker, "vm-gate/%d", index);
		REQUIRE(!IS_ERR(worker->task));
		kthread_bind(worker->task, index);
		wake_up_process(worker->task);
		REQUIRE(wait_for_completion_timeout(&worker->ready, GATE_WAIT));
		worker->address = host->start;
		worker->length = 2 * PAGE_SIZE;
		if (index == 1 && host->lifetime_case == KOBOX_VM_LIFETIME_ROLLBACK) {
			REQUIRE(!run(worker, REJECT_MAPPING));
			report->rollbacks++;
			goto out;
		}
		REQUIRE(!run(worker, MAP_BACKING));
		REQUIRE(!run(worker, ACCESS) && !worker->signal && worker->observed == initial);
	}
	REQUIRE(workers[0].space->mm != workers[1].space->mm && workers[0].faults && workers[1].faults);
	report->phase = 2;
	workers[1].write = 1;
	workers[1].value = 0x55667788;
	REQUIRE(!run(&workers[1], ACCESS) && !workers[1].signal &&
		workers[1].observed == workers[1].value);
	REQUIRE(!run(&workers[0], ACCESS) && !workers[0].signal &&
		workers[0].observed == workers[1].value);
	if (host->lifetime_case) {
		if (host->lifetime_case == KOBOX_VM_LIFETIME_DEATH) {
			result = unsolicited_exit(workers);
			REQUIRE(!result);
		}
		goto out;
	}
	if (host->reuse_case) {
		result = reuse_after_invalidation(workers);
		REQUIRE(!result);
		goto out;
	}
	if (host->race_case) {
		result = (host->race_case == KOBOX_VM_RACE_LATE_FAULT ||
			  host->race_case == KOBOX_VM_RACE_PRESSURE_FAULT) ?
			delayed_fault_vs_truncate(workers) : publication_vs_invalidation(workers);
		REQUIRE(!result);
		goto out;
	}
	report->phase = 3;
	workers[0].length = PAGE_SIZE;
	REQUIRE(!run(&workers[0], UNMAP));
	workers[0].address += PAGE_SIZE;
	REQUIRE(!run(&workers[0], ACCESS) && !workers[0].signal && !workers[0].observed);
	workers[1].write = 0;
	REQUIRE(!run(&workers[1], ACCESS) && !workers[1].signal &&
		workers[1].observed == workers[1].value);
	if (host->readonly_case) {
		unsigned int before = workers[0].faults;

		REQUIRE(!run(&workers[0], PROTECT_READONLY));
		workers[1].address = host->start + PAGE_SIZE;
		workers[1].write = 1;
		workers[1].value = 0xabcdef97;
		REQUIRE(!run(&workers[1], ACCESS) && !workers[1].signal);
		REQUIRE(!run(&workers[0], ACCESS) && !workers[0].signal &&
			workers[0].observed == workers[1].value && workers[0].faults > before);
		workers[0].write = 1;
		workers[0].value = 0xdeadbeef;
		REQUIRE(!run(&workers[0], ACCESS) && workers[0].signal == SIGSEGV);
		workers[1].write = 0;
		REQUIRE(!run(&workers[1], ACCESS) && !workers[1].signal &&
			workers[1].observed == workers[1].value);
	} else {
		workers[0].address = host->start;
		REQUIRE(!run(&workers[0], ACCESS) && workers[0].signal == SIGSEGV);
	}
	report->phase = 4;
	REQUIRE(!vfs_truncate(&file->f_path, 0));
	REQUIRE(!run(&workers[1], ACCESS) && workers[1].signal == SIGBUS);
	report->phase = 5;
out:
	if (!result) {
		struct mm_struct *mms[2] = {
			workers[0].space->mm, workers[1].space->mm,
		};
		struct task_struct *tasks[2] = {
			workers[0].task, workers[1].task,
		};

		audit = kobox_vm_lifetime_begin(file, mms, tasks, report);
		if (IS_ERR(audit)) {
			result = PTR_ERR(audit);
			audit = NULL;
		}
	}
	for (index = 0; index < 2; index++) {
		struct vm_worker *worker = &workers[index];

		report->faults += worker->faults;
		report->accesses += worker->accesses;
		report->mm_checks += worker->checks;
		report->signals += !!worker->signal;
		if (!IS_ERR_OR_NULL(worker->task)) {
			WRITE_ONCE(worker->finish, true);
			complete(&worker->go);
			if (kthread_stop(worker->task))
				__builtin_trap();
		}
		if (!IS_ERR_OR_NULL(worker->space) && kobox_vm_space_destroy(worker->space))
			__builtin_trap();
	}
	if (audit)
		result = kobox_vm_lifetime_finish(audit);
	else
		fput(file);
	if (!result && host->lifetime_case && report->folio_reclaims != 1)
		result = -EINVAL;
	if (!result && host->race_case == KOBOX_VM_RACE_PRESSURE_FAULT &&
	    !report->large_alias_pages)
		result = -EINVAL;
	kfree(workers);
	report->warnings = kobox_linux_exception_warnings();
	if (!result && report->warnings)
		result = -EINVAL;
	if (!report->result)
		report->result = result;
	return result;
#undef REQUIRE
}
