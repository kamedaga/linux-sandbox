// SPDX-License-Identifier: GPL-2.0-only
#include "reporting.h"
#include "reporting_ranges.h"
#include "resident_policy.h"
#include <linux/mutex.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/oom.h>
/* Upstream allocator helper, linked into the hosted core (mm/internal.h). */
extern void setup_per_zone_wmarks(void);
#include "../arch/x86_64/host_call.h"
#include "../boot/diagnostic.h"
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/swap.h>
#include <linux/gfp.h>
#include <linux/workqueue.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
static atomic64_t reported_ok_pages = ATOMIC64_INIT(0);
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#ifdef CONFIG_PAGE_REPORTING
#include <linux/mm.h>
#include <linux/page_reporting.h>

static const struct kobox_linux_memory_layout *reporting_memory;
static bool logged;

static int discard(void *backing, const struct kobox_linux_memory_range *ranges,
		   size_t count)
{
	return kobox_host_call(reporting_memory->operations->discard(backing, ranges, count));
}

static int report(struct page_reporting_dev_info *device,
		  struct scatterlist *sgl, unsigned int count)
{
	struct kobox_linux_memory_range ranges[PAGE_REPORTING_CAPACITY];
	struct scatterlist *sg;
	unsigned int i;
	uint64_t pages = 0;
	int status;

	(void)device;
	if (!count || count > PAGE_REPORTING_CAPACITY || in_atomic() || irqs_disabled())
		return -EINVAL;
	might_sleep();
	for_each_sg(sgl, sg, count, i) {
		struct kobox_reporting_segment segment = {
			.pfn = page_to_pfn(sg_page(sg)),
			.offset = sg->offset,
			.length = sg->length,
		};
		if (kobox_reporting_convert(segment, reporting_memory->ram_size >> PAGE_SHIFT, &ranges[i]))
			return -EINVAL;
		pages += ranges[i].page_count;
	}
	status = kobox_reporting_submit(ranges, count, discard, reporting_memory->ram_backing);
	/* Any nonzero native result leaves the entire SG unreported. NOT_READY
	 * can be partial success: live views, DMA pins/retention/quarantine or
	 * transient slot pressure leave ranges busy. Already discarded holes
	 * are safe on retry; upstream keeps ownership of the entire SG.
	 */
	/* Reporting success can include already absent native pages. This is
	 * deliberately NOT an exact counter of newly returned physical bytes. */
	if (!status) atomic64_add(pages, &reported_ok_pages);
	if (!status && !logged) {
		logged = true;
		kobox_linux_boot_diagnostic("kobox-reporting: synchronous discard OK segments=%u pages=%llu\n",
			count, (unsigned long long)pages);
	}
	return status ? -EIO : 0;
}

static struct page_reporting_dev_info reporting_device = {.report = report};
#endif

/* One owner serializes reserved-tail supply and balloon transitions. List
 * links live in resident struct-page metadata, never in discarded data. */
static const struct kobox_linux_memory_layout *resident_memory;
static DEFINE_MUTEX(resident_lock);
static LIST_HEAD(balloon);
static struct delayed_work supply_work;
static u64 supply_cursor;
/* Publish admission before returning any page of a proven batch to buddy. */
static u64 admitted_end;
void kobox_linux_resident_assert_page(struct page *page)
{
 /* The allocation owner excludes balloon, and supply only grows the admitted
  * prefix. Reserved tail pages cannot enter buddy or a cache lease. */
 if (resident_memory && (!page || PageReserved(page) ||
     page_to_pfn(page) >= READ_ONCE(admitted_end)))
  panic("kobox-resident: cache lease outside admitted resident pool");
}
EXPORT_SYMBOL_GPL(kobox_linux_resident_assert_page);
static unsigned long supply_cooldown;
static unsigned long balloon_cooldown;
static bool supply_warm = true;
/* Explicit host-test entry only: ordinary Linux allocations pin test data;
 * production never starts this worker. Release all test allocations on ALLOC. */
