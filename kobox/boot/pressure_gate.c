// SPDX-License-Identifier: GPL-2.0-only

#include "host.h"
#include "diagnostic.h"
#include "pressure_gate.h"

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/list_lru.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>

#define CACHE_PAGES 4096
#define CACHE_DENTRIES 16384
#define HELD_PAGES 4
#define GATE_WAIT (10 * HZ)
#define PRESSURE_GFP (GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN)
#define DIRECT_REQUESTS (2 * CACHE_PAGES)

struct pressure_failure {
	unsigned long cached;
	unsigned long pages;
	long count_calls;
	long scan_calls;
	unsigned int task_flags;
	gfp_t gfp;
	bool reclaim_state;
	bool captured;
};

struct pressure_case {
	struct kobox_linux_pressure_report *report;
	int (*inspect)(void *argument);
	void *inspect_argument;
	long committed_delta;
	struct shrinker *shrinker;
	spinlock_t lock;
	struct list_head cache;
	struct list_head pressure;
	unsigned long cached;
	unsigned long *reclaimed;
	unsigned long *reused;
	unsigned long pfn_limit;
	unsigned long events[NR_VM_EVENT_ITEMS];
	unsigned long committed;
	unsigned long extra_pages;
	struct file *file;
	struct page *pins[2];
	unsigned long pfns[HELD_PAGES];
	void *alias;
	struct vfsmount *mnt;
	struct dentry *held_dentry;
	unsigned long dcache_before;
	atomic_long_t count_calls;
	atomic_long_t scan_calls;
	struct pressure_failure first_nowait;
	struct pressure_failure first_direct;
};

static void snapshot_failure(struct pressure_case *test,
			     struct pressure_failure *failure, gfp_t gfp)
{
	if (failure->captured)
		return;
	failure->cached = READ_ONCE(test->cached);
	failure->pages = test->report->pressure_pages;
	failure->count_calls = atomic_long_read(&test->count_calls);
	failure->scan_calls = atomic_long_read(&test->scan_calls);
	failure->task_flags = current->flags;
	failure->gfp = gfp;
	failure->reclaim_state = current->reclaim_state != NULL;
	failure->captured = true;
}

static void report_failure(const char *stage,
			   const struct pressure_failure *failure)
{
	kobox_linux_boot_diagnostic(
		"kobox pressure: stage=%s captured=%u cached=%lu pages=%lu count=%ld scan=%ld flags=%#x reclaim=%u gfp=%#x\n",
		stage, failure->captured, failure->cached, failure->pages,
		failure->count_calls, failure->scan_calls, failure->task_flags,
		failure->reclaim_state, (__force unsigned int)failure->gfp);
}

static int fail(struct pressure_case *test, unsigned int line, int result)
{
	test->report->line = line;
	test->report->result = result;
	kobox_linux_boot_diagnostic(
		"kobox pressure: fail line=%u result=%d phase=%u cached=%lu count=%ld scan=%ld flags=%#x reclaim=%u\n",
		line, result, test->report->phase, READ_ONCE(test->cached),
		atomic_long_read(&test->count_calls),
		atomic_long_read(&test->scan_calls), current->flags,
		current->reclaim_state != NULL);
	report_failure("first-nowait", &test->first_nowait);
	report_failure("first-direct", &test->first_direct);
	return -EINVAL;
}

/* Only this test cache's expendable copies are released here. This is the
 * normal shrinker client contract, not reclaim policy: Linux chooses when,
 * where and how much to scan. Neither callback is called by the test driver.
 * Shmem data and pins are deliberately outside this cache.
 */
static unsigned long cache_count(struct shrinker *shrinker,
				 struct shrink_control *sc)
{
	struct pressure_case *test = shrinker->private_data;
	unsigned long count = READ_ONCE(test->cached);

	atomic_long_inc(&test->count_calls);
	return count ?: SHRINK_EMPTY;
}

static unsigned long cache_scan(struct shrinker *shrinker,
				struct shrink_control *sc)
{
	struct pressure_case *test = shrinker->private_data;
	struct page *page;
	unsigned long count = 0;

