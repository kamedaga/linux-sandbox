/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_MEMORY_CACHE_LEASE_H
#define KOBOX_MEMORY_CACHE_LEASE_H
#include "host.h"
#ifndef __KERNEL__
#include <stdbool.h>
#endif
struct page;
struct kobox_cache_lease;
struct kobox_cache_owner_ops {
    /* Retain BO/page-pin/module ownership before any policy changes. Called
     * in sleepable context, never under the journal or alias spinlocks. */
    int (*get)(void *owner);
    void (*put)(void *owner);
};
enum kobox_cache_lease_state {
    KOBOX_CACHE_PREPARED, KOBOX_CACHE_ACTIVE, KOBOX_CACHE_RETAINED,
    KOBOX_CACHE_RESTORED,
};
struct kobox_cache_lease_status {
    enum kobox_cache_lease_state state;
    unsigned int cache;
    int native_error, cpa_error, restore_error;
};
/* Caller excludes CPU/GPU accesses and competing BO maps during transitions.
 * Pages must be pinned normal RAM, initially WB. Each PFN admits one owner;
 * the BO layer shares that owner's lease for same-policy client mappings.
 * No external PAGE_VIEW may be published until acquire succeeds. */
int kobox_cache_lease_create(struct page *const *pages, size_t count,
    const struct kobox_cache_owner_ops *ops, void *owner,
    struct kobox_cache_lease **out);
/* For vmap callers which retain their WHOLE allocation until unmap returns.
 * Noncompound higher-order tail pages must never receive individual refs.
 * Caller must restore WB before releasing that allocation; if restoration
 * fails it must fail-stop before returning to the allocation's free path.
 * A borrowed lease cannot become an orphan after its caller unwinds. */
int kobox_cache_lease_create_borrowed(struct page *const *pages, size_t count,
    const struct kobox_cache_owner_ops *ops, void *owner,
    struct kobox_cache_lease **out);
int kobox_cache_lease_acquire(struct kobox_cache_lease *lease, unsigned int cache);
/* External views must be revoked and GPU access quiesced before restore.
 * Failure retains page/owner refs and memtype reservations for explicit retry. */
int kobox_cache_lease_restore_wb(struct kobox_cache_lease *lease);
int kobox_cache_lease_destroy(struct kobox_cache_lease **lease);
int kobox_cache_lease_status(struct kobox_cache_lease *lease,
    struct kobox_cache_lease_status *out);

/* Internal early_boot bridge. PFNs are hosted RAM indices, never native PA.
 * A hook returns true only for a wholly-owned contiguous span. */
unsigned long kobox_cache_lease_boundary(unsigned long pfn, unsigned long end);
bool kobox_cache_lease_shadow(unsigned long pfn, size_t pages,
    unsigned int cache, unsigned int protection, int *status);
bool kobox_cache_lease_flush_ack(void);
int kobox_memory_cache_inspect(unsigned long pfn, unsigned int *cache,
    unsigned int *protection);
int kobox_memory_cache_native(unsigned long pfn, size_t pages, unsigned int cache);
/* Registry publication must exclude an early_boot publisher which already
 * computed an unmanaged span. Order: operation mutex -> alias -> journal. */
unsigned long kobox_memory_cache_alias_lock(void);
void kobox_memory_cache_alias_unlock(unsigned long flags);
#endif
