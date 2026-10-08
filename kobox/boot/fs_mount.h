/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_MOUNT_H
#define KOBOX_BOOT_FS_MOUNT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

struct kobox_linux_fs_mount;
struct kobox_linux_fs_service;
struct pci_dev;

/* Process-local immutable launch policy. UUID probing may read only block
 * devices under the granted PCI ancestry; no candidate is mounted to inspect
 * it. One namespace owns the selected root and its tmpfs submounts. */
struct kobox_linux_fs_mount_config {
	size_t size;
	uint64_t generation;
	uint8_t root_uuid[16];
};

#ifdef __KERNEL__
int kobox_linux_fs_mount_open(struct kobox_linux_fs_mount **out,
	const struct kobox_linux_fs_mount_config *config,
	struct pci_dev * const *devices, size_t device_count);
struct kobox_linux_fs_service *
kobox_linux_fs_mount_service(struct kobox_linux_fs_mount *mount);
/* Dispatch has quiesced before close; upstream synchronization/unmount runs
 * before any storage driver/module is released. Failures retain ownership. */
int kobox_linux_fs_mount_close(struct kobox_linux_fs_mount *mount);
#endif

#endif