	atomic_long_inc(&test->scan_calls);
	spin_lock(&test->lock);
	while (count < sc->nr_to_scan && !list_empty(&test->cache)) {
		page = list_first_entry(&test->cache, struct page, lru);
		list_del(&page->lru);
		__set_bit(page_to_pfn(page), test->reclaimed);
		__free_page(page);
		test->cached--;
		count++;
	}
	if (current_is_kswapd())
		test->report->kswapd_freed += count;
	else if (current->reclaim_state)
		test->report->direct_freed += count;
	else
		WARN_ON_ONCE(1);
	spin_unlock(&test->lock);
	/* Keep Linux's requested scan budget. A concurrent scan may have
	 * emptied the cache after count_objects(); reporting zero scanned
	 * would prevent do_shrink_slab() from consuming its remaining budget.
	 * The return value, unlike the scan budget, is actual pages freed.
	 */
	return count;
}

static void free_pages_list(struct list_head *list)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, list, lru) {
		list_del(&page->lru);
		__free_page(page);
	}
}

static void keep_pressure_page(struct pressure_case *test, struct page *page)
{
	unsigned long pfn = page_to_pfn(page);

	list_add(&page->lru, &test->pressure);
	test->report->pressure_pages++;
	spin_lock(&test->lock);
	if (test_bit(pfn, test->reclaimed) &&
	    !__test_and_set_bit(pfn, test->reused))
		test->report->cache_reused++;
	spin_unlock(&test->lock);
}

static void exhaust_ram(struct pressure_case *test, gfp_t flags)
{
	struct page *page;

	while ((page = alloc_page(flags | __GFP_NOWARN)))
		keep_pressure_page(test, page);
	snapshot_failure(test, &test->first_nowait, flags | __GFP_NOWARN);
}

static int populate_cache(struct pressure_case *test)
{
	struct page *page;
	unsigned int i;

	for (i = 0; i < CACHE_PAGES; i++) {
		page = alloc_page(GFP_KERNEL);
		if (!page)
			return fail(test, __LINE__, -ENOMEM);
		memset(page_address(page), i & 255, PAGE_SIZE);
		spin_lock(&test->lock);
		list_add(&page->lru, &test->cache);
		test->cached++;
		spin_unlock(&test->lock);
	}
	return 0;
}

static int populate_dcache(struct pressure_case *test)
{
	struct file_system_type *type = get_fs_type("sysfs");
	struct dentry *dentry;
	unsigned int i;
	char name[40];

	if (!type)
		return fail(test, __LINE__, -ENOENT);
	test->mnt = kern_mount(type);
	put_filesystem(type);
	if (IS_ERR(test->mnt))
		return fail(test, __LINE__, PTR_ERR(test->mnt));
	/* Tmpfs deliberately does not cache negative lookups. Kernfs does:
	 * use its real lookup path and superblock shrinker, without changing
	 * any cache flags. One referenced entry must survive the pressure.
	 */
	for (i = 0; i <= CACHE_DENTRIES; i++) {
		struct qstr component;

		snprintf(name, sizeof(name), "pressure-negative-%u", i);
		component = (struct qstr)QSTR_INIT(name, strlen(name));
		inode_lock(d_inode(test->mnt->mnt_root));
		dentry = lookup_one(mnt_idmap(test->mnt), &component,
				    test->mnt->mnt_root);
		inode_unlock(d_inode(test->mnt->mnt_root));
		if (IS_ERR(dentry))
			return fail(test, __LINE__, PTR_ERR(dentry));
		if (i == CACHE_DENTRIES)
			test->held_dentry = dentry;
		else
			dput(dentry);
	}
	test->dcache_before = list_lru_count(&test->mnt->mnt_sb->s_dentry_lru);
	if (test->dcache_before < CACHE_DENTRIES)
		return fail(test, __LINE__, test->dcache_before);
	return 0;
}