static bool resident_probe_failed;
static bool resident_probe_started;
static LIST_HEAD(resident_probe_pages);
static struct delayed_work resident_probe_work;
static int resident_populate(void *context, uint64_t first, uint64_t count)
{
 const struct kobox_linux_memory_layout *m = context;
 return kobox_host_call(m->operations->populate(m->ram_backing, first, count));
}
static void resident_publish(void *context, uint64_t first, uint64_t count)
{
 u64 i;
 (void)context;
 WRITE_ONCE(admitted_end, first + count);
 for (i = 0; i < count; ++i) {
  struct page *page = pfn_to_page(first + i);
  if (!PageReserved(page)) panic("kobox-resident: duplicate PFN publication");
  free_reserved_page(page); /* updates zone managed count */
 }
}
static bool resident_low(void)
{
 struct zone *zone;
 unsigned long free = 0, low = 0;
 /* A depleted DMA zone cannot be refilled by supplying a DMA32/Normal
  * tail. Use aggregate watermarks rather than draining native memory in
  * response to an unrelated zone which this cursor cannot replenish. */
 for_each_populated_zone(zone) {
  free += zone_page_state(zone, NR_FREE_PAGES);
  low += low_wmark_pages(zone);
 }
 return free < low + KOBOX_RESIDENT_BATCH;
}
static void resident_status(int status)
{
 if (!status) return;
 if (status != KOBOX_RESIDENT_ALLOC)
  panic("kobox-resident: populate fatal status=%d", status);
 resident_probe_failed = true;
 supply_cooldown = jiffies + msecs_to_jiffies(5000);
 kobox_linux_boot_diagnostic("kobox-resident: ALLOC supply stopped cooldown_ms=5000 pfn=%llu\n",
  (unsigned long long)supply_cursor);
}
static void supply_run(struct work_struct *work)
{
 u64 before;
 unsigned int supplied = 0;
 int status = 0;
 (void)work;
 mutex_lock(&resident_lock);
 if (time_before(jiffies, supply_cooldown) || (!supply_warm && !resident_low())) goto out;
 supply_warm = false;
 /* Deflate held pages first; unlike first publication these are ordinary
  * allocated pages. Never use free_reserved_page for a balloon page. */
 while (supplied < KOBOX_RESIDENT_BATCH && !list_empty(&balloon)) {
  struct page *page = list_first_entry(&balloon, struct page, lru);
  status = resident_populate((void *)resident_memory, page_to_pfn(page), 1);
  if (status) break;
  list_del_init(&page->lru);
  adjust_managed_page_count(page, 1);
  __free_page(page);
  ++supplied;
 }
 before = supply_cursor;
 if (!status && !supplied)
  status = kobox_resident_supply((void *)resident_memory, &supply_cursor,
   resident_memory->ram_size >> PAGE_SHIFT, resident_populate, resident_publish);
 supplied += supply_cursor - before;
 if (supplied) {
  setup_per_zone_wmarks();
  kobox_linux_boot_diagnostic("kobox-resident: populate published pages=%u next_pfn=%llu balloon_empty=%u\n",
   supplied, (unsigned long long)supply_cursor, list_empty(&balloon));
 }
 resident_status(status);
out:
 mutex_unlock(&resident_lock);
 queue_delayed_work(system_unbound_wq, &supply_work, msecs_to_jiffies(100));
}
static void resident_probe_run(struct work_struct *work)
{
 unsigned int i;
 (void)work;
 mutex_lock(&resident_lock);
 if (resident_probe_failed) {
  while (!list_empty(&resident_probe_pages)) {
   struct page *page = list_first_entry(&resident_probe_pages, struct page, lru);
   list_del_init(&page->lru);
   __free_page(page); /* resident throughout: these were never discarded */
  }
  kobox_linux_boot_diagnostic("kobox-resident-test: survived ALLOC; released held resident pages\n");
  mutex_unlock(&resident_lock);
  return;
 }
 for (i = 0; i < KOBOX_RESIDENT_BATCH; ++i) {
  struct page *page;
  /* Leave low watermarks plus one supply batch as spare. Active coherent
   * allocations, RX/TX rings and in-flight buffers remain owned by their
   * drivers; normal order-0 allocation cannot take them or pinned BOs. No
   * packet-path populate/discard or reclaim of a driver's private pool. */
  if (resident_low()) break;
  page = alloc_page(GFP_NOWAIT | __GFP_NOWARN | __GFP_NORETRY);
  if (!page) break;
  list_add_tail(&page->lru, &resident_probe_pages);
 }
 mutex_unlock(&resident_lock);
 queue_delayed_work(system_unbound_wq, &resident_probe_work, msecs_to_jiffies(25));
}
int kobox_linux_resident_test_start(void)
{
 if (!resident_memory || resident_probe_started) return -EINVAL;
 resident_probe_started = true;
 resident_probe_failed = false;
 INIT_DELAYED_WORK(&resident_probe_work, resident_probe_run);
 /* Let FileD and the first pressure balloon finish booting first. */
 queue_delayed_work(system_unbound_wq, &resident_probe_work, msecs_to_jiffies(20000));
 kobox_linux_boot_diagnostic("kobox-resident-test: armed Linux allocation load delay_ms=20000\n");
 return 0;
}

