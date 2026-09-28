// SPDX-License-Identifier: GPL-2.0-only
#ifndef KOBOX_BOOT_FIRMWARE_FILES_H
#define KOBOX_BOOT_FIRMWARE_FILES_H

#include <linux/types.h>
#ifndef __KERNEL__
#include <stddef.h>
#endif

#define KOBOX_FIRMWARE_MAX_FILES 64U

/* Immutable, owner-verified bytes borrowed until the module lifecycle ends.
 * Names are paths relative to Linux's /lib/firmware, never host paths. */
struct kobox_linux_firmware_file {
	const char *name;
	const void *data;
	size_t size;
	unsigned char sha256[32];
};

struct kobox_linux_firmware_store;

int kobox_linux_firmware_install(const struct kobox_linux_firmware_file *files,
				 size_t count,
				 struct kobox_linux_firmware_store **out);
int kobox_linux_firmware_remove(struct kobox_linux_firmware_store **store);

#endif
