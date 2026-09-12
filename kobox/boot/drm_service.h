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

#ifdef __KERNEL__
int kobox_linux_drm_service_create(dev_t render, unsigned int limit,
				   struct kobox_linux_drm_service **out);
/* Stops admission, synchronously closes every retained file, and preserves
 * the first close error even when a failed close consumed its file pointer.
 */
int kobox_linux_drm_service_quiesce(struct kobox_linux_drm_service *service,
				    struct kobox_linux_drm_service_report *report);
int kobox_linux_drm_service_destroy(struct kobox_linux_drm_service **service);
#endif

int kobox_linux_drm_service_open(struct kobox_linux_drm_service *service,
				 uint64_t *cookie_out);
/* The borrowed file is valid only within this owner-task dispatch. */
int kobox_linux_drm_service_file(struct kobox_linux_drm_service *service,
				 uint64_t cookie,
				 struct kobox_linux_drm_file **file_out);
int kobox_linux_drm_service_close(struct kobox_linux_drm_service *service,
				  uint64_t cookie);

#endif
