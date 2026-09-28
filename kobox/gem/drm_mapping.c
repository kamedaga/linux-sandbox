// SPDX-License-Identifier: GPL-2.0-only

#include "../boot/drm_file.h"

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#if IS_ENABLED(CONFIG_DRM_GEM_SHMEM_HELPER)
#include <drm/drm_gem_shmem_helper.h>
#endif
#include <drm/drm_vma_manager.h>
#if IS_ENABLED(CONFIG_DRM_TTM)
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/ttm/ttm_resource.h>
#endif

struct kobox_drm_mapping {
	struct drm_gem_object *object;
#if IS_ENABLED(CONFIG_DRM_TTM)
	struct ttm_buffer_object *ttm;
	bool io_reserved;
#endif
};

static int gem_size(struct file *file, u32 handle, u64 *size)
{
	struct drm_file *drm_file;
	struct drm_gem_object *object;

	if (!file || !handle || !size)
		return -EINVAL;
	drm_file = file->private_data;
	if (!drm_file)
		return -ENODEV;
	object = drm_gem_object_lookup(drm_file, handle);
	if (!object)
		return -ENOENT;
	*size = object->size;
	drm_gem_object_put(object);
	return *size ? 0 : -EUCLEAN;
}

#if IS_ENABLED(CONFIG_DRM_TTM)
static bool vram_pages_in_one_pci_bar(struct drm_gem_object *object,
				     struct ttm_buffer_object *ttm,
				     size_t page_count)
{
	struct pci_dev *pci;
	unsigned int bar;

	if (!object->dev || !object->dev->dev ||
	    !dev_is_pci(object->dev->dev) ||
	    !ttm->bdev->funcs->io_mem_pfn)
		return false;
	pci = to_pci_dev(object->dev->dev);
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		resource_size_t start = pci_resource_start(pci, bar);
		resource_size_t length = pci_resource_len(pci, bar);
		size_t index;

		if (!(pci_resource_flags(pci, bar) & IORESOURCE_MEM) ||
		    !length || length < PAGE_SIZE)
			continue;
		for (index = 0; index < page_count; index++) {
			u64 pfn = ttm->bdev->funcs->io_mem_pfn(ttm, index);
			u64 physical;

			if (pfn > U64_MAX >> PAGE_SHIFT)
				break;
			physical = pfn << PAGE_SHIFT;
			if (physical < start ||
			    physical - start > length - PAGE_SIZE)
				break;
		}
		if (index == page_count)
			return true;
	}
	return false;
}

