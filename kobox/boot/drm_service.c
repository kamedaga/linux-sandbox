// SPDX-License-Identifier: GPL-2.0-only

#include "drm_service.h"
#include "drm_limits.h"
#include "diagnostic.h"

#include <drm/drm.h>
#include <drm/drm_ioctl.h>
#include <drm/virtgpu_drm.h>
#include <linux/fdtable.h>
#include <linux/dma-buf.h>
#include <linux/dma-direction.h>
#include <linux/file.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/sync_file.h>
#include <kobox2/gpu_layout.h>
#include "../arch/x86_64/host_call.h"

struct kobox_linux_drm_service;

struct drm_owned_file {
	u64 cookie;
	struct kobox_linux_drm_file *file;
	struct kobox_linux_drm_service *service;
	struct kobox_linux_drm_event_observer observer;
	u32 slot;
	int close_error;
};

enum { DRM_MAPPING_LIMIT = KOBOX_DRM_MAPPING_LIMIT,
       DRM_PRIME_LIMIT = KOBOX_DRM_PRIME_LIMIT };

struct drm_owned_mapping {
	u64 id;
	struct kobox_linux_drm_mapping *owner;
};

struct drm_owned_prime {
	u64 id;
	int dma_fd;
	struct kobox_linux_drm_mapping *owner;
};

struct drm_owned_sync_file {
    u64 id;
    int fd;
};

struct kobox_linux_drm_service {
	struct task_struct *owner;
	struct files_struct *files;
	struct drm_owned_file *entries;
	struct drm_owned_mapping mappings[DRM_MAPPING_LIMIT];
	struct drm_owned_prime primes[DRM_PRIME_LIMIT];
    struct drm_owned_sync_file sync_files[KOBOX_DRM_SYNC_FILE_LIMIT];
    u64 sync_file_sequence;
	struct kobox_linux_drm_service_report report;
	struct kobox_linux_drm_event_host events;
	struct kobox_drm_fences fences;
	dev_t primary, render;
	unsigned int limit;
	u64 sequence;
	u64 mapping_sequence;
	u64 prime_sequence;
	bool stopping;
};

static void notify_event(void *context)
{
	struct drm_owned_file *entry = context;
	u64 cookie = READ_ONCE(entry->cookie);

	if (cookie)
		(void)kobox_host_call(entry->service->events.notify(
			entry->service->events.context, entry->slot, cookie));
}

static int notify_fences(void *context)
{
	struct kobox_linux_drm_service *service = context;

	return service->events.notify_fences ?
		service->events.notify_fences(service->events.context) : -EOPNOTSUPP;
}

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
	/* close_fd may queue the dma-buf's last fput on hosted PID 1, which
	 * has no Linux exit-to-user path. All object/FD locks are released. */
	task_work_run();
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
				   const struct kobox_linux_drm_event_host *events,
				   struct kobox_linux_drm_service **out)
{
	struct kobox_linux_drm_service *service;
	unsigned int index;

