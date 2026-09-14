// SPDX-License-Identifier: GPL-2.0-only

#include "device_launch.h"
#include "drm_file.h"
#include "drm_file_gate.h"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <drm/drm.h>
#include <kobox2/gpu_layout.h>

struct kobox_linux_device_session {
	struct pci_host_bridge *bridge;
	struct pci_dev *pci;
	struct kobox_linux_dma_port *dma;
	struct kobox_linux_irq_port *irq;
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
	    !launch->pci || !launch->dma || !launch->irq ||
	    !launch->render_file_limit || launch->render_file_limit > 1024)
		return -EINVAL;
	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		return -ENOMEM;
	*out = session;
	session->render_file_limit = launch->render_file_limit;
	result = kobox_linux_pci_scan(launch->pci, &session->bridge);
	if (result)
		return result;
	pci_assign_unassigned_bus_resources(session->bridge->bus);
	session->pci = pci_get_slot(session->bridge->bus, launch->pci->devfn);
	if (!session->pci || session->pci->driver)
		return -ENODEV;
	result = kobox_linux_dma_attach(&session->pci->dev, launch->dma,
				       &session->dma);
	if (result)
		return result;
	result = kobox_linux_irq_attach(session->pci, launch->irq, &session->irq);
	if (result)
		return result;
	pci_bus_add_devices(session->bridge->bus);
	return 0;
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
	struct device *gpu;
	struct kobox_linux_drm_version version;
	const size_t capacity[] = {sizeof(version.name), sizeof(version.date),
				   sizeof(version.description)};
	u64 prime;
	int result;

	if (!session || !session->pci || session->render || !report)
		return -EINVAL;
	wait_for_device_probe();
	if (!session->pci->driver ||
	    strcmp(session->pci->driver->name, "virtio-pci"))
		return -ENODEV;
	gpu = device_find_child(&session->pci->dev, NULL, is_virtio_gpu);
	if (!gpu)
		return -ENODEV;
	/* DRM minors are children of this PCI function, not a global name scan. */
	result = device_for_each_child(&session->pci->dev, &candidate,
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
			session->render_file_limit, &session->service);
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

	if (!session || !report)
		return -EINVAL;
	if (session->close_error || session->render ||
	    (session->pci && session->pci->driver))
		return -EBUSY;
	if (session->service) {
		result = kobox_linux_drm_service_destroy(&session->service);
		if (result)
			return result;
	}
	if (session->irq) {
		result = kobox_linux_irq_detach(session->irq);
		if (result)
			return result;
		session->irq = NULL;
	}
	if (session->dma) {
		result = kobox_linux_dma_detach(session->dma);
		if (result)
			return result;
		session->dma = NULL;
	}
	pci_dev_put(session->pci);
	session->pci = NULL;
	if (session->bridge) {
		result = kobox_linux_pci_remove(session->bridge);
		if (result)
			return result;
	}
	kfree(session);
	report->drained = 1;
	return 0;
}
