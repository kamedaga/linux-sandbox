// SPDX-License-Identifier: GPL-2.0-only
/* i915's internal WC vmaps must have real native WC backing. Keep a lease
 * until the original vunmap has revoked the last driver-owned CPU alias. */
#include "../memory/cache_lease.h"
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/panic.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/memtype.h>

struct i915_cache_alias {
    struct i915_cache_alias *next;
    void *address;
    struct kobox_cache_lease *lease;
};
static struct i915_cache_alias *cache_aliases;
static DEFINE_MUTEX(cache_alias_lock);

extern void *__real_vmap(struct page **pages, unsigned int count,
                         unsigned long flags, pgprot_t protection);
extern void __real_vunmap(const void *address);
void *__wrap_vmap(struct page **pages, unsigned int count,
                  unsigned long flags, pgprot_t protection);
void __wrap_vunmap(const void *address);

static int cache_module_get(void *owner)
{
    (void)owner;
    return try_module_get(THIS_MODULE) ? 0 : -ENODEV;
}
static void cache_module_put(void *owner)
{
    (void)owner;
    module_put(THIS_MODULE);
}
static const struct kobox_cache_owner_ops cache_module_ops = {
    .get = cache_module_get, .put = cache_module_put,
};

static void retain_alias(struct i915_cache_alias *alias)
{
    mutex_lock(&cache_alias_lock);
    alias->next = cache_aliases;
    cache_aliases = alias;
    mutex_unlock(&cache_alias_lock);
}

static int destroy_alias(struct i915_cache_alias *alias)
{
    int result = kobox_cache_lease_restore_wb(alias->lease);

    if (!result)
        result = kobox_cache_lease_destroy(&alias->lease);
    if (!result)
        kfree(alias);
    return result;
}

void *__wrap_vmap(struct page **pages, unsigned int count,
                  unsigned long flags, pgprot_t protection)
{
    struct i915_cache_alias *alias;
    unsigned int index;
    int result;

    if ((pgprot_val(protection) & _PAGE_CACHE_MASK) !=
        cachemode2protval(_PAGE_CACHE_MODE_WC))
        return __real_vmap(pages, count, flags, protection);
    /* The vmap caller retains the whole allocation. Ownership-taking
     * VM_MAP_PUT_PAGES has a different vfree contract and is not admitted. */
    if (!pages || !count || count > 65536 || flags & VM_MAP_PUT_PAGES)
        return NULL;
    for (index = 0; index < count; ++index)
        if (!pages[index])
            return NULL;
    alias = kzalloc(sizeof(*alias), GFP_KERNEL);
    if (!alias)
        return NULL;
    result = kobox_cache_lease_create_borrowed(pages, count, &cache_module_ops,
                                     alias, &alias->lease);
    if (result) {
        kfree(alias);
        return NULL;
    }
    result = kobox_cache_lease_acquire(alias->lease, KOBOX_LINUX_MEMORY_CACHE_WC);
    if (!result)
        alias->address = __real_vmap(pages, count, flags, protection);
    if (!alias->address) {
        if (destroy_alias(alias))
            panic("i915 borrowed vmap cache rollback failed before publication");
        return NULL;
    }
    retain_alias(alias);
    return alias->address;
}

void __wrap_vunmap(const void *address)
{
    struct i915_cache_alias **link, *alias = NULL;

    mutex_lock(&cache_alias_lock);
    for (link = &cache_aliases; *link; link = &(*link)->next)
        if ((*link)->address == address && address) {
            alias = *link;
            *link = alias->next;
            break;
        }
    mutex_unlock(&cache_alias_lock);
    __real_vunmap(address);
    if (alias) {
        alias->address = NULL;
        if (destroy_alias(alias))
            panic("i915 borrowed vmap cache restore failed before allocation release");
    }
}

