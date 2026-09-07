/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_VFS_GATE_H
#define KOBOX_LINUX_VFS_GATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Process-local diagnostics, not a host or wire ABI. */
struct kobox_linux_vfs_report {
	size_t size;
	uint32_t cases;
	uint32_t mounts;
	uint32_t io_checks;
	uint32_t linked_reopens;
	uint32_t negative_checks;
	uint32_t task_work_cases;
	uint32_t delayed_fput_cases;
	uint32_t rcu_holds;
	uint32_t file_reclaims;
	uint32_t inode_reclaims;
	uint32_t folio_reclaims;
	uint32_t super_reclaims;
	uint32_t cpu;
	uint32_t deferred;
	uint32_t unlink_first;
	uint32_t phase;
	uint32_t line;
	int32_t result;
	uint64_t warnings;
};

/* No subsystem initialization. Failure requires immediate process exit. */
int kobox_linux_vfs_verify(struct kobox_linux_vfs_report *report);

#endif
