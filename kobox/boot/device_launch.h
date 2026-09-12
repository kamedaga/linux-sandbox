/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DEVICE_LAUNCH_H
#define KOBOX_BOOT_DEVICE_LAUNCH_H

#include "pci_host.h"
#include "dma_host.h"
#include "irq_host.h"
#include "drm_service.h"

/* One exclusively authorized virtio GPU function, not a native handle or
 * wire message. Host ports and their contexts survive module unload and any
 * failed cleanup. Other device classes need their own readiness criteria.
 */
struct kobox_linux_device_launch {
	size_t size;
	const struct kobox_linux_pci_host *pci;
	const struct kobox_linux_dma_host *dma;
	const struct kobox_linux_irq_host *irq;
	uint32_t render_file_limit;
};

struct kobox_linux_device_launch_report {
	uint32_t bound;
	uint32_t primary_major, primary_minor;
	uint32_t render_major, render_minor;
	uint32_t render_opened, drm_queried, drm_checks, render_closed;
	uint32_t drained;
	struct kobox_linux_drm_service_report files;
};

#ifdef __KERNEL__
struct kobox_linux_device_session;
struct kobox_linux_drm_file;

/* Borrow only in the launch owner's Linux task until quiesce. */
struct kobox_linux_drm_service *
kobox_linux_device_service(struct kobox_linux_device_session *session);

/* Called only by the one-shot module launch owner. Even failed preparation
 * may publish a partially owned session; finish is then mandatory. A failed
 * finish retains all remaining ownership until process termination/revoke.
 */
int kobox_linux_device_prepare(const struct kobox_linux_device_launch *launch,
			      struct kobox_linux_device_session **session);
int kobox_linux_device_ready(struct kobox_linux_device_session *session,
			    struct kobox_linux_device_launch_report *report);
/* Owner-task close/drain before any module unload, including READY failure. */
int kobox_linux_device_quiesce(struct kobox_linux_device_session *session,
			      struct kobox_linux_device_launch_report *report);
int kobox_linux_device_finish(struct kobox_linux_device_session *session,
			     struct kobox_linux_device_launch_report *report);
#endif

#endif
