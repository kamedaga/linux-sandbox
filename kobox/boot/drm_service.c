// SPDX-License-Identifier: GPL-2.0-only

#include "drm_service.h"

#include <drm/drm.h>
#include <drm/drm_ioctl.h>
#include <linux/fdtable.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <kobox2/gpu_layout.h>

struct drm_owned_file {
	u64 cookie;
	struct kobox_linux_drm_file *file;
	int close_error;
};

enum { DRM_MAPPING_LIMIT = 64, DRM_PRIME_LIMIT = 64 };

struct drm_owned_mapping {
	u64 id;
	struct kobox_linux_drm_mapping *owner;
};

struct drm_owned_prime {
	u64 id;
	int dma_fd;
	struct kobox_linux_drm_mapping *owner;
};

struct kobox_linux_drm_service {
	struct task_struct *owner;
	struct files_struct *files;
	struct drm_owned_file *entries;
	struct drm_owned_mapping mappings[DRM_MAPPING_LIMIT];
	struct drm_owned_prime primes[DRM_PRIME_LIMIT];
	struct kobox_linux_drm_service_report report;
	dev_t primary, render;
	unsigned int limit;
	u64 sequence;
	u64 mapping_sequence;
	u64 prime_sequence;
	bool stopping;
};

static int release_mapping(struct drm_owned_mapping *mapping)
{
	int result = kobox_linux_drm_mapping_release(&mapping->owner);

	if (!result)
		mapping->id = 0;
	return result;
}

static int release_prime(struct drm_owned_prime *prime)
{
	int result = 0;

	if (prime->dma_fd >= 0) {
		result = close_fd(prime->dma_fd);
		prime->dma_fd = -1;
	}
	if (prime->owner) {
		int released = kobox_linux_drm_mapping_release(&prime->owner);

		if (!result)
			result = released;
	}
	if (!result)
		prime->id = 0;
	return result;
}

static int check_owner(struct kobox_linux_drm_service *service)
{
	if (!service || current != service->owner ||
	    current->files != service->files || current->mm ||
	    in_interrupt() || irqs_disabled())
		return -EPERM;
	return 0;
}

static struct drm_owned_file *find_file(struct kobox_linux_drm_service *service,
				      u64 cookie)
{
	unsigned int index;

	if (!cookie)
		return NULL;
	for (index = 0; index < service->limit; index++)
		if (service->entries[index].cookie == cookie)
			return &service->entries[index];
	return NULL;
}

int kobox_linux_drm_service_create(dev_t primary, dev_t render,
				   unsigned int limit,
				   struct kobox_linux_drm_service **out)
{
	struct kobox_linux_drm_service *service;

	if (!out || *out || !limit || limit > 1024 ||
	    MAJOR(primary) != DRM_MAJOR || MAJOR(render) != DRM_MAJOR)
		return -EINVAL;
	if (current->mm || !current->files || in_interrupt() || irqs_disabled() ||
	    (!(current->flags & PF_KTHREAD) && current->pid != 1))
		return -EPERM;
	service = kzalloc(sizeof(*service), GFP_KERNEL);
	if (!service)
		return -ENOMEM;
	service->entries = kcalloc(limit, sizeof(*service->entries), GFP_KERNEL);
	if (!service->entries) {
		kfree(service);
		return -ENOMEM;
	}
	service->owner = current;
	get_task_struct(service->owner);
	service->files = current->files;
	service->primary = primary;
	service->render = render;
	service->limit = limit;
	*out = service;
	return 0;
}