static u64 balloon_inflate(u64 requested)
{
 u64 returned = 0, attempted, held = 0;
 mutex_lock(&resident_lock);
 /* Entry user-copy can still need resources. After a resource
  * failure, taking another Linux page per pressure poll would shrink Linux
  * one page at a time while native stays exhausted; wait instead. */
 if (time_before(jiffies, balloon_cooldown)) {
  mutex_unlock(&resident_lock);
  return 0;
 }
 /* Give native users time to use returned pages before another admission. */
 supply_cooldown = jiffies + msecs_to_jiffies(1000);
 for (attempted = 0; attempted < requested; ++attempted) {
  struct page *page;
  /* Leave low watermarks plus one supply batch as spare. Active coherent
   * allocations, RX/TX rings and in-flight buffers remain owned by their
   * drivers; normal order-0 allocation cannot take them or pinned BOs. No
   * packet-path populate/discard or reclaim of a driver's private pool. */
  if (resident_low()) break;
  page = alloc_page(GFP_NOWAIT | __GFP_NOWARN | __GFP_NORETRY);
  struct kobox_linux_memory_range range;
  int status;
  if (!page) break;
  range = (struct kobox_linux_memory_range){page_to_pfn(page), 1};
  /* Remove capacity before discard. No allocator owns this page again
   * until a successful populate, even if discard reports partial failure. */
  adjust_managed_page_count(page, -1);
  list_add_tail(&page->lru, &balloon);
  ++held;
  status = kobox_host_call(resident_memory->operations->discard(
   resident_memory->ram_backing, &range, 1));
  /* NOT_READY includes views, DMA retention and slot pressure. Keep every
   * held page until populate succeeds, including any already discarded hole. */
  if (!status) ++returned;
  else if (status == -1 || status == -4) /* native INVALID / MAP */
   panic("kobox-balloon: discard fatal status=%d", status);
  else if (status != -2) {
   kobox_linux_boot_diagnostic("kobox-balloon: discard retained status=%d cooldown_ms=5000\n", status);
   balloon_cooldown = jiffies + msecs_to_jiffies(5000);
   break; /* Resource or transient failure: held, never returned unpopulated. */
  }
  cond_resched();
 }
 setup_per_zone_wmarks();
 mutex_unlock(&resident_lock);
 kobox_linux_boot_diagnostic("kobox-balloon: inflate discard pages=%llu held=%llu\n",
  (unsigned long long)returned, (unsigned long long)held);
 return returned;
}

int kobox_linux_reporting_start(const struct kobox_linux_memory_layout *memory)
{
#ifdef CONFIG_PAGE_REPORTING
	int status;
#endif

	if (memory->initial_resident_size) {
		if (resident_memory || !memory->operations->populate || !memory->operations->discard)
			return -EINVAL;
		resident_memory = memory;
		supply_cursor = memory->initial_resident_size >> PAGE_SHIFT;
		WRITE_ONCE(admitted_end, supply_cursor);
		INIT_DELAYED_WORK(&supply_work, supply_run);
		queue_delayed_work(system_unbound_wq, &supply_work, 0);
		kobox_linux_boot_diagnostic("kobox-resident: started initial_pages=%llu arena_pages=%llu reporting=disabled\n",
		 (unsigned long long)supply_cursor, (unsigned long long)(memory->ram_size >> PAGE_SHIFT));
		return 0;
	}
	if (!memory->operations->discard)
		return 0;
#ifdef CONFIG_PAGE_REPORTING
	if (reporting_memory || system_state != SYSTEM_RUNNING ||
	    !rcu_inkernel_boot_has_ended() || in_atomic() || irqs_disabled())
		return -EINVAL;
	/* Process-lifetime core/backing: no module unload. If a future shutdown
	 * releases backing, unregister (RCU + cancel_delayed_work_sync) first.
	 */
	reporting_memory = memory;
	status = page_reporting_register(&reporting_device);
	if (status)
		reporting_memory = NULL;
	else
		kobox_linux_boot_diagnostic("kobox-reporting: registered (sleepable, synchronous)\n");
	return status;
#else
	return -EOPNOTSUPP;
#endif
}

