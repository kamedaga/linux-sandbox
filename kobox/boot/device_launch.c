// SPDX-License-Identifier: GPL-2.0-only

#include "device_launch.h"
#include "device_port.h"
#include "drm_file.h"
#include "drm_file_gate.h"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/virtio.h>
#include <drm/drm.h>
#include <kobox2/gpu_layout.h>

struct kobox_linux_device_session {
	struct kobox_linux_device_port *port;
	struct kobox_linux_drm_event_host drm_events;
	struct kobox_linux_drm_file *render;
	struct kobox_linux_drm_service *service;
	u32 render_file_limit;
	int close_error;
	bool render_close_attempted;
};

struct kobox_linux_drm_service *
kobox_linux_device_service(struct kobox_linux_device_session *session)
{
	return session ? session->service : NULL;
}

int kobox_linux_device_prepare(const struct kobox_linux_device_launch *launch,
			      struct kobox_linux_device_session **out)
{
	struct kobox_linux_device_session *session;
	int result;

	if (!out || *out || !launch || launch->size != sizeof(*launch) ||
	    !launch->pci || !launch->dma || !launch->irq || !launch->drm_events ||
	    launch->drm_events->size != sizeof(*launch->drm_events) ||
	    !launch->drm_events->context || !launch->drm_events->notify ||
	    !launch->render_file_limit || launch->render_file_limit > 1024)
		return -EINVAL;
	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		return -ENOMEM;
	*out = session;
	session->render_file_limit = launch->render_file_limit;
	session->drm_events = *launch->drm_events;
	const struct kobox_linux_device_port_config config = {
		.size = sizeof(config),
		.pci = launch->pci,
		.dma = launch->dma,
		.irq = launch->irq,
	};
	result = kobox_linux_device_port_prepare(&config, &session->port);
	return result;
}

static int bind_queue_dma(struct device *device, void *argument)
{
	struct kobox_linux_device_session *session = argument;

	if (!device->bus || strcmp(device->bus->name, "virtio"))
		return 0;
	return kobox_linux_dma_bind_virtio(
		kobox_linux_device_port_dma(session->port), dev_to_virtio(device));
}

int kobox_linux_device_module_ready(struct kobox_linux_device_session *session)
{
	struct pci_dev *pci = session ?
		kobox_linux_device_port_pci(session->port) : NULL;

	if (!pci)
		return -EINVAL;
	/* virtio-pci publishes its child before virtio_gpu is loaded. Install
	 * the upstream mapping interface here, never into already-live queues.
	 */
	wait_for_device_probe();
	return device_for_each_child(&pci->dev, session, bind_queue_dma);
}

static int is_virtio_gpu(struct device *device, const void *argument)
{
	return device->bus && !strcmp(device->bus->name, "virtio") &&
	       device->driver && !strcmp(device->driver->name, "virtio_gpu");
}

static int find_drm_node(struct device *device, void *argument)
{
	struct kobox_linux_device_launch_report *report = argument;

	if (!device->class || strcmp(device->class->name, "drm") || !device->devt)
		return 0;
	if (!strncmp(dev_name(device), "renderD", 7)) {
		if (report->render_major)
			return -EEXIST;
		report->render_major = MAJOR(device->devt);
		report->render_minor = MINOR(device->devt);
	} else if (!strncmp(dev_name(device), "card", 4)) {
		if (report->primary_major)
			return -EEXIST;
		report->primary_major = MAJOR(device->devt);
		report->primary_minor = MINOR(device->devt);
	}
	return 0;
}

int kobox_linux_device_ready(struct kobox_linux_device_session *session,
			    struct kobox_linux_device_launch_report *report)
{
	struct kobox_linux_device_launch_report candidate = {0};
	struct pci_dev *pci = session ?
		kobox_linux_device_port_pci(session->port) : NULL;
	struct device *gpu;
	struct kobox_linux_drm_version version;
	const size_t capacity[] = {sizeof(version.name), sizeof(version.date),
				   sizeof(version.description)};
	u64 prime;
	int result;

	if (!pci || session->render || !report)
		return -EINVAL;
	wait_for_device_probe();
	if (!pci->driver || strcmp(pci->driver->name, "virtio-pci"))
		return -ENODEV;
	gpu = device_find_child(&pci->dev, NULL, is_virtio_gpu);
	if (!gpu)
		return -ENODEV;
	/* DRM minors are children of this PCI function, not a global name scan. */
	result = device_for_each_child(&pci->dev, &candidate,
				       find_drm_node);
	put_device(gpu);
	if (result)
		return result;
	if (!candidate.primary_major || !candidate.render_major)
		return -ENODEV;
	candidate.bound = 1;
	result = kobox_linux_drm_open(MKDEV(candidate.render_major,
					  candidate.render_minor),
				      KB2_GPU_NODE_RENDER, &session->render);
	if (result)
		return result;
	candidate.render_opened = 1;
	/* Readiness is based on a real render file and regular DRM ioctl routing.
	 * It does not yet advertise a peer-facing command endpoint.
	 */
	result = kobox_linux_drm_version(session->render, capacity, &version);
	if (!result)
		result = kobox_linux_drm_get_cap(session->render, DRM_CAP_PRIME, &prime);
	if (!result)
		candidate.drm_queried = 1;
#ifdef KOBOX_RUNTIME_GATES
	if (!result)
		result = kobox_linux_drm_file_gate(session->render,
						 &candidate.drm_checks);
#endif
	if (!result)
		result = kobox_linux_drm_service_create(
			MKDEV(candidate.primary_major, candidate.primary_minor),
			MKDEV(candidate.render_major, candidate.render_minor),
			session->render_file_limit, &session->drm_events,
			&session->service);
	*report = candidate;
	return result;
}

int kobox_linux_device_quiesce(struct kobox_linux_device_session *session,
			      struct kobox_linux_device_launch_report *report)
{
	int result;

	if (!session || !report)
		return -EINVAL;
	if (session->service) {
		result = kobox_linux_drm_service_quiesce(session->service,
							&report->files);
		if (result && !session->close_error)
			session->close_error = result;
	}
	if (session->render && !session->render_close_attempted) {
		session->render_close_attempted = true;
		result = kobox_linux_drm_close(&session->render);
		if (result && !session->close_error)
			session->close_error = result;
		if (!result)
			report->render_closed = 1;
	}
	return session->close_error;
}

int kobox_linux_device_finish(struct kobox_linux_device_session *session,
			     struct kobox_linux_device_launch_report *report)
{
	int result;
	struct pci_dev *pci = session ?
		kobox_linux_device_port_pci(session->port) : NULL;

	if (!session || !report)
		return -EINVAL;
	if (session->close_error || session->render ||
	    (pci && pci->driver))
		return -EBUSY;
	if (session->service) {
		result = kobox_linux_drm_service_destroy(&session->service);
		if (result)
			return result;
	}
	if (session->port) {
		result = kobox_linux_device_port_finish(session->port);
		if (result)
			return result;
		session->port = NULL;
	}
	kfree(session);
	report->drained = 1;
	return 0;
}
