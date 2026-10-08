/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_PORT_GATE_H
#define KOBOX_BOOT_FS_PORT_GATE_H

#include "fs_workload.h"

/* Process-local, test-only image exchange. The host owns a writable mapping
 * of a disposable image; production storage never enters through this path.
 */
struct kobox_fs_image {
	void *data;
	size_t length;
};

struct kobox_fs_port_report {
	struct fs_test_report ext4;
	struct fs_test_report tmpfs;
	uint32_t stage;
	uint32_t line;
	int32_t result;
	uint32_t handles;
	uint32_t shared_reads;
	uint32_t cpu_mask;
	uint32_t close_races;
	uint64_t warnings;
};

int kobox_linux_fs_port_verify(const struct kobox_fs_image *image,
			       struct kobox_fs_port_report *report);

#endif /* KOBOX_BOOT_FS_PORT_GATE_H */