/* One outstanding request per process, owned by the Linux FS control task.
 * Work runs on Linux's sleepable unbound queue, never on a native receiver.
 * No IO/FS reclaim: dirty pages and live shmem data are retained. Bound the
 * number of direct-reclaim invocations and check time between invocations;
 * upstream reclaim itself is not a hard real-time primitive.
 */
static struct delayed_work pressure_work;
static atomic_t pressure_state = ATOMIC_INIT(0); /* 0 idle, 1 work, 2 result */
static struct kobox_linux_pressure_result pressure_result;
static u64 pressure_before, pressure_deadline;
static bool pressure_reporting_wait, pressure_started;
static int (*pressure_wake)(void *);
static void *pressure_context;
static void pressure_run(struct work_struct *work)
{
 unsigned int i;
 u64 end;
 (void)work;
 if (!pressure_reporting_wait) {
  end = min_t(u64, pressure_deadline, ktime_get_ns() + 100 * NSEC_PER_MSEC);
  pressure_before = atomic64_read(&reported_ok_pages);
  for (i = 0; i < 8 && ktime_get_ns() < end &&
       pressure_result.linux_freed_pages < pressure_result.requested_pages; ++i) {
   unsigned long freed = try_to_free_pages(node_zonelist(numa_node_id(), GFP_KERNEL),
    0, (GFP_KERNEL & ~(__GFP_IO | __GFP_FS)) | __GFP_NORETRY | __GFP_NOWARN, NULL);
   pressure_result.linux_freed_pages += freed;
   if (!freed) break;
   cond_resched();
  }
  if (resident_memory) {
   pressure_result.reported_ok_pages = balloon_inflate(pressure_result.requested_pages);
   goto complete;
  }
  /* Reporting uses its existing delayed isolation/discard path. Do not
   * scan buddy pages or purge all caches to manufacture an acknowledgement. */
  pressure_reporting_wait = true;
  queue_delayed_work(system_unbound_wq, &pressure_work, msecs_to_jiffies(2500));
  return;
 }
 pressure_result.reported_ok_pages = atomic64_read(&reported_ok_pages) - pressure_before;
complete:
 atomic_set_release(&pressure_state, 2);
 kobox_host_call(pressure_wake(pressure_context));
}
int kobox_linux_pressure_start(int (*wake)(void *), void *context)
{
 if (pressure_started || !wake) return -EINVAL;
 INIT_DELAYED_WORK(&pressure_work, pressure_run);
 pressure_wake = wake; pressure_context = context;
 pressure_started = true;
 return 0;
}
int kobox_linux_pressure_submit(uint64_t pages, uint64_t budget_ns)
{
 if (!pressure_started || !pages || pages > 1024 || !budget_ns || budget_ns > 100 * NSEC_PER_MSEC)
  return -EINVAL;
 if (atomic_cmpxchg(&pressure_state, 0, 1)) return -EBUSY;
 pressure_result = (struct kobox_linux_pressure_result){.requested_pages = pages};
 pressure_deadline = ktime_get_ns() + budget_ns;
 pressure_reporting_wait = false;
 queue_delayed_work(system_unbound_wq, &pressure_work, 0);
 return 0;
}
int kobox_linux_pressure_collect(struct kobox_linux_pressure_result *result)
{
 if (atomic_read_acquire(&pressure_state) != 2) return -EAGAIN;
 *result = pressure_result;
 atomic_set(&pressure_state, 0);
 return 0;
}
void kobox_linux_pressure_stop(void)
{
 if (!pressure_started) return;
 cancel_delayed_work_sync(&pressure_work);
 atomic_set(&pressure_state, 0);
 pressure_started = false;
}