static int map_amdgpu_pages(struct drm_gem_object *object,
				u32 mapping_rights, u64 *page_indices,
				size_t page_capacity,
				struct kobox_linux_drm_map_pages *result,
				void **private_mapping)
{
	struct ttm_buffer_object *ttm = container_of(object,
		struct ttm_buffer_object, base);
	struct kobox_drm_mapping *mapping;
	struct ttm_operation_ctx context = {0};
	size_t page_count, index;
	u32 backing = KOBOX_DRM_BACKING_RAM, cache = KOBOX_DRM_CACHE_WB;
	int error;

	(void)mapping_rights;
	if (!object->size || object->size & (PAGE_SIZE - 1))
		return -EINVAL;
	page_count = object->size >> PAGE_SHIFT;
	if (page_count > page_capacity)
		return -ENOSPC;
	if (drm_gem_is_imported(object))
		return -EACCES;
	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return -ENOMEM;
	error = ttm_bo_reserve(ttm, false, false, NULL);
	if (error)
		goto free_mapping;
	if (ttm->resource && ttm->resource->mem_type == TTM_PL_VRAM &&
	    !vram_pages_in_one_pci_bar(object, ttm, page_count)) {
		struct ttm_place place = { .mem_type = TTM_PL_TT };
		struct ttm_placement placement = {
			.num_placement = 1,
			.placement = &place,
		};

		/* The normal AMDGPU CPU-fault path migrates non-visible VRAM before
		 * mapping it. This capability-backed path has no Linux VMA fault, so
		 * make the same placement decision before pinning. Never publish a
		 * physical PFN outside the delegated PCI BAR as an MMIO view. */
		if (ttm->pin_count) {
			error = -EBUSY;
			goto unreserve;
		}
		error = ttm_bo_validate(ttm, &placement, &context);
		if (error)
			goto unreserve;
		if (!ttm->resource || ttm->resource->mem_type != TTM_PL_TT) {
			error = -EUCLEAN;
			goto unreserve;
		}
		pr_info("kobox-drm: CPU map relocated non-visible VRAM to GTT bytes=%zu\n",
			object->size);
	}
	/* A GPU move may complete asynchronously. Do not expose the CPU pages
	 * until its fence has signalled. */
	error = ttm_bo_wait_ctx(ttm, &context);
	if (error)
		goto unreserve;
	if (ttm->resource && ttm->resource->mem_type == TTM_PL_VRAM) {
		struct ttm_bus_placement *bus = &ttm->resource->bus;

		/* Use the driver's resource resolver, including its non-contiguous
		 * VRAM cursor. A linear resource->start approximation is invalid.
		 * The host subsequently proves every page belongs to its PCI BAR;
		 * inaccessible VRAM is rejected rather than exported as RAM. */
		if (!ttm->bdev->funcs->io_mem_reserve ||
		    !ttm->bdev->funcs->io_mem_pfn) {
			error = -EOPNOTSUPP;
			goto unreserve;
		}
		if (!bus->offset && !bus->addr) {
			bus->is_iomem = false;
			error = ttm->bdev->funcs->io_mem_reserve(ttm->bdev,
							   ttm->resource);
			if (error)
				goto unreserve;
			mapping->io_reserved = true;
		}
		if (!bus->is_iomem || (bus->caching != ttm_write_combined &&
				      bus->caching != ttm_uncached)) {
			error = -EOPNOTSUPP;
			goto unreserve;
		}
		backing = KOBOX_DRM_BACKING_DEVICE;
		cache = bus->caching == ttm_write_combined ?
			KOBOX_DRM_CACHE_WC : KOBOX_DRM_CACHE_UC;
		ttm_bo_pin(ttm);
		for (index = 0; index < page_count; index++)
			page_indices[index] = ttm->bdev->funcs->io_mem_pfn(ttm,
									index);
		goto ready;
	}
	/* TTM's WC allocation has already completed set_pages_array_wc(), which
	 * makes the hosted RAM cache transition authoritative for every alias.
	 * A later native page view inherits that physical policy. Do not accept
	 * ttm_uncached here: x86 set_pages_array_uc() uses UC-minus, while this
	 * mapping protocol's UC value denotes strong UC for device BARs. */
	if (!ttm->resource || ttm->resource->mem_type != TTM_PL_TT ||
	    !ttm->ttm || (ttm->ttm->caching != ttm_cached &&
			  ttm->ttm->caching != ttm_write_combined) ||
	    ttm->ttm->num_pages < page_count) {
		error = -EOPNOTSUPP;
		goto unreserve;
	}
	/* The exported BO entry point also restores swapped TTs and updates TTM's
	 * LRU bookkeeping; ttm_tt_populate itself is private to the TTM module. */
	error = ttm_bo_populate(ttm, &context);
	if (error)
		goto unreserve;
	if (!ttm->ttm->pages) {
		error = -EUCLEAN;
		goto unreserve;
	}
	cache = ttm->ttm->caching == ttm_write_combined ?
		KOBOX_DRM_CACHE_WC : KOBOX_DRM_CACHE_WB;
	ttm_bo_pin(ttm);
	for (index = 0; index < page_count; index++) {
		if (!ttm->ttm->pages[index]) {
			error = -EUCLEAN;
			goto unpin;
		}
		page_indices[index] = page_to_phys(ttm->ttm->pages[index]) >> PAGE_SHIFT;
	}
ready:
	ttm_bo_unreserve(ttm);
	mapping->object = object;
	mapping->ttm = ttm;
	*result = (struct kobox_linux_drm_map_pages) {
		.length = object->size,
		.cache_policy = cache,
		.backing_kind = backing,
		.page_count = page_count,
	};
	*private_mapping = mapping;
	return 0;

unpin:
	ttm_bo_unpin(ttm);
unreserve:
	pr_warn("kobox-drm: CPU mapping refused status=%d bytes=%zu mem_type=%u cache=%u device=%u\n",
		error, object->size, ttm->resource ? ttm->resource->mem_type : ~0U,
		ttm->resource && ttm->resource->mem_type == TTM_PL_VRAM ?
		(unsigned int)ttm->resource->bus.caching :
		(ttm->ttm ? (unsigned int)ttm->ttm->caching : ~0U), backing);
	if (mapping->io_reserved) {
		if (ttm->bdev->funcs->io_mem_free)
			ttm->bdev->funcs->io_mem_free(ttm->bdev, ttm->resource);
		ttm->resource->bus.offset = 0;
		ttm->resource->bus.addr = NULL;
	}
	ttm_bo_unreserve(ttm);
free_mapping:
	kfree(mapping);
	return error;
}
#endif