static int create_retained(struct pressure_case *test)
{
	struct page *page;
	unsigned int i;

	test->committed = vm_memory_committed();
	test->file = shmem_file_setup("pressure-retained",
				     (HELD_PAGES + totalram_pages()) * PAGE_SIZE,
				     VM_NORESERVE);
	if (IS_ERR(test->file))
		return fail(test, __LINE__, PTR_ERR(test->file));
	for (i = 0; i < HELD_PAGES; i++) {
		page = shmem_read_mapping_page(test->file->f_mapping, i);
		if (IS_ERR(page))
			return fail(test, __LINE__, PTR_ERR(page));
		memset(page_address(page), 0xa0 + i, PAGE_SIZE);
		set_page_dirty(page);
		test->pfns[i] = page_to_pfn(page);
		if (i < ARRAY_SIZE(test->pins))
			test->pins[i] = page;
		else
			put_page(page);
	}
	test->alias = vmap(test->pins, ARRAY_SIZE(test->pins), VM_MAP, PAGE_KERNEL);
	if (!test->alias)
		return fail(test, __LINE__, -ENOMEM);
	lru_add_drain_all();
	return 0;
}

static int check_retained(struct pressure_case *test)
{
	struct inode *inode = file_inode(test->file);
	unsigned long expected = HELD_PAGES + test->extra_pages;
	struct page *page;
	unsigned int i;

	if (file_count(test->file) != 1 || inode->i_mapping->nrpages != expected ||
	    SHMEM_I(inode)->alloced != expected || SHMEM_I(inode)->swapped ||
	    inode->i_blocks != expected * (PAGE_SIZE / 512) ||
	    vm_memory_committed() != test->committed + expected)
		return fail(test, __LINE__, -EINVAL);
	for (i = 0; i < HELD_PAGES; i++) {
		page = shmem_read_mapping_page_gfp(inode->i_mapping, i, GFP_NOWAIT);
		if (IS_ERR(page))
			return fail(test, __LINE__, PTR_ERR(page));
		if (page_to_pfn(page) != test->pfns[i] ||
		    memchr_inv(page_address(page), 0xa0 + i, PAGE_SIZE))
			return fail(test, __LINE__, -EINVAL);
		put_page(page);
		if (i < ARRAY_SIZE(test->pins) &&
		    (vmalloc_to_page(test->alias + i * PAGE_SIZE) != test->pins[i] ||
		     memchr_inv(test->alias + i * PAGE_SIZE, 0xa0 + i, PAGE_SIZE)))
			return fail(test, __LINE__, -EINVAL);
		test->report->retained++;
	}
	for (i = 0; i < test->extra_pages; i++) {
		page = shmem_read_mapping_page_gfp(inode->i_mapping,
						 HELD_PAGES + i, GFP_NOWAIT);
		if (IS_ERR(page))
			return fail(test, __LINE__, PTR_ERR(page));
		if (memchr_inv(page_address(page), 0x6d, PAGE_SIZE))
			return fail(test, __LINE__, -EINVAL);
		put_page(page);
		test->report->retained++;
	}
	if (d_unhashed(test->held_dentry) || !d_is_negative(test->held_dentry))
		return fail(test, __LINE__, -EINVAL);
	return 0;
}

static int require_direct_reclaim(struct pressure_case *test)
{
	gfp_t gfp = PRESSURE_GFP & ~__GFP_KSWAPD_RECLAIM;
	unsigned long deadline = jiffies + GATE_WAIT;
	struct page *page;
	unsigned int request;

	/*
	 * NORETRY bounds each allocation, not the lifetime of a reclaimable
	 * cache. One reclaim pass can stop after freeing other caches before
	 * this shrinker earns a scan budget, yet still fail its allocation.
	 * Keep issuing real requests while retaining every successful page.
	 * Require both an allocation and actual direct frees within the bounds;
	 * neither count_objects() nor an intermediate NULL proves progress.
	 */
	for (request = 0; request < DIRECT_REQUESTS; request++) {
		page = alloc_page(gfp);
		if (page) {
			keep_pressure_page(test, page);
			if (READ_ONCE(test->report->direct_freed))
				return 0;
		} else {
			snapshot_failure(test, &test->first_direct, gfp);
		}
		if (time_after_eq(jiffies, deadline))
			break;
		cond_resched();
	}
	return fail(test, __LINE__, -ETIMEDOUT);
}

