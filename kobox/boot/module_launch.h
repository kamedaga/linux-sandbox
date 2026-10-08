/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_MODULE_LAUNCH_H
#define KOBOX_BOOT_MODULE_LAUNCH_H

#include "lifecycle.h"
#include "device_launch.h"
#include "device_port.h"
#include "input_port.h"
#include "net_port.h"
#include "block_port.h"
#include "firmware_files.h"
#include "fs_mount.h"
#include <kobox2/module_progress.h>

/* Borrowed immutable package data, local to one sandbox process. One launch
 * attempt per boot; restart creates a fresh process and resource generation.
 */
struct kobox_linux_native_module {
	const void *image;
	size_t length;
	const char *name;
	const char *parameters;
};

struct kobox_linux_module_launch {
	size_t size;
	const struct kobox_linux_native_module *modules;
	size_t count;
	/* Optional verified files materialized in Linux's private rootfs before
	 * modules can issue request_firmware(). Never a host-rootfs copy. */
	const struct kobox_linux_firmware_file *firmware;
	size_t firmware_count;
	/* Optional device attachment; readiness precedes lifecycle publication. */
	const struct kobox_linux_device_launch *device;
	/* Device-class-neutral PCI attachment. Mutually exclusive with device. */
	const struct kobox_linux_device_port_config *pci_devices;
	size_t pci_device_count;
	/* USB input events are captured after the upstream HID stack loads. */
	uint32_t capture_input;
	/* Ethernet-frame service for the single PCI NIC in this generation. */
	uint32_t capture_network;
	/* Block I/O shares the same PCI ownership as input for USB storage.
	 * Write access is an explicit launch-time policy, never inferred from
	 * whether Linux registered a writable disk. */
	uint32_t capture_block;
	uint32_t block_write;
	struct kobox_linux_block_key block_write_key;
	/* The storage service uses its own VFS data plane, never block RPC. */
	const struct kobox_linux_fs_mount_config *filesystem;
	/* Optional process-local progress callback. Called by the Linux opening
	 * task before potentially blocking driver work; it must not call Linux. */
	void (*progress)(void *context, unsigned phase, size_t module_index,
			 int status);
	void *progress_context;
	const struct kobox_linux_lifecycle *lifecycle;
};

struct kobox_linux_io_service {
	struct kobox_linux_input_port *input;
	struct kobox_linux_block_port *block;
	struct kobox_linux_fs_service *filesystem;
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
