// SPDX-License-Identifier: GPL-2.0-only
/* Linked into i915 by the owned build before modpost. Upstream source stays
 * untouched; private types and functions come from its actual headers. */
#include "i915_mapping.h"
#include "../memory/cache_lease.h"
#include "../../drivers/gpu/drm/i915/i915_drv.h"
#include "../../drivers/gpu/drm/i915/gem/i915_gem_object.h"
#include "../../drivers/gpu/drm/i915/i915_scatterlist.h"
#include <drm/drm_file.h>
#include <drm/drm_prime.h>
#include <drm/drm_vma_manager.h>
#include <linux/dma-resv.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

struct mapped_bo {
    struct list_head link;
    struct drm_i915_gem_object *object;
    struct page **pages;
    size_t count, users;
    unsigned int cache;
    struct kobox_cache_lease *lease;
};
static LIST_HEAD(mapped_bos);
static DEFINE_MUTEX(mapped_bos_lock);

static int pin_owner(void *context)
{
    struct mapped_bo *bo=context;
    int error;
    if(!try_module_get(THIS_MODULE)) return -ENODEV;
    i915_gem_object_get(bo->object);
    i915_gem_object_lock(bo->object,NULL);
    error=i915_gem_object_pin_pages(bo->object);
    i915_gem_object_unlock(bo->object);
    if(error) {
        i915_gem_object_put(bo->object);
        module_put(THIS_MODULE);
    }
    return error;
}
static void unpin_owner(void *context)
{
    struct mapped_bo *bo=context;
    i915_gem_object_lock(bo->object,NULL);
    i915_gem_object_unpin_pages(bo->object);
    i915_gem_object_unlock(bo->object);
    i915_gem_object_put(bo->object);
    module_put(THIS_MODULE);
}
static const struct kobox_cache_owner_ops cache_owner={.get=pin_owner,.put=unpin_owner};

static int restore_bo(struct mapped_bo *bo)
{
    long waited;
    int error;
    /* A CPU PAT transition is not CPU ownership transfer. Wait only to make
     * the global cache-policy change safe; DMA_BUF SYNC stays independent. */
    waited=dma_resv_wait_timeout(bo->object->base.resv,DMA_RESV_USAGE_BOOKKEEP,
        true,MAX_SCHEDULE_TIMEOUT);
    if(waited<=0) return waited<0 ? (int)waited : -EBUSY;
    error=kobox_cache_lease_restore_wb(bo->lease);
    if(error) return error;
    return kobox_cache_lease_destroy(&bo->lease);
}
static void free_bo(struct mapped_bo *bo)
{
    list_del(&bo->link);
    kvfree(bo->pages);
    kfree(bo);
}
static int quiesce(void)
{
    struct mapped_bo *bo,*next;
    int first=0;
    mutex_lock(&mapped_bos_lock);
    list_for_each_entry_safe(bo,next,&mapped_bos,link) {
        int error=bo->users ? -EBUSY : restore_bo(bo);
        if(!error) free_bo(bo);
        else if(!first) first=error;
    }
    mutex_unlock(&mapped_bos_lock);
    return first;
}

static int validate_offset(struct drm_file *file,struct drm_i915_gem_object *object,
    u64 offset,u32 kind)
{
    struct drm_vma_offset_node *node;
    struct i915_mmap_offset *mmo;
    struct drm_device *device=object->base.dev;
    int error=-EACCES;
    if(!offset || offset & (PAGE_SIZE-1)) return -EINVAL;
    rcu_read_lock();
    drm_vma_offset_lock_lookup(device->vma_offset_manager);
    node=drm_vma_offset_lookup_locked(device->vma_offset_manager,
        offset>>PAGE_SHIFT,object->base.size>>PAGE_SHIFT);
    if(node && !node->driver_private && drm_vma_node_is_allowed(node,file) &&
        drm_vma_node_offset_addr(node)==offset && drm_vma_node_size(node)==object->base.size>>PAGE_SHIFT) {
        mmo=container_of(node,struct i915_mmap_offset,vma_node);
        if(mmo->obj==object && mmo->mmap_type==
            (kind==KOBOX_I915_MAP_WB ? I915_MMAP_TYPE_WB : I915_MMAP_TYPE_WC)) error=0;
    }
    drm_vma_offset_unlock_lookup(device->vma_offset_manager);
    rcu_read_unlock();
    return error;
}