int kobox_linux_drm_service_open(struct kobox_linux_drm_service *service,
				 u32 node_type, u64 *cookie_out)
{
	struct drm_owned_file *entry = NULL;
	unsigned int index;
	int result;

	if (!cookie_out ||
	    (node_type != KB2_GPU_NODE_PRIMARY &&
	     node_type != KB2_GPU_NODE_RENDER))
		return -EINVAL;
	result = check_owner(service);
	if (result)
		return result;
	if (service->stopping || service->report.close_error)
		return -ESHUTDOWN;
	if (service->report.active == service->limit || service->sequence == U64_MAX)
		return -EMFILE;
	for (index = 0; index < service->limit; index++) {
		if (!service->entries[index].cookie) {
			entry = &service->entries[index];
			break;
		}
	}
	if (!entry)
		return -EUCLEAN;
	/* Reserve ownership before allocation; a cookie is never reused even if
	 * open fails. Neither the caller nor response publication owns this file.
	 */
	entry->cookie = ++service->sequence;
	result = kobox_linux_drm_open(
		node_type == KB2_GPU_NODE_PRIMARY ? service->primary : service->render,
		node_type, &entry->file);
	if (result) {
		entry->cookie = 0;
		return result;
	}
	service->report.opened++;
	service->report.active++;
	service->report.peak = max(service->report.peak, service->report.active);
	for (index = 0; index < service->limit; index++) {
		struct drm_owned_file *other = &service->entries[index];

		if (other == entry || !other->file)
			continue;
		result = kobox_linux_drm_distinct(entry->file, other->file);
		if (result) {
			/* Retain both files and stop admission on an identity violation. */
			service->report.close_error = result;
			service->stopping = true;
			return result;
		}
		if (service->report.distinct_checks != U64_MAX)
			service->report.distinct_checks++;
	}
	*cookie_out = entry->cookie;
	return 0;
}

int kobox_linux_drm_service_file(struct kobox_linux_drm_service *service,
				 u64 cookie, struct kobox_linux_drm_file **file_out)
{
	struct drm_owned_file *entry;
	int result;

	if (!file_out)
		return -EINVAL;
	result = check_owner(service);
	if (result)
		return result;
	if (service->stopping || service->report.close_error)
		return -ESHUTDOWN;
	entry = find_file(service, cookie);
	if (!entry || !entry->file)
		return -ENOENT;
	*file_out = entry->file;
	return 0;
}

static int close_entry(struct kobox_linux_drm_service *service,
		       struct drm_owned_file *entry)
{
	int result = kobox_linux_drm_close(&entry->file);

	entry->close_error = result;
	if (result && !service->report.close_error)
		service->report.close_error = result;
	if (!entry->file) {
		entry->cookie = 0;
		service->report.active--;
		service->report.closed++;
	}
	return result;
}

int kobox_linux_drm_service_close(struct kobox_linux_drm_service *service,
				  u64 cookie)
{
	struct drm_owned_file *entry;
	int result = check_owner(service);

	if (result)
		return result;
	if (service->stopping || service->report.close_error)
		return -ESHUTDOWN;
	entry = find_file(service, cookie);
	if (!entry || !entry->file)
		return -ENOENT;
	return close_entry(service, entry);
}

int kobox_linux_drm_service_map(struct kobox_linux_drm_service *service,
				u64 cookie, u32 handle, u32 mapping_rights,
				u64 *page_indices, size_t page_capacity,
				struct kobox_linux_drm_service_mapping *result)
{
	struct kobox_linux_drm_map_pages pages;
	struct kobox_linux_drm_mapping *owner = NULL;
	struct drm_owned_mapping *mapping = NULL;
	struct drm_owned_file *file;
	unsigned int index;
	int error = check_owner(service);

	if (error)
		return error;
	if (!result || service->stopping || service->report.close_error)
		return !result ? -EINVAL : -ESHUTDOWN;
	file = find_file(service, cookie);
	if (!file || !file->file)
		return -ENOENT;
	if (service->mapping_sequence >= (U64_MAX >> PAGE_SHIFT))
		return -ENOSPC;
	for (index = 0; index < DRM_MAPPING_LIMIT; index++) {
		if (!service->mappings[index].id) {
			mapping = &service->mappings[index];
			break;
		}
	}
	if (!mapping)
		return -ENOSPC;
	error = kobox_linux_drm_map_pages(file->file, handle, mapping_rights,
		page_indices, page_capacity, &pages, &owner);
	if (error)
		return error;
	mapping->id = ++service->mapping_sequence << PAGE_SHIFT;
	mapping->owner = owner;
	*result = (struct kobox_linux_drm_service_mapping) {
		.mapping_id = mapping->id,
		.length = pages.length,
		.page_count = pages.page_count,
		.cache_policy = pages.cache_policy,
	};
	return 0;
}

int kobox_linux_drm_service_unmap(struct kobox_linux_drm_service *service,
				  u64 mapping_id)
{
	unsigned int index;
	int result = check_owner(service);

	if (result)
		return result;
	if (!mapping_id)
		return -EINVAL;
	for (index = 0; index < DRM_MAPPING_LIMIT; index++)
		if (service->mappings[index].id == mapping_id)
			return release_mapping(&service->mappings[index]);
	for (index = 0; index < DRM_PRIME_LIMIT; index++)
		if (service->primes[index].id == mapping_id)
			return release_prime(&service->primes[index]);
	return -ENOENT;
}

