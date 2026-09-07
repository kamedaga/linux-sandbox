/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_SHMEM_GATE_H
#define KOBOX_LINUX_SHMEM_GATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Process-local diagnostics, not a host or wire ABI. */
struct kobox_linux_shmem_report {
	size_t size;
	uint32_t cases;
	uint32_t io_checks;
	uint32_t accounting_checks;
	uint32_t sharing_checks;
	uint32_t negative_checks;
	uint32_t increments;
	uint32_t race_rounds;
	uint32_t locked_waits;
	uint32_t page_reclaims;
	uint32_t rollbacks;
	uint32_t cpu;
	uint32_t noreserve;
	uint32_t phase;
	uint32_t line;
	int32_t result;
	uint64_t warnings;
};

/* No subsystem initialization. Failure requires immediate process exit. */
int kobox_linux_shmem_verify(struct kobox_linux_shmem_report *report);

#endif
