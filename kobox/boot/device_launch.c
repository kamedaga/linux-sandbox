// SPDX-License-Identifier: GPL-2.0-only

#include "device_launch.h"
#include "device_port.h"
#include "diagnostic.h"
#include "drm_file.h"
#include "drm_file_gate.h"

#ifndef KOBOX_BOOT_VIRTIO_GPU
#define KOBOX_BOOT_VIRTIO_GPU 1
#endif

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#if KOBOX_BOOT_VIRTIO_GPU
#include <linux/virtio.h>
#endif
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
#if !KOBOX_BOOT_VIRTIO_GPU
	u64 inspect_started_ns;
	unsigned int inspect_samples;
#endif
};

#if !KOBOX_BOOT_VIRTIO_GPU
/* These are inspection abort limits, not a claim about the card's thermal
 * rating. This profile cannot read the board surface after driver unload,
 * so keep the driver alive only for a short, continuously sampled window.
 */
#define KOBOX_INSPECT_TEMP_LIMIT_MC 70000u
#define KOBOX_INSPECT_MAX_NS (120ULL * NSEC_PER_SEC)

int kobox_linux_device_inspection_monitor(void *context)
{
	struct kobox_linux_device_session *session = context;
	u64 elapsed_ns;
	u32 temperature = 0, watts = 0;
	int result, power_result;

	if (!session || !session->render || !session->inspect_started_ns)
		return -EINVAL;
	elapsed_ns = ktime_get_mono_fast_ns() - session->inspect_started_ns;
	result = kobox_linux_drm_amdgpu_temperature(session->render,
						  &temperature);
	if (result) {
		kobox_linux_boot_diagnostic("kobox-drm: inspection sensor unavailable sample=%u status=%d elapsed_ms=%llu\n",
					    session->inspect_samples, result,
					    (unsigned long long)(elapsed_ns / NSEC_PER_MSEC));
		return result;
	}
	power_result = kobox_linux_drm_amdgpu_average_power(session->render,
							    &watts);
	kobox_linux_boot_diagnostic("kobox-drm: inspection sample=%u elapsed_ms=%llu temperature_mc=%u average_power_status=%d average_power_w=%u\n",
				    session->inspect_samples++,
				    (unsigned long long)(elapsed_ns / NSEC_PER_MSEC),
				    temperature, power_result, watts);
	if (temperature >= KOBOX_INSPECT_TEMP_LIMIT_MC) {
		kobox_linux_boot_diagnostic("kobox-drm: inspection abort temperature_mc=%u limit_mc=%u\n",
					    temperature, KOBOX_INSPECT_TEMP_LIMIT_MC);
		return -ERANGE;
	}
	if (elapsed_ns >= KOBOX_INSPECT_MAX_NS) {
		kobox_linux_boot_diagnostic("kobox-drm: inspection time limit reached elapsed_ms=%llu\n",
					    (unsigned long long)(elapsed_ns / NSEC_PER_MSEC));
		return -ETIME;
	}
	return 0;
}
#else
int kobox_linux_device_inspection_monitor(void *context)
{
	(void)context;
	return -EOPNOTSUPP;
}
#endif

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
	    !launch->pci || !launch->dma || !launch->irq)
		return -EINVAL;
	if (!launch->drm_events ||
	    launch->drm_events->size != sizeof(*launch->drm_events) ||
	    !launch->drm_events->context || !launch->drm_events->notify ||
	    !launch->render_file_limit || launch->render_file_limit > 64)
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

#if KOBOX_BOOT_VIRTIO_GPU
static int bind_queue_dma(struct device *device, void *argument)
{
	struct kobox_linux_device_session *session = argument;

	if (!device->bus || strcmp(device->bus->name, "virtio"))
		return 0;
	return kobox_linux_dma_bind_virtio(
		kobox_linux_device_port_dma(session->port), dev_to_virtio(device));
}
#endif

int kobox_linux_device_module_ready(struct kobox_linux_device_session *session)
{
	struct pci_dev *pci = session ?
		kobox_linux_device_port_pci(session->port) : NULL;

	if (!pci)
		return -EINVAL;
#if KOBOX_BOOT_VIRTIO_GPU
	/* virtio-pci publishes its child before virtio_gpu is loaded. Install
	 * the upstream mapping interface here, never into already-live queues.
	 */
	wait_for_device_probe();
	return device_for_each_child(&pci->dev, session, bind_queue_dma);
#else
	/* A direct PCI DRM driver uses the domain already attached to the PCI
	 * function. There is no nested virtio queue to bind.
	 */
	return 0;
#endif
}