int kobox_linux_drm_service_prime_export(
	struct kobox_linux_drm_service *service, u64 cookie,
	u32 handle, u32 flags, u64 *page_indices, size_t page_capacity,
	struct kobox_linux_drm_service_prime *result)
{
	struct kobox_linux_drm_map_pages pages;
	struct kobox_linux_drm_mapping *owner = NULL;
	struct drm_owned_prime *prime = NULL;
	struct drm_owned_file *file;
	unsigned int index;
	int dma_fd = -1;
	int error = check_owner(service);

	if (error)
		return error;
	if (!result || !page_indices || !page_capacity || !handle ||
	    (flags & ~(DRM_CLOEXEC | DRM_RDWR)) ||
	    service->stopping || service->report.close_error)
		return -EINVAL;
	file = find_file(service, cookie);
	if (!file || !file->file)
		return -ENOENT;
	if (service->prime_sequence >= (U64_MAX >> PAGE_SHIFT))
		return -ENOSPC;
	for (index = 0; index < DRM_PRIME_LIMIT; index++)
		if (!service->primes[index].id) {
			prime = &service->primes[index];
			break;
		}
	if (!prime)
		return -ENOSPC;
	error = kobox_linux_drm_map_pages(file->file, handle, 3,
		page_indices, page_capacity, &pages, &owner);
	if (error)
		return error;
	error = kobox_linux_drm_prime_export(file->file, handle, flags, &dma_fd);
	if (error) {
		int release_error = kobox_linux_drm_mapping_release(&owner);

		return release_error ?: error;
	}
	prime->id = (++service->prime_sequence << PAGE_SHIFT) | 1;
	prime->dma_fd = dma_fd;
	prime->owner = owner;
	*result = (struct kobox_linux_drm_service_prime) {
		.prime_id = prime->id,
		.length = pages.length,
		.page_count = pages.page_count,
	};
	return 0;
}

int kobox_linux_drm_service_prime_import(
	struct kobox_linux_drm_service *service, u64 cookie,
	u64 prime_id, u32 *handle)
{
	struct drm_owned_file *file;
	unsigned int index;
	int error = check_owner(service);

	if (error)
		return error;
	if (!prime_id || !handle || service->stopping ||
	    service->report.close_error)
		return -EINVAL;
	file = find_file(service, cookie);
	if (!file || !file->file)
		return -ENOENT;
	for (index = 0; index < DRM_PRIME_LIMIT; index++)
		if (service->primes[index].id == prime_id &&
		    service->primes[index].dma_fd >= 0)
			return kobox_linux_drm_prime_import(file->file,
				service->primes[index].dma_fd, handle);
	return -ENOENT;
}

int kobox_linux_drm_service_quiesce(struct kobox_linux_drm_service *service,
				    struct kobox_linux_drm_service_report *report)
{
	unsigned int index;
	int result = check_owner(service);

	if (result)
		return result;
	if (!report)
		return -EINVAL;
	service->stopping = true;
	for (index = 0; index < DRM_MAPPING_LIMIT; index++) {
		if (service->mappings[index].owner) {
			result = release_mapping(&service->mappings[index]);
			if (result && !service->report.close_error)
				service->report.close_error = result;
		}
	}
	for (index = 0; index < DRM_PRIME_LIMIT; index++) {
		if (service->primes[index].id) {
			result = release_prime(&service->primes[index]);
			if (result && !service->report.close_error)
				service->report.close_error = result;
		}
	}
	for (index = 0; index < service->limit; index++)
		if (service->entries[index].file && !service->entries[index].close_error)
			close_entry(service, &service->entries[index]);
	*report = service->report;
	return service->report.close_error;
}

int kobox_linux_drm_service_destroy(struct kobox_linux_drm_service **owner)
{
	struct kobox_linux_drm_service *service;
	int result;

	if (!owner || !*owner)
		return -EINVAL;
	service = *owner;
	result = check_owner(service);
	if (result)
		return result;
	if (!service->stopping || service->report.active || service->report.close_error)
		return -EBUSY;
	put_task_struct(service->owner);
	kfree(service->entries);
	kfree(service);
	*owner = NULL;
	return 0;
}