static int map_system_memory_pages(struct file *file, u32 handle, u32 mapping_rights,
			   u64 *page_indices, size_t page_capacity,
			   struct kobox_linux_drm_map_pages *result,
			   void **private_mapping)
{
#if IS_ENABLED(CONFIG_DRM_GEM_SHMEM_HELPER)
	struct kobox_drm_mapping *mapping;
	struct drm_gem_shmem_object *shmem;
#endif
	struct drm_gem_object *object;
	struct drm_file *drm_file;
	struct drm_device *device;
#if IS_ENABLED(CONFIG_DRM_GEM_SHMEM_HELPER)
	u64 offset;
#endif
#if IS_ENABLED(CONFIG_DRM_GEM_SHMEM_HELPER)
	size_t page_count, index;
#endif
	int error;

	if (!file || !handle || !mapping_rights || (mapping_rights & ~3U) ||
	    !page_indices || !page_capacity || !result || !private_mapping ||
	    *private_mapping)
		return -EINVAL;
	drm_file = file->private_data;
	if (!drm_file || !drm_file->minor || !drm_file->minor->dev)
		return -ENODEV;
	device = drm_file->minor->dev;
	if (!device->driver ||
	    (strcmp(device->driver->name, "virtio_gpu") &&
	     strcmp(device->driver->name, "amdgpu")))
		return -ENODEV;
	object = drm_gem_object_lookup(drm_file, handle);
	if (!object)
		return -ENOENT;
	if (!drm_vma_node_is_allowed(&object->vma_node, drm_file)) {
		error = -EACCES;
		goto put_object;
	}
	if (!strcmp(device->driver->name, "amdgpu")) {
#if IS_ENABLED(CONFIG_DRM_TTM)
		error = map_amdgpu_pages(object, mapping_rights,
			page_indices, page_capacity, result, private_mapping);
		if (error)
			goto put_object;
		return 0;
#else
		error = -EOPNOTSUPP;
		goto put_object;
#endif
	}
#if IS_ENABLED(CONFIG_DRM_GEM_SHMEM_HELPER)
	if (!object->size || object->size & (PAGE_SIZE - 1)) {
		error = -EINVAL;
		goto put_object;
	}
	if (drm_gem_is_imported(object)) {
		error = -EACCES;
		goto put_object;
	}
	if (!object->funcs || object->funcs->vm_ops != &drm_gem_shmem_vm_ops) {
		error = -EBUSY;
		goto put_object;
	}
	shmem = to_drm_gem_shmem_obj(object);
	/* Pacha's RAM view is normal coherent memory; WC cannot be represented. */
	if (shmem->map_wc) {
		error = -EBUSY;
		goto put_object;
	}
	page_count = object->size >> PAGE_SHIFT;
	if (!page_count || page_count > page_capacity) {
		error = -ENOSPC;
		goto put_object;
	}
	/* This is exactly the operation behind VIRTGPU_MAP for the pinned driver. */
	error = drm_gem_dumb_map_offset(drm_file, device, handle, &offset);
	if (error)
		goto put_object;
	if (!offset || offset & (PAGE_SIZE - 1) ||
	    offset != drm_vma_node_offset_addr(&object->vma_node)) {
		error = -EUCLEAN;
		goto put_object;
	}
	if (!drm_vma_node_is_allowed(&object->vma_node, drm_file)) {
		error = -EACCES;
		goto put_object;
	}
	/* virtio_gpu_object_shmem_init() keeps this SGT and its page array for
	 * the object's lifetime.  The GEM reference held by this mapping is the
	 * lifetime pin; taking a second shmem pin would only re-enter the hosted
	 * interruptible reservation path. */
	if (!shmem->sgt || !shmem->pages ||
	    !refcount_read(&shmem->pages_use_count)) {
		error = -EUCLEAN;
		goto put_object;
	}
	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping) {
		error = -ENOMEM;
		goto put_object;
	}
	for (index = 0; index < page_count; index++) {
		if (!shmem->pages[index]) {
			error = -EUCLEAN;
			goto free_mapping;
		}
		page_indices[index] = page_to_phys(shmem->pages[index]) >> PAGE_SHIFT;
	}
	mapping->object = object;
	*result = (struct kobox_linux_drm_map_pages) {
		.length = object->size,
		.cache_policy = 0,
		.page_count = page_count,
	};
	*private_mapping = mapping;
	return 0;