	if (!out || *out || !limit || limit > 64 || !events ||
	    events->size != sizeof(*events) || !events->context || !events->notify ||
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
	service->events = *events;
	if (kobox_drm_fences_init(&service->fences, notify_fences, service)) {
		put_task_struct(service->owner);
		kfree(service->entries);
		kfree(service);
		return -EINVAL;
	}
	for (index = 0; index < limit; index++) {
		service->entries[index].service = service;
		service->entries[index].slot = index;
	}
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
	if (service->report.active == service->limit || service->sequence == U64_MAX) {
        static unsigned int emfile_seen;
        if (emfile_seen < 4) {
            ++emfile_seen;
            kobox_linux_boot_diagnostic(
                "EMFILE kobox drm-open used=%u limit=%u sequence=%llu pid=%d\n",
                service->report.active, service->limit,
                (unsigned long long)service->sequence, task_pid_nr(current));
        }
        return -EMFILE;
    }
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
	result = kobox_linux_drm_event_attach(entry->file, &entry->observer,
					      notify_event, entry);
	if (result) {
		int closed = kobox_linux_drm_close(&entry->file);

		entry->cookie = 0;
		return closed ? closed : result;
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
	int result = entry->observer.attached ?
		kobox_linux_drm_event_detach(&entry->observer) : 0;

	if (!result)
		result = kobox_linux_drm_close(&entry->file);

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

int kobox_linux_drm_service_execbuffer(struct kobox_linux_drm_service *service,
	u64 cookie, u64 session, u64 correlation, u32 flags, u32 ring_index,
	const void *command, size_t command_size, const void *handles, size_t handle_count,
	const void *input, size_t input_count, const void *output, size_t output_count)
{
	struct kobox_linux_drm_file *file = NULL;
	struct kobox_drm_fence_entry *entry = NULL;
	struct dma_fence *fence = NULL;
	int result = kobox_linux_drm_service_file(service, cookie, &file);

	if (result)
		return result;
	if (flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) {
		if (!service->events.notify_fences)
			return -EOPNOTSUPP;
		result = kobox_drm_fence_reserve(&service->fences,
			session, correlation, &entry);
		if (result)
			return result;
	}
	result = kobox_linux_drm_virtgpu_execbuffer(file, flags, ring_index,
		command, command_size, handles, handle_count,
		input, input_count, output, output_count, &fence);
	if (!result && entry) {
		result = fence ? kobox_drm_fence_arm(entry, fence) : -EPROTO;
		if (!result) {
			/* The service, not the submitting file, now owns completion. */
			fence = NULL;
			entry = NULL;
		}
	}
	if (fence)
		dma_fence_put(fence);
	if (entry)
		(void)kobox_drm_fence_cancel(&entry);
	return result;
}

int kobox_linux_drm_service_take_fence(struct kobox_linux_drm_service *service,
	struct kobox_drm_fence_result *result)
{
	int error = check_owner(service);

	return error ? error : kobox_drm_fences_take(&service->fences, result);
}

static int service_map(struct kobox_linux_drm_service *service,
				u64 cookie, u32 handle, u32 mapping_rights, u32 kind, bool i915,
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
	error = i915 ? kobox_linux_drm_i915_map_pages(file->file, handle, mapping_rights,
		kind, page_indices, page_capacity, &pages, &owner) :
		kobox_linux_drm_map_pages(file->file, handle, mapping_rights,
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
		.backing_kind = pages.backing_kind,
	};
	return 0;
}

int kobox_linux_drm_service_map(struct kobox_linux_drm_service *service,
    u64 cookie,u32 handle,u32 rights,u64 *pages,size_t capacity,
    struct kobox_linux_drm_service_mapping *result)
{ return service_map(service,cookie,handle,rights,0,false,pages,capacity,result); }
int kobox_linux_drm_service_i915_map(struct kobox_linux_drm_service *service,
    u64 cookie,u32 handle,u32 rights,u32 kind,u64 *pages,size_t capacity,
    struct kobox_linux_drm_service_mapping *result)
{
    if(kind>2) return -EINVAL;
    return service_map(service,cookie,handle,rights,kind,true,pages,capacity,result);
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
	/* The attachment publisher proves BAR ownership or RAM bounds using
	 * these tags. Dropping them would reinterpret BAR PFNs as RAM indices
	 * or create a WB alias for a WC buffer. Keep the BO pinned until every
	 * exported lease has ended, just as for ordinary DRM mappings. */
	if (!((pages.backing_kind == KOBOX_DRM_BACKING_RAM &&
	       (pages.cache_policy == KOBOX_DRM_CACHE_WB ||
		pages.cache_policy == KOBOX_DRM_CACHE_WC)) ||
	      (pages.backing_kind == KOBOX_DRM_BACKING_DEVICE &&
	       (pages.cache_policy == KOBOX_DRM_CACHE_UC ||
		pages.cache_policy == KOBOX_DRM_CACHE_WC)))) {
		error = kobox_linux_drm_mapping_release(&owner);
		return error ?: -EOPNOTSUPP;
	}
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
		.cache_policy = pages.cache_policy,
		.backing_kind = pages.backing_kind,
	};
	return 0;
}

/* Real Linux sync_file FDs remain installed in this owner task. An opaque
 * token selects the ledger entry only after the native broker proves its
 * lease capability. No wait channel is ever treated as a Linux dma_fence. */
static struct drm_owned_sync_file *find_sync_file(
    struct kobox_linux_drm_service *service, u64 token)
{
    unsigned int i;
    if(!token) return NULL;
    for(i=0;i<KOBOX_DRM_SYNC_FILE_LIMIT;++i)
        if(service->sync_files[i].id==token) return &service->sync_files[i];
    return NULL;
}

static int sync_file_reserve(struct kobox_linux_drm_service *service,
    u64 session,u64 correlation,struct drm_owned_sync_file **slot,
    struct kobox_drm_fence_entry **watch)
{
    unsigned int i;
    if(service->stopping || service->report.close_error) return -ESHUTDOWN;
    if(service->sync_file_sequence==U64_MAX) return -EOVERFLOW;
    for(i=0;i<KOBOX_DRM_SYNC_FILE_LIMIT;++i) if(!service->sync_files[i].id) break;
    if(i==KOBOX_DRM_SYNC_FILE_LIMIT) return -ENOSPC;
    *slot=&service->sync_files[i];
    return kobox_drm_fence_reserve(&service->fences,session,correlation,watch);
}

static int retain_sync_file(struct kobox_linux_drm_service *service,
    struct drm_owned_sync_file *slot,struct kobox_drm_fence_entry **watch,
    int internal_fd,u64 *token)
{
    struct dma_fence *fence=sync_file_get_fence(internal_fd);
    int error=fence ? kobox_drm_fence_arm(*watch,fence) : -EBADF;
    if(error) {
        int closed;
        if(fence) dma_fence_put(fence);
        closed=close_fd(internal_fd);
        task_work_run();
        if(closed && !service->report.close_error) service->report.close_error=closed;
        return error;
    }
    /* The watch owns its fence reference independently of this FD/lease.
     * Closing an export before completion does not manufacture a signal. */
    *slot=(struct drm_owned_sync_file){.id=++service->sync_file_sequence,.fd=internal_fd};
    *token=slot->id;
    *watch=NULL;
    return 0;
}

int kobox_linux_drm_service_syncobj_export_file(
    struct kobox_linux_drm_service *service,u64 cookie,u32 handle,
    u64 session,u64 correlation,u64 *token)
{
    struct drm_owned_file *file;
    struct drm_owned_sync_file *slot=NULL;
    struct kobox_drm_fence_entry *watch=NULL;
    int internal_fd=-1,error=check_owner(service);
    if(error) return error;
    if(!handle || !token) return -EINVAL;
    file=find_file(service,cookie);
    if(!file || !file->file) return -ENOENT;
    error=sync_file_reserve(service,session,correlation,&slot,&watch);
    if(error) return error;
    error=kobox_linux_drm_syncobj_export_file(file->file,handle,&internal_fd);
    if(!error) error=retain_sync_file(service,slot,&watch,internal_fd,token);
    if(watch) kobox_drm_fence_cancel(&watch);
    return error;
}

int kobox_linux_drm_service_prime_export_file(
    struct kobox_linux_drm_service *service,u64 prime,u32 flags,
    u64 session,u64 correlation,u64 *token)
{
    struct drm_owned_sync_file *slot=NULL;
    struct kobox_drm_fence_entry *watch=NULL;
    unsigned int i;
    int internal_fd=-1,error=check_owner(service);
    if(error) return error;
    if(!prime || !flags || (flags & ~3u) || !token) return -EINVAL;
    for(i=0;i<DRM_PRIME_LIMIT;++i)
        if(service->primes[i].id==prime && service->primes[i].dma_fd>=0) break;
    if(i==DRM_PRIME_LIMIT) return -ENOENT;
    error=sync_file_reserve(service,session,correlation,&slot,&watch);
    if(error) return error;
    error=kobox_linux_drm_dmabuf_export_file(service->primes[i].dma_fd,flags,&internal_fd);
    if(!error) error=retain_sync_file(service,slot,&watch,internal_fd,token);
    if(watch) kobox_drm_fence_cancel(&watch);
    return error;
}

int kobox_linux_drm_service_syncobj_import_file(
    struct kobox_linux_drm_service *service,u64 cookie,u32 handle,u64 token)
{
    struct drm_owned_sync_file *sync;
    struct drm_owned_file *file;
    int error=check_owner(service);
    if(error) return error;
    if(service->stopping || service->report.close_error) return -ESHUTDOWN;
    if(!handle) return -EINVAL;
    sync=find_sync_file(service,token);
    if(!sync) return -ENOENT;
    file=find_file(service,cookie);
    if(!file || !file->file) return -ENOENT;
    return kobox_linux_drm_syncobj_import_file(file->file,handle,sync->fd);
}

int kobox_linux_drm_service_prime_import_file(
    struct kobox_linux_drm_service *service,u64 prime,u32 flags,u64 token)
{
    struct drm_owned_sync_file *sync;
    unsigned int i;
    int error=check_owner(service);
    if(error) return error;
    if(service->stopping || service->report.close_error) return -ESHUTDOWN;
    if(!prime || !flags || (flags & ~3u)) return -EINVAL;
    sync=find_sync_file(service,token);
    if(!sync) return -ENOENT;
    for(i=0;i<DRM_PRIME_LIMIT;++i)
        if(service->primes[i].id==prime && service->primes[i].dma_fd>=0) break;
    if(i==DRM_PRIME_LIMIT) return -ENOENT;
    return kobox_linux_drm_dmabuf_import_file(service->primes[i].dma_fd,flags,sync->fd);
}

int kobox_linux_drm_service_release_sync_file(
    struct kobox_linux_drm_service *service,u64 token)
{
    struct drm_owned_sync_file *sync;
    int error=check_owner(service);
    if(error) return error;
    sync=find_sync_file(service,token);
    if(!sync) return -ENOENT;
    /* close_fd consumes the descriptor even when flush reports an error. */
    error=close_fd(sync->fd);
    *sync=(struct drm_owned_sync_file){0};
    task_work_run();
    if(error && !service->report.close_error) service->report.close_error=error;
    return error;
}

int kobox_linux_drm_service_prime_cpu_sync(
    struct kobox_linux_drm_service *service, u64 prime_id, u64 flags)
{
    enum dma_data_direction direction;
    struct dma_buf *buffer;
    unsigned int index;
    int error = check_owner(service);

    if (error)
        return error;
    if (!(flags & 3ULL) || (flags & ~7ULL))
        return -EINVAL;
    if (!prime_id)
        return -EOPNOTSUPP;
    if (service->stopping || service->report.close_error)
        return -ESHUTDOWN;
    for (index = 0; index < DRM_PRIME_LIMIT; ++index)
        if (service->primes[index].id == prime_id &&
            service->primes[index].dma_fd >= 0)
            break;
    if (index == DRM_PRIME_LIMIT)
        return -ENOENT;
    /* The same lifecycle owner serializes export, sync and retirement.
     * Retain a dma-buf reference across the potentially sleeping exporter
     * callback; never infer coherency from a native PAGE_VIEW mapping. */
    buffer = dma_buf_get(service->primes[index].dma_fd);
    if (IS_ERR(buffer))
        return PTR_ERR(buffer);
    if (((flags & 1) && !(buffer->file->f_mode & FMODE_READ)) ||
        ((flags & 2) && !(buffer->file->f_mode & FMODE_WRITE))) {
        dma_buf_put(buffer);
        return -EACCES;
    }
    direction = (flags & 3) == 3 ? DMA_BIDIRECTIONAL :
        (flags & 2) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    error = flags & 4 ? dma_buf_end_cpu_access(buffer, direction) :
        dma_buf_begin_cpu_access(buffer, direction);
    dma_buf_put(buffer);
    return error;
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
	result = kobox_drm_fences_quiesce(&service->fences);
	if (result && !service->report.close_error)
		service->report.close_error = result;
    for(index=0;index<KOBOX_DRM_SYNC_FILE_LIMIT;++index) {
        if(service->sync_files[index].id) {
            result=kobox_linux_drm_service_release_sync_file(service,service->sync_files[index].id);
            if(result && !service->report.close_error) service->report.close_error=result;
        }
    }
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
	result = kobox_linux_drm_mapping_quiesce();
	if (result && !service->report.close_error)
		service->report.close_error = result;
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
	if (!service->stopping || service->report.active || service->report.close_error) {
		kobox_linux_boot_diagnostic(
			"kobox-drm: service destroy busy stopping=%u active=%u close_error=%d\n",
			service->stopping, service->report.active,
			service->report.close_error);
		return -EBUSY;
	}
	put_task_struct(service->owner);
	kfree(service->entries);
	kfree(service);
	*owner = NULL;
	return 0;
}
