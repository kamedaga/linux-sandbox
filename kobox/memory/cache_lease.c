/* SPDX-License-Identifier: GPL-2.0-only */
#include "cache_lease.h"
#include "reporting.h"
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <asm/memtype.h>
#include <asm/set_memory.h>

#define CACHE_LEASE_MAX_PAGES 65536U
struct cache_page {
    struct page *page;
    unsigned long pfn;
    unsigned int shadow_cache, protection;
    bool reserved;
};
struct kobox_cache_lease {
    struct kobox_cache_lease *next;
    struct cache_page *pages;
    size_t count;
    struct kobox_cache_owner_ops owner_ops;
    void *owner;
    struct kobox_cache_lease_status status;
    bool mirroring, native_complete, retain_page_refs;
};
/* Operations may sleep in CPA and wait for remote CPUs. The callbacks take
 * journal_lock only, never operation_lock, so an IPI cannot wait on its caller.
 * Lock order in early_boot: kernel_alias_lock -> journal_lock. */
static DEFINE_MUTEX(cache_operation_lock);
static DEFINE_RAW_SPINLOCK(cache_journal_lock);
static struct kobox_cache_lease *cache_leases;

static int cache_page_compare(const void *left, const void *right)
{
    const struct cache_page *a = left, *b = right;
    return a->pfn < b->pfn ? -1 : a->pfn != b->pfn;
}
static bool registered(struct kobox_cache_lease *lease)
{
    struct kobox_cache_lease *p;
    for (p = cache_leases; p; p = p->next)
        if (p == lease) return true;
    return false;
}
static size_t find_page(struct kobox_cache_lease *lease, unsigned long pfn)
{
    size_t lo = 0, hi = lease->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (lease->pages[mid].pfn < pfn) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
unsigned long kobox_cache_lease_boundary(unsigned long pfn, unsigned long end)
{
    struct kobox_cache_lease *lease;
    unsigned long flags;
    raw_spin_lock_irqsave(&cache_journal_lock, flags);
    for (lease = cache_leases; lease; lease = lease->next) {
        size_t i = find_page(lease, pfn);
        if (i == lease->count) continue;
        if (lease->pages[i].pfn > pfn) {
            end = min(end, lease->pages[i].pfn);
            continue;
        }
        while (i + 1 < lease->count &&
               lease->pages[i + 1].pfn == lease->pages[i].pfn + 1) ++i;
        end = min(end, lease->pages[i].pfn + 1);
    }
    raw_spin_unlock_irqrestore(&cache_journal_lock, flags);
    return end;
}
bool kobox_cache_lease_shadow(unsigned long pfn, size_t pages,
    unsigned int cache, unsigned int protection, int *status)
{
    struct kobox_cache_lease *lease;
    unsigned long flags;
    bool found = false;
    raw_spin_lock_irqsave(&cache_journal_lock, flags);
    for (lease = cache_leases; lease; lease = lease->next) {
        size_t i = find_page(lease, pfn), n;
        if (i == lease->count || lease->pages[i].pfn != pfn) continue;
        found = true;
        *status = -EBUSY;
        if (!pages || pages > lease->count - i) break;
        for (n = 0; n < pages; ++n) {
            struct cache_page *page = &lease->pages[i + n];
            if (page->pfn != pfn + n || protection != page->protection) break;
            if (lease->mirroring) {
                if (!lease->native_complete) break;
                /* A full CPA flush also visits runs not yet mirrored. Their
                 * previous shadow bits are valid while native policy is
                 * already uniform across the entire owned page set. */
                if (cache == page->shadow_cache) continue;
                if (lease->status.cache == KOBOX_LINUX_MEMORY_CACHE_WC) {
                    if (cache != KOBOX_LINUX_MEMORY_CACHE_UC_MINUS &&
                        cache != KOBOX_LINUX_MEMORY_CACHE_WC) break;
                } else if (cache != KOBOX_LINUX_MEMORY_CACHE_WB) break;
            } else if (cache != page->shadow_cache) break;
        }
        if (n == pages) {
            /* Native aliases already have one canonical policy. CPA changes
             * hosted shadow PTEs only; incidental global flushes must not
             * retag an in-flight or retained lease from stale shadow bits. */
            *status = 0;
            if (lease->mirroring)
                for (n = 0; n < pages; ++n)
                    lease->pages[i + n].shadow_cache = cache;
        }
        break;
    }
    raw_spin_unlock_irqrestore(&cache_journal_lock, flags);
    return found;
}
bool kobox_cache_lease_flush_ack(void)
{
    struct kobox_cache_lease *lease;
    unsigned long flags;
    bool complete = false;
    raw_spin_lock_irqsave(&cache_journal_lock, flags);
    for (lease = cache_leases; lease; lease = lease->next)
        if (lease->mirroring && lease->native_complete) {
            complete = true;
            break;
        }
    raw_spin_unlock_irqrestore(&cache_journal_lock, flags);
    return complete;
}
static size_t run_end(struct kobox_cache_lease *lease, size_t first)
{
    size_t end = first + 1;
    while (end < lease->count &&
           lease->pages[end].pfn == lease->pages[end - 1].pfn + 1) ++end;
    return end;
}
/* A callback can successfully transition a prefix before returning failure.
 * Always visit every target during recovery, including the failed span. */
static int native_policy(struct kobox_cache_lease *lease, unsigned int cache)
{
    size_t first;
    int first_error = 0;
    for (first = 0; first < lease->count; ) {
        size_t end = run_end(lease, first);
        int error = kobox_memory_cache_native(lease->pages[first].pfn,
                                              end - first, cache);
        if (error && !first_error) first_error = error;
        first = end;
    }
    return first_error;
}
static int mirror_policy(struct kobox_cache_lease *lease, unsigned int cache)
{
    unsigned long flags;
    size_t first, i;
    int first_error = 0;
    raw_spin_lock_irqsave(&cache_journal_lock, flags);
    lease->status.cache = cache;
    lease->native_complete = true;
    lease->mirroring = true;
    raw_spin_unlock_irqrestore(&cache_journal_lock, flags);
    for (first = 0; first < lease->count; ) {
        size_t end = run_end(lease, first);
        unsigned long address = (unsigned long)page_address(lease->pages[first].page);
        int error = cache == KOBOX_LINUX_MEMORY_CACHE_WC ?
            _set_memory_wc(address, (int)(end - first)) :
            _set_memory_wb(address, (int)(end - first));
        if (error && !first_error) first_error = error;
        first = end;
    }
    raw_spin_lock_irqsave(&cache_journal_lock, flags);
    lease->mirroring = false;
    /* CPA may partially update PTEs and then fail. Snapshot their actual
     * cache bits for incidental flush validation; structural large-page
     * splits and hardware A/D changes need not be reversed. */
    for (i = 0; i < lease->count; ++i) {
        unsigned int actual, protection;
        int error = kobox_memory_cache_inspect(lease->pages[i].pfn,
                                               &actual, &protection);
        if (error || protection != lease->pages[i].protection) {
            if (!first_error) first_error = error ? error : -EIO;
        } else {
            lease->pages[i].shadow_cache = actual;
            if (actual != cache && !first_error) first_error = -EIO;
        }
    }
    raw_spin_unlock_irqrestore(&cache_journal_lock, flags);
    return first_error;
}
static int restore_locked(struct kobox_cache_lease *lease)
{
    size_t i;
    int error;
    unsigned long flags;
    raw_spin_lock_irqsave(&cache_journal_lock, flags);
    lease->native_complete = false;
    raw_spin_unlock_irqrestore(&cache_journal_lock, flags);
    /* Explicit WB is mandatory even if the hosted PTE already says WB and
     * CPA would skip its flush hook after an earlier partial native failure. */
    error = native_policy(lease, KOBOX_LINUX_MEMORY_CACHE_WB);
    if (!error) error = mirror_policy(lease, KOBOX_LINUX_MEMORY_CACHE_WB);
    if (!error) {
        for (i = 0; i < lease->count; ++i) {
            struct cache_page *page = &lease->pages[i];
            if (!page->reserved) continue;
            error = memtype_free((u64)page->pfn << PAGE_SHIFT,
                                 ((u64)page->pfn + 1) << PAGE_SHIFT);
            if (error) break;
            page->reserved = false;
        }
    }
    lease->status.restore_error = error;
    lease->status.state = error ? KOBOX_CACHE_RETAINED : KOBOX_CACHE_RESTORED;
    return error ? -EIO : 0;
}
static int create_lease(struct page *const *pages, size_t count,
    const struct kobox_cache_owner_ops *ops, void *owner,
    struct kobox_cache_lease **out, bool retain_page_refs)
{
    struct kobox_cache_lease *lease, *other;
    unsigned long flags, alias_flags;
    size_t i;
    int error = 0;
    if (!pages || !count || count > CACHE_LEASE_MAX_PAGES || !ops ||
        !ops->get || !ops->put || !out || *out) return -EINVAL;
    lease = kzalloc(sizeof(*lease), GFP_KERNEL);
    if (!lease) return -ENOMEM;
    lease->pages = kvmalloc_array(count, sizeof(*lease->pages), GFP_KERNEL);
    if (!lease->pages) { kfree(lease); return -ENOMEM; }
    memset(lease->pages, 0, count * sizeof(*lease->pages));
    lease->count = count; lease->owner_ops = *ops; lease->owner = owner;
    lease->retain_page_refs = retain_page_refs;
    mutex_lock(&cache_operation_lock);
    for (i = 0; i < count; ++i) {
        struct cache_page *p = &lease->pages[i];
        if (!pages[i] || PageHighMem(pages[i]) || PageReserved(pages[i]) ||
            is_zone_device_page(pages[i]) ||
            (retain_page_refs && page_count(pages[i]) <= 0)) { error = -EINVAL; break; }
        kobox_linux_resident_assert_page(pages[i]);
        p->page = pages[i]; p->pfn = page_to_pfn(pages[i]);
        error = kobox_memory_cache_inspect(p->pfn, &p->shadow_cache, &p->protection);
        if (error) break;
        if (p->shadow_cache != KOBOX_LINUX_MEMORY_CACHE_WB ||
            p->protection != (KOBOX_LINUX_MEMORY_READ | KOBOX_LINUX_MEMORY_WRITE)) {
            error = -EBUSY; break;
        }
    }
    if (error) goto unlock_fail;
    sort(lease->pages, count, sizeof(*lease->pages), cache_page_compare, NULL);
    for (i = 0; i < count; ++i) {
        if (i && lease->pages[i].pfn == lease->pages[i - 1].pfn) {
            error = -EINVAL; goto unlock_fail;
        }
        for (other = cache_leases; other; other = other->next) {
            size_t n = find_page(other, lease->pages[i].pfn);
            if (n < other->count && other->pages[n].pfn == lease->pages[i].pfn) {
                error = -EBUSY; goto unlock_fail;
            }
        }
    }
    error = ops->get(owner);
    if (error) goto unlock_fail;
    if (retain_page_refs)
        for (i = 0; i < count; ++i) get_page(lease->pages[i].page);
    lease->status.state = KOBOX_CACHE_PREPARED;
    lease->status.cache = KOBOX_LINUX_MEMORY_CACHE_WB;
    alias_flags = kobox_memory_cache_alias_lock();
    /* Recheck after any old unmanaged publication has completed. An
     * overlapping policy writer must not be mistaken for fresh WB RAM. */
    for (i = 0; i < count; ++i) {
        unsigned int cache, protection;
        error = kobox_memory_cache_inspect(lease->pages[i].pfn, &cache, &protection);
        if (error || cache != KOBOX_LINUX_MEMORY_CACHE_WB ||
            protection != lease->pages[i].protection) {
            if (!error) error = -EBUSY;
            break;
        }
    }
    if (error) {
        kobox_memory_cache_alias_unlock(alias_flags);
        mutex_unlock(&cache_operation_lock);
        if (retain_page_refs)
            for (i = 0; i < count; ++i) put_page(lease->pages[i].page);
        ops->put(owner);
        kvfree(lease->pages); kfree(lease);
        return error;
    }
    raw_spin_lock_irqsave(&cache_journal_lock, flags);
    lease->next = cache_leases; cache_leases = lease;
    raw_spin_unlock_irqrestore(&cache_journal_lock, flags);
    kobox_memory_cache_alias_unlock(alias_flags);
    *out = lease;
    mutex_unlock(&cache_operation_lock);
    return 0;
unlock_fail:
    mutex_unlock(&cache_operation_lock);
    kvfree(lease->pages); kfree(lease); return error;
}
int kobox_cache_lease_create(struct page *const *pages, size_t count,
    const struct kobox_cache_owner_ops *ops, void *owner,
    struct kobox_cache_lease **out)
{
    return create_lease(pages, count, ops, owner, out, true);
}
int kobox_cache_lease_create_borrowed(struct page *const *pages, size_t count,
    const struct kobox_cache_owner_ops *ops, void *owner,
    struct kobox_cache_lease **out)
{
    return create_lease(pages, count, ops, owner, out, false);
}
int kobox_cache_lease_acquire(struct kobox_cache_lease *lease, unsigned int cache)
{
    unsigned long flags;
    size_t i;
    int error = 0;
    if (cache != KOBOX_LINUX_MEMORY_CACHE_WB && cache != KOBOX_LINUX_MEMORY_CACHE_WC)
        return -EINVAL;
    mutex_lock(&cache_operation_lock);
    if (!registered(lease) || lease->status.state != KOBOX_CACHE_PREPARED) {
        error = -EINVAL; goto out;
    }
    /* Establish native WB explicitly; hosted PAT bits alone are not proof. */
    error = native_policy(lease, KOBOX_LINUX_MEMORY_CACHE_WB);
    if (error) { lease->status.native_error = error; goto rollback; }
    if (cache == KOBOX_LINUX_MEMORY_CACHE_WC) {
        for (i = 0; i < lease->count; ++i) {
            struct cache_page *page = &lease->pages[i];
            error = memtype_reserve((u64)page->pfn << PAGE_SHIFT,
                ((u64)page->pfn + 1) << PAGE_SHIFT, _PAGE_CACHE_MODE_WC, NULL);
            if (error) { lease->status.cpa_error = error; goto rollback; }
            page->reserved = true;
        }
        error = native_policy(lease, KOBOX_LINUX_MEMORY_CACHE_UC_MINUS);
        if (!error) error = native_policy(lease, KOBOX_LINUX_MEMORY_CACHE_WC);
        if (error) { lease->status.native_error = error; goto rollback; }
    }
    error = mirror_policy(lease, cache);
    if (error) { lease->status.cpa_error = error; goto rollback; }
    raw_spin_lock_irqsave(&cache_journal_lock, flags);
    lease->status.state = KOBOX_CACHE_ACTIVE;
    raw_spin_unlock_irqrestore(&cache_journal_lock, flags);
    goto out;
rollback:
    if (restore_locked(lease)) error = -EIO;
    else if (error > 0) error = -EIO;
out:
    mutex_unlock(&cache_operation_lock);
    return error;
}
int kobox_cache_lease_restore_wb(struct kobox_cache_lease *lease)
{
    int error;
    mutex_lock(&cache_operation_lock);
    if (!registered(lease)) error = -EINVAL;
    else if (lease->status.state == KOBOX_CACHE_RESTORED) error = 0;
    else error = restore_locked(lease);
    mutex_unlock(&cache_operation_lock);
    return error;
}
int kobox_cache_lease_destroy(struct kobox_cache_lease **owner)
{
    struct kobox_cache_lease *lease, **link;
    unsigned long flags, alias_flags;
    size_t i;
    int error = 0;
    if (!owner || !*owner) return -EINVAL;
    lease = *owner;
    mutex_lock(&cache_operation_lock);
    if (!registered(lease)) error = -EINVAL;
    else if (lease->status.state != KOBOX_CACHE_RESTORED) error = -EBUSY;
    if (error) { mutex_unlock(&cache_operation_lock); return error; }
    alias_flags = kobox_memory_cache_alias_lock();
    raw_spin_lock_irqsave(&cache_journal_lock, flags);
    for (link = &cache_leases; *link != lease; link = &(*link)->next) {}
    *link = lease->next;
    raw_spin_unlock_irqrestore(&cache_journal_lock, flags);
    kobox_memory_cache_alias_unlock(alias_flags);
    *owner = NULL;
    mutex_unlock(&cache_operation_lock);
    if (lease->retain_page_refs)
        for (i = 0; i < lease->count; ++i) put_page(lease->pages[i].page);
    lease->owner_ops.put(lease->owner);
    kvfree(lease->pages); kfree(lease);
    return 0;
}
int kobox_cache_lease_status(struct kobox_cache_lease *lease,
    struct kobox_cache_lease_status *out)
{
    int error = 0;
    if (!out) return -EINVAL;
    mutex_lock(&cache_operation_lock);
    if (!registered(lease)) error = -EINVAL;
    else *out = lease->status;
    mutex_unlock(&cache_operation_lock);
    return error;
}
EXPORT_SYMBOL_GPL(kobox_cache_lease_create);
EXPORT_SYMBOL_GPL(kobox_cache_lease_create_borrowed);
EXPORT_SYMBOL_GPL(kobox_cache_lease_acquire);
EXPORT_SYMBOL_GPL(kobox_cache_lease_restore_wb);
EXPORT_SYMBOL_GPL(kobox_cache_lease_destroy);
EXPORT_SYMBOL_GPL(kobox_cache_lease_status);