static int create_bo(struct drm_i915_gem_object *object,unsigned int cache,
    struct mapped_bo **out)
{
    struct mapped_bo *bo;
    struct sgt_iter iter;
    struct page *page;
    size_t index=0;
    long waited;
    int error;
    bo=kzalloc(sizeof(*bo),GFP_KERNEL);
    if(!bo) return -ENOMEM;
    bo->object=object;bo->count=object->base.size>>PAGE_SHIFT;bo->cache=cache;
    bo->pages=kvmalloc_array(bo->count,sizeof(*bo->pages),GFP_KERNEL);
    if(!bo->pages) {kfree(bo);return -ENOMEM;}
    waited=dma_resv_wait_timeout(object->base.resv,DMA_RESV_USAGE_BOOKKEEP,true,MAX_SCHEDULE_TIMEOUT);
    if(waited<=0) {error=waited<0?(int)waited:-EBUSY;goto fail;}
    i915_gem_object_lock(object,NULL);
    /* Do not silently retag a retained i915 vmap with a different PAT type. */
    if(object->mm.mapping &&
        (cache==KOBOX_LINUX_MEMORY_CACHE_WC ||
         page_unmask_bits(object->mm.mapping)!=I915_MAP_WB)) {error=-EBUSY;goto unlock;}
    error=i915_gem_object_pin_pages(object);
    if(error) goto unlock;
    for_each_sgt_page(page,iter,object->mm.pages) {
        if(index==bo->count) {error=-EUCLEAN;break;}
        bo->pages[index++]=page;
    }
    if(!error && index!=bo->count) error=-EUCLEAN;
    i915_gem_object_unlock(object);
    if(!error) error=kobox_cache_lease_create(bo->pages,bo->count,&cache_owner,bo,&bo->lease);
    i915_gem_object_lock(object,NULL);
    i915_gem_object_unpin_pages(object);
    i915_gem_object_unlock(object);
    if(error) goto fail;
    /* Record before acquire: failed restoration must remain discoverable by
     * quiesce, holding module/BO/page refs instead of returning pages to RAM. */
    list_add_tail(&bo->link,&mapped_bos);
    error=kobox_cache_lease_acquire(bo->lease,cache);
    if(error) {
        if(!kobox_cache_lease_restore_wb(bo->lease) && !kobox_cache_lease_destroy(&bo->lease))
            free_bo(bo);
        return error;
    }
    *out=bo;
    return 0;
unlock:
    i915_gem_object_unlock(object);
fail:
    kvfree(bo->pages);kfree(bo);return error;
}

static int acquire(struct file *linux_file,u32 handle,u64 offset,u32 kind,u32 rights,
    u64 *pages,size_t capacity,struct kobox_linux_drm_map_pages *result,void **cookie)
{
    struct drm_file *file;
    struct drm_gem_object *base;
    struct drm_i915_gem_object *object;
    struct mapped_bo *bo,*found=NULL;
    unsigned int cache;
    size_t i;
    int error=0;
    if(!linux_file || !handle || !pages || !result || !cookie || *cookie ||
        !rights || rights & ~3u || kind>KOBOX_I915_MAP_WC) return -EINVAL;
    file=linux_file->private_data;
    if(!file || strcmp(file->minor->dev->driver->name,"i915")) return -ENODEV;
    if(!(linux_file->f_mode & FMODE_READ) || ((rights & 2) && !(linux_file->f_mode & FMODE_WRITE))) return -EACCES;
    base=drm_gem_object_lookup(file,handle);
    if(!base) return -ENOENT;
    object=to_intel_bo(base);
    if(!i915_gem_object_is_shmem(object) || drm_gem_is_imported(base) ||
        !i915_gem_object_has_struct_page(object)) {error=-EOPNOTSUPP;goto put;}
    if(!base->size || base->size & (PAGE_SIZE-1) || (base->size>>PAGE_SHIFT)>65536 ||
        (base->size>>PAGE_SHIFT)>capacity) {error=-ENOSPC;goto put;}
    if((rights & 2) && i915_gem_object_is_readonly(object)) {error=-EACCES;goto put;}
    if(kind) {
        error=validate_offset(file,object,offset,kind);
        if(error) goto put;
    } else if(offset) {error=-EINVAL;goto put;}
    mutex_lock(&mapped_bos_lock);
    list_for_each_entry(bo,&mapped_bos,link) if(bo->object==object) {found=bo;break;}
    cache=kind==KOBOX_I915_MAP_WC ? KOBOX_LINUX_MEMORY_CACHE_WC : KOBOX_LINUX_MEMORY_CACHE_WB;
    if(found) {
        if(!found->users) {error=-EBUSY;goto unlock;}
        if(!kind) cache=found->cache;
        if(cache!=found->cache) {error=-EBUSY;goto unlock;}
    } else {
        error=create_bo(object,cache,&found);
        if(error) goto unlock;
    }
    if(rights & 2) {
        i915_gem_object_lock(object,NULL);object->mm.dirty=true;i915_gem_object_unlock(object);
    }
    for(i=0;i<found->count;++i) pages[i]=page_to_pfn(found->pages[i]);
    ++found->users;*cookie=found;
    *result=(struct kobox_linux_drm_map_pages){.length=base->size,.page_count=found->count,
        .backing_kind=KOBOX_DRM_BACKING_RAM,.cache_policy=cache==KOBOX_LINUX_MEMORY_CACHE_WC ? KOBOX_DRM_CACHE_WC : KOBOX_DRM_CACHE_WB};
unlock:
    mutex_unlock(&mapped_bos_lock);
put:
    drm_gem_object_put(base);
    return error;
}
static int release(void **cookie)
{
    struct mapped_bo *bo;
    int error=0;
    if(!cookie || !*cookie) return -EINVAL;
    mutex_lock(&mapped_bos_lock);
    bo=*cookie;
    if(bo->users>1) --bo->users;
    else {
        error=restore_bo(bo);
        if(!error) free_bo(bo);
    }
    if(!error) *cookie=NULL;
    mutex_unlock(&mapped_bos_lock);
    return error;
}
const struct kobox_i915_mapping_api kobox_i915_mapping_api={
    .size=sizeof(struct kobox_i915_mapping_api),.acquire=acquire,.release=release,.quiesce=quiesce};
EXPORT_SYMBOL_GPL(kobox_i915_mapping_api);
