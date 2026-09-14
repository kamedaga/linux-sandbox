/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DRM_SERVICE_H
#define KOBOX_BOOT_DRM_SERVICE_H

#include "drm_file.h"

struct kobox_linux_drm_service;

/* Process-local Linux ownership ledger. Cookies are not GPU session IDs and
 * must never be accepted directly from wire requests. All operations run on
 * the creating Linux task; the device launcher remains the final owner.
 */
struct kobox_linux_drm_service_report {
	uint64_t opened;
	uint64_t closed;
	uint32_t active;
	uint32_t peak;
	uint64_t distinct_checks;
	int close_error;
};

struct kobox_linux_drm_service_mapping {
	uint64_t mapping_id;
	uint64_t length;
	uint64_t page_count;
	uint32_t cache_policy;
};

struct kobox_linux_drm_service_prime {
	uint64_t prime_id;
	uint64_t length;
	uint64_t page_count;
};

#ifdef __KERNEL__
int kobox_linux_drm_service_create(dev_t primary, dev_t render,
				   unsigned int limit,
				   struct kobox_linux_drm_service **out);
/* Stops admission, synchronously closes every retained file, and preserves
 * the first close error even when a failed close consumed its file pointer.
 */
int kobox_linux_drm_service_quiesce(struct kobox_linux_drm_service *service,
				    struct kobox_linux_drm_service_report *report);
int kobox_linux_drm_service_destroy(struct kobox_linux_drm_service **service);
#endif

int kobox_linux_drm_service_open(struct kobox_linux_drm_service *service,
				 uint32_t node_type, uint64_t *cookie_out);
/* The borrowed file is valid only within this owner-task dispatch. */
int kobox_linux_drm_service_file(struct kobox_linux_drm_service *service,
				 uint64_t cookie,
				 struct kobox_linux_drm_file **file_out);
int kobox_linux_drm_service_close(struct kobox_linux_drm_service *service,
				  uint64_t cookie);
int kobox_linux_drm_service_map(struct kobox_linux_drm_service *service,
				uint64_t cookie, uint32_t handle,
				uint32_t mapping_rights,
				uint64_t *page_indices, size_t page_capacity,
				struct kobox_linux_drm_service_mapping *result);
int kobox_linux_drm_service_unmap(struct kobox_linux_drm_service *service,
				  uint64_t mapping_id);
int kobox_linux_drm_service_prime_export(
	struct kobox_linux_drm_service *service, uint64_t cookie,
	uint32_t handle, uint32_t flags, uint64_t *page_indices,
	size_t page_capacity, struct kobox_linux_drm_service_prime *result);
int kobox_linux_drm_service_prime_import(
	struct kobox_linux_drm_service *service, uint64_t cookie,
	uint64_t prime_id, uint32_t *handle);

#endif
