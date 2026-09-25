/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_MODULE_LAUNCH_H
#define KOBOX_BOOT_MODULE_LAUNCH_H

#include "lifecycle.h"
#include "device_launch.h"
#include "device_port.h"
#include "input_port.h"
#include "net_port.h"

/* Borrowed immutable package data, local to one sandbox process. One launch
 * attempt per boot; restart creates a fresh process and resource generation.
 */
struct kobox_linux_native_module {
	const void *image;
	size_t length;
	const char *name;
	const char *parameters;
};

enum kobox_linux_module_progress {
	KOBOX_MODULE_PROGRESS_PCI_PREPARE = 1,
	KOBOX_MODULE_PROGRESS_PCI_READY,
	KOBOX_MODULE_PROGRESS_MODULE_BEGIN,
	KOBOX_MODULE_PROGRESS_MODULE_FAILED,
	KOBOX_MODULE_PROGRESS_MODULES_LOADED,
	KOBOX_MODULE_PROGRESS_PROBE_WAIT,
	KOBOX_MODULE_PROGRESS_PROBE_BOUND,
	KOBOX_MODULE_PROGRESS_NET_OPEN,
	KOBOX_MODULE_PROGRESS_NET_ALLOCATED,
	KOBOX_MODULE_PROGRESS_NET_RTNL_WAIT,
	KOBOX_MODULE_PROGRESS_NET_RTNL_HELD,
	KOBOX_MODULE_PROGRESS_NET_DEVICE_FOUND,
	KOBOX_MODULE_PROGRESS_NET_DRIVER_OPEN,
	KOBOX_MODULE_PROGRESS_NET_DRIVER_OPENED,
	KOBOX_MODULE_PROGRESS_NET_DMA_MAPS,
	KOBOX_MODULE_PROGRESS_NET_DMA_FAILURE,
	KOBOX_MODULE_PROGRESS_NET_DMA_FAILURE_DETAIL,
	KOBOX_MODULE_PROGRESS_NET_FREE_PAGES,
	KOBOX_MODULE_PROGRESS_NET_PACKET_ATTACHED,
	KOBOX_MODULE_PROGRESS_NET_READY,
	KOBOX_MODULE_PROGRESS_LIFECYCLE_READY,
};

struct kobox_linux_module_launch {
	size_t size;
	const struct kobox_linux_native_module *modules;
	size_t count;
	/* Optional device attachment; readiness precedes lifecycle publication. */
	const struct kobox_linux_device_launch *device;
	/* Device-class-neutral PCI attachment. Mutually exclusive with device. */
	const struct kobox_linux_device_port_config *pci_device;
	/* USB input events are captured after the upstream HID stack loads. */
	uint32_t capture_input;
	/* Ethernet-frame service for the single PCI NIC in this generation. */
	uint32_t capture_network;
	/* Optional process-local progress callback. Called by the Linux opening
	 * task before potentially blocking driver work; it must not call Linux. */
	void (*progress)(void *context, unsigned phase, size_t module_index,
			 int status);
	void *progress_context;
	const struct kobox_linux_lifecycle *lifecycle;
};

struct kobox_linux_module_launch_report {
	size_t size;
	size_t loaded;
	size_t unloaded;
	int result;
	int cleanup_result;
	uint32_t pci_bound;
	uint32_t pci_detached;
	struct kobox_linux_device_launch_report device;
};

#ifdef __KERNEL__
int kobox_linux_modules_run(const struct kobox_linux_module_launch *launch,
			   struct kobox_linux_module_launch_report *report);
#endif

#endif
