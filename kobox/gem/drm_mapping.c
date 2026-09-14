// SPDX-License-Identifier: GPL-2.0-only

#include "../boot/drm_file.h"

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_vma_manager.h>

struct kobox_drm_mapping {
	struct drm_gem_object *object;
};

static int map_shmem_pages(struct file *file, u32 handle, u32 mapping_rights,
			   u64 *page_indices, size_t page_capacity,
			   struct kobox_linux_drm_map_pages *result,
			   void **private_mapping)
{
	struct kobox_drm_mapping *mapping;
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *object;
	struct drm_file *drm_file;
	struct drm_device *device;
	u64 offset;
	size_t page_count, index;
	int error;

	if (!file || !handle || !mapping_rights || (mapping_rights & ~3U) ||
	    !page_indices || !page_capacity || !result || !private_mapping ||
	    *private_mapping)
		return -EINVAL;
	drm_file = file->private_data;
	if (!drm_file || !drm_file->minor || !drm_file->minor->dev)
		return -ENODEV;
	device = drm_file->minor->dev;
	if (!device->driver || strcmp(device->driver->name, "virtio_gpu"))
		return -ENODEV;
	object = drm_gem_object_lookup(drm_file, handle);
	if (!object)
		return -ENOENT;
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
put_object:
	drm_gem_object_put(object);
	return error;
}

static void release_shmem_pages(void *private_mapping)
{
	struct kobox_drm_mapping *mapping = private_mapping;

	if (!mapping)
		return;
	drm_gem_object_put(mapping->object);
	kfree(mapping);
}

static const struct kobox_linux_drm_mapping_operations mapping_operations = {
	.size = sizeof(mapping_operations),
	.map = map_shmem_pages,
	.release = release_shmem_pages,
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
MODULE_DESCRIPTION("kobox2 virtio-gpu shmem mapping provider");
MODULE_LICENSE("GPL");