static int run_pressure(struct pressure_case *test)
{
	unsigned long deadline, unused;
	struct page *page;
	unsigned int i;

	test->report->phase = 2;
	all_vm_events(test->events);
	/* Select direct reclaim using the upstream caller's GFP contract.
	 * No kswapd wake is requested by these allocations; no thread is
	 * stopped, frozen or replaced to control the outcome.
	 */
	exhaust_ram(test, GFP_NOWAIT & ~__GFP_KSWAPD_RECLAIM);
	if (require_direct_reclaim(test))
		return -EINVAL;
	free_pages_list(&test->pressure);
	if (populate_cache(test))
		return -EINVAL;
	/* NOWAIT wakes kswapd but cannot itself enter direct reclaim. Observe
	 * actual frees and reacquire their PFNs while no test teardown runs.
	 */
	deadline = jiffies + GATE_WAIT;
	do {
		exhaust_ram(test, GFP_NOWAIT);
		if (READ_ONCE(test->report->kswapd_freed) &&
		    READ_ONCE(test->report->cache_reused))
			break;
		msleep(20);
	} while (time_before(jiffies, deadline));
	if (!test->report->kswapd_freed || !test->report->cache_reused ||
	    test->report->pressure_pages < 4096)
		return fail(test, __LINE__, -ETIMEDOUT);
	test->report->phase = 3;
	/* Observe a bounded allocation failure under retained pressure.
	 * NORETRY prevents this bounded negative test from invoking the OOM
	 * killer or relying on killing an unrelated task to regain memory.
	 */
	for (i = 0; i < 2 * CACHE_PAGES; i++) {
		page = alloc_page(PRESSURE_GFP);
		if (!page)
			break;
		keep_pressure_page(test, page);
	}
	if (i == 2 * CACHE_PAGES)
		return fail(test, __LINE__, -EOVERFLOW);
	test->report->failures++;
	unused = list_lru_count(&test->mnt->mnt_sb->s_dentry_lru);
	if (unused >= test->dcache_before)
		return fail(test, __LINE__, -EINVAL);
	test->report->dentries_freed = test->dcache_before - unused;
	if (check_retained(test))
		return -EINVAL;
	test->report->phase = 4;
	/* RCU/VFS cleanup can return RAM after the first allocation failure.
	 * Consume any such progress with new shmem pages until its own path
	 * fails. Keep every successful page alive: none is discarded as reclaim.
	 */
	for (i = 0; i < totalram_pages() - 1; i++) {
		exhaust_ram(test, GFP_NOWAIT);
		page = shmem_read_mapping_page_gfp(test->file->f_mapping,
				HELD_PAGES + test->extra_pages, PRESSURE_GFP);
		if (IS_ERR(page))
			break;
		if (memchr_inv(page_address(page), 0, PAGE_SIZE))
			return fail(test, __LINE__, -EINVAL);
		/* A freshly read hole contains reconstructible zeroes. Write real
		 * data and mark it dirty before dropping our reference, so native
		 * reclaim cannot reuse it as a clean, zero-filled cache entry.
		 */
		memset(page_address(page), 0x6d, PAGE_SIZE);
		set_page_dirty(page);
		put_page(page);
		test->extra_pages++;
	}
	if (i == totalram_pages() - 1)
		return fail(test, __LINE__, -EOVERFLOW);
	if (PTR_ERR(page) != -ENOMEM)
		return fail(test, __LINE__, PTR_ERR(page));
	test->report->failures++;
	if (check_retained(test))
		return -EINVAL;
	if (test->inspect) {
		int result = test->inspect(test->inspect_argument);

		if (result)
			return fail(test, __LINE__, result);
		/* The mutating test must independently verify this exact delta.
		 * Do not infer a new baseline from the observed global counter.
		 */
		test->committed += test->committed_delta;
	}
	free_pages_list(&test->pressure);
	test->report->phase = 5;
	page = shmem_read_mapping_page_gfp(test->file->f_mapping,
				HELD_PAGES + test->extra_pages, PRESSURE_GFP);
	if (IS_ERR(page))
		return fail(test, __LINE__, PTR_ERR(page));
	if (memchr_inv(page_address(page), 0, PAGE_SIZE))
		return fail(test, __LINE__, -EINVAL);
	memset(page_address(page), 0x6d, PAGE_SIZE);
	set_page_dirty(page);
	put_page(page);
	test->extra_pages++;
	if (check_retained(test))
		return -EINVAL;
	test->report->recovered++;
	return 0;
}

