/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_BOOT_MEMORY_GATE_H
#define KOBOX_LINUX_BOOT_MEMORY_GATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Process-local diagnostics, not a host or wire ABI. */
struct kobox_linux_boot_memory_report {
	size_t size;
	uint32_t phase;
	uint32_t line;
	uint32_t cpu;
	uint32_t page_cases;
	uint32_t heap_cases;
	uint32_t slab_cases;
	uint32_t percpu_cases;
	uint32_t dynamic_vmap_allocations;
	uint32_t alias_cases;
	uint32_t parent_permission_cases;
	uint32_t shmem_cases;
	uint32_t kswapd_ready;
	uint64_t warnings;
};

/* No subsystem initialization. Failure requires immediate process exit. */
int kobox_linux_boot_memory_verify(struct kobox_linux_boot_memory_report *report);

#endif