static int find_drm_node(struct device *device, void *argument)
{
	struct kobox_linux_device_launch_report *report = argument;

	if (device->class && !strcmp(device->class->name, "drm") &&
	    device->devt && !strncmp(dev_name(device), "renderD", 7)) {
		if (report->render_major)
			return -EEXIST;
		report->render_major = MAJOR(device->devt);
		report->render_minor = MINOR(device->devt);
	} else if (device->class && !strcmp(device->class->name, "drm") &&
		   device->devt && !strncmp(dev_name(device), "card", 4)) {
		if (report->primary_major)
			return -EEXIST;
		report->primary_major = MAJOR(device->devt);
		report->primary_minor = MINOR(device->devt);
	}
	/* DRM minors may be parented directly by a PCI GPU or below an
	 * intermediate bus device (virtio-pci). Never search the global DRM
	 * class: only descendants of the exclusively granted PCI function count.
	 */
	return device_for_each_child(device, argument, find_drm_node);
}

int kobox_linux_device_ready(struct kobox_linux_device_session *session,
			    struct kobox_linux_device_launch_report *report)
{
	struct kobox_linux_device_launch_report candidate = {0};
	struct pci_dev *pci = session ?
		kobox_linux_device_port_pci(session->port) : NULL;
	struct kobox_linux_drm_version version;
	const size_t capacity[] = {sizeof(version.name), sizeof(version.date),
				   sizeof(version.description)};
	u64 prime;
	int result;

	if (!pci || session->render || !report)
		return -EINVAL;
	wait_for_device_probe();
	if (!pci->driver) {
		kobox_linux_boot_diagnostic("kobox-drm: PCI driver bind status=%d\n",
					    -ENODEV);
		return -ENODEV;
	}
	/* The authorized function must itself have bound a driver and published
	 * both DRM nodes. No vendor or driver name is part of this boundary.
	 */
	result = device_for_each_child(&pci->dev, &candidate,
				       find_drm_node);
	kobox_linux_boot_diagnostic("kobox-drm: node discovery status=%d primary=%u:%u render=%u:%u\n",
				    result, candidate.primary_major,
				    candidate.primary_minor, candidate.render_major,
				    candidate.render_minor);
	if (result)
		return result;
	if (!candidate.primary_major || !candidate.render_major)
		return -ENODEV;
	candidate.bound = 1;
	kobox_linux_boot_diagnostic("kobox-drm: render open begin dev=%u:%u\n",
				    candidate.render_major, candidate.render_minor);
	result = kobox_linux_drm_open(MKDEV(candidate.render_major,
					  candidate.render_minor),
					      KB2_GPU_NODE_RENDER, &session->render);
	kobox_linux_boot_diagnostic("kobox-drm: render open status=%d\n", result);
	if (result)
		return result;
	candidate.render_opened = 1;
	/* Readiness is based on a real render file and regular DRM ioctl routing.
	 * It does not yet advertise a peer-facing command endpoint.
	 */
	kobox_linux_boot_diagnostic("kobox-drm: DRM_IOCTL_VERSION begin\n");
	result = kobox_linux_drm_version(session->render, capacity, &version);
	if (!result) {
		const size_t name_length = min(version.name_length,
					       sizeof(version.name));

		kobox_linux_boot_diagnostic("kobox-drm: DRM_IOCTL_VERSION status=0 driver=%.*s version=%d.%d.%d\n",
					    (int)name_length, version.name,
					    version.major, version.minor,
					    version.patchlevel);
		kobox_linux_boot_diagnostic("kobox-drm: DRM_IOCTL_GET_CAP PRIME begin\n");
		result = kobox_linux_drm_get_cap(session->render,
						 DRM_CAP_PRIME, &prime);
		if (!result)
			kobox_linux_boot_diagnostic("kobox-drm: DRM_IOCTL_GET_CAP PRIME status=0 value=0x%llx\n",
						    (unsigned long long)prime);
		else
			kobox_linux_boot_diagnostic("kobox-drm: DRM_IOCTL_GET_CAP PRIME status=%d\n",
						    result);
	} else {
		kobox_linux_boot_diagnostic("kobox-drm: DRM_IOCTL_VERSION status=%d\n",
					    result);
	}
	if (!result)
		candidate.drm_queried = 1;
#if !KOBOX_BOOT_VIRTIO_GPU
	if (!result) {
		/* Upstream schedules its real IB/ring test two seconds after probe.
		 * Sample across that interval before the timed lifecycle monitor
		 * takes over. An unavailable sensor aborts instead of leaving the
		 * inspection running without thermal feedback.
		 */
		session->inspect_started_ns = ktime_get_mono_fast_ns();
		for (unsigned int sample = 0; sample < 6; ++sample) {
			result = kobox_linux_device_inspection_monitor(session);
			if (result)
				break;
			if (sample != 5)
				msleep(500);
		}
	}
#endif
#ifdef KOBOX_RUNTIME_GATES
	if (!result)
		result = kobox_linux_drm_file_gate(session->render,
						 &candidate.drm_checks);
#endif
	if (!result && session->render_file_limit)
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
		kobox_linux_boot_diagnostic("kobox-drm: render close status=%d\n",
					    result);
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
	    (pci && pci->driver)) {
		kobox_linux_boot_diagnostic(
			"kobox-drm: device finish busy close_error=%d render_open=%u pci_bound=%u\n",
			session->close_error, !!session->render,
			!!(pci && pci->driver));
		return -EBUSY;
	}
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