int kobox_linux_pressure_mutate(struct kobox_linux_pressure_report *report,
		int (*inspect)(void *), void *argument, long committed_delta)
{
	struct pressure_case *test;
	unsigned long events[NR_VM_EVENT_ITEMS];
	unsigned int i;
	int result;

	if (!report || report->size != sizeof(*report) ||
	    system_state != SYSTEM_RUNNING || num_online_cpus() != 2 ||
	    IS_ENABLED(CONFIG_SWAP) || !IS_ENABLED(CONFIG_VM_EVENT_COUNTERS) ||
	    (!inspect && committed_delta))
		return -EINVAL;
	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->report = report;
	test->inspect = inspect;
	test->inspect_argument = argument;
	test->committed_delta = committed_delta;
	report->phase = 1;
	spin_lock_init(&test->lock);
	INIT_LIST_HEAD(&test->cache);
	INIT_LIST_HEAD(&test->pressure);
	test->pfn_limit = max_pfn;
	test->reclaimed = bitmap_zalloc(test->pfn_limit, GFP_KERNEL);
	test->reused = bitmap_zalloc(test->pfn_limit, GFP_KERNEL);
	test->shrinker = shrinker_alloc(0, "kobox-pressure-cache");
	if (!test->reclaimed || !test->reused || !test->shrinker)
		return fail(test, __LINE__, -ENOMEM);
	test->shrinker->count_objects = cache_count;
	test->shrinker->scan_objects = cache_scan;
	test->shrinker->seeks = DEFAULT_SEEKS;
	test->shrinker->private_data = test;
	result = populate_cache(test) ?: populate_dcache(test) ?: create_retained(test);
	if (result)
		return result;
	shrinker_register(test->shrinker);
	result = run_pressure(test);
	if (result)
		return result;
	/* Unregistration drains in-flight callers before freeing private data. */
	shrinker_free(test->shrinker);
	free_pages_list(&test->cache);
	vunmap(test->alias);
	for (i = 0; i < ARRAY_SIZE(test->pins); i++)
		put_page(test->pins[i]);
	__fput_sync(test->file);
	if (vm_memory_committed() != test->committed)
		return fail(test, __LINE__, -EINVAL);
	dput(test->held_dentry);
	kern_unmount(test->mnt);
	rcu_barrier();
	all_vm_events(events);
	if (events[SLABS_SCANNED] <= test->events[SLABS_SCANNED] ||
	    events[OOM_KILL] != test->events[OOM_KILL])
		return fail(test, __LINE__, -EINVAL);
	bitmap_free(test->reclaimed);
	bitmap_free(test->reused);
	kfree(test);
	report->warnings = kobox_linux_exception_warnings();
	report->phase = 6;
	return report->warnings ? -EINVAL : 0;
}

int kobox_linux_pressure_inspect(struct kobox_linux_pressure_report *report,
				 int (*inspect)(void *), void *argument)
{
	return kobox_linux_pressure_mutate(report, inspect, argument, 0);
}

__attribute__((visibility("default")))
int kobox_linux_pressure_verify(struct kobox_linux_pressure_report *report)
{
	return kobox_linux_pressure_inspect(report, NULL, NULL);
}
