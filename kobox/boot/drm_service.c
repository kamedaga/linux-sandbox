// SPDX-License-Identifier: GPL-2.0-only

#include "drm_service.h"

#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>

struct drm_owned_file {
	u64 cookie;
	struct kobox_linux_drm_file *file;
	int close_error;
};

struct kobox_linux_drm_service {
	struct task_struct *owner;
	struct files_struct *files;
	struct drm_owned_file *entries;
	struct kobox_linux_drm_service_report report;
	dev_t render;
	unsigned int limit;
	u64 sequence;
	bool stopping;
};

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

int kobox_linux_drm_service_create(dev_t render, unsigned int limit,
				   struct kobox_linux_drm_service **out)
{
	struct kobox_linux_drm_service *service;

	if (!out || *out || !limit || limit > 1024)
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
	service->render = render;
	service->limit = limit;
	*out = service;
	return 0;
}

int kobox_linux_drm_service_open(struct kobox_linux_drm_service *service,
				 u64 *cookie_out)
{
	struct drm_owned_file *entry = NULL;
	unsigned int index;
	int result;

	if (!cookie_out)
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
	result = kobox_linux_drm_open(service->render, &entry->file);
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