free_mapping:
	kfree(mapping);
#else
	error = -EOPNOTSUPP;
#endif
put_object:
	drm_gem_object_put(object);
	return error;
}

static void release_system_memory_pages(void *private_mapping)
{
	struct kobox_drm_mapping *mapping = private_mapping;
#if IS_ENABLED(CONFIG_DRM_TTM)
	int error;
#endif

	if (!mapping)
		return;
#if IS_ENABLED(CONFIG_DRM_TTM)
	if (mapping->ttm) {
		error = ttm_bo_reserve(mapping->ttm, false, false, NULL);
		/* Never drop the GEM reference while the TTM pin remains. A
		 * failed uninterruptible reserve is exceptional and must be visible
		 * rather than turning a release into a hidden use-after-free. */
		if (WARN_ON_ONCE(error))
			return;
		if (mapping->io_reserved) {
			struct ttm_buffer_object *ttm = mapping->ttm;

			if (ttm->bdev->funcs->io_mem_free)
				ttm->bdev->funcs->io_mem_free(ttm->bdev,
							    ttm->resource);
			ttm->resource->bus.offset = 0;
			ttm->resource->bus.addr = NULL;
		}
		ttm_bo_unpin(mapping->ttm);
		ttm_bo_unreserve(mapping->ttm);
	}
#endif
	drm_gem_object_put(mapping->object);
	kfree(mapping);
}

static const struct kobox_linux_drm_mapping_operations mapping_operations = {
	.size = sizeof(mapping_operations),
	.get_size = gem_size,
	.map = map_system_memory_pages,
	.release = release_system_memory_pages,
};

static int __init kobox_drm_mapping_init(void)
{
	return kobox_linux_drm_mapping_register(&mapping_operations, THIS_MODULE);
}

static void __exit kobox_drm_mapping_exit(void)
{
	kobox_linux_drm_mapping_unregister(&mapping_operations);
}

module_init(kobox_drm_mapping_init);
module_exit(kobox_drm_mapping_exit);
MODULE_DESCRIPTION("kobox2 DRM system-memory mapping provider");
MODULE_LICENSE("GPL");
