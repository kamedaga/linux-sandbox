/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_BOOT_CLEANUP_GATE_H
#define KOBOX_LINUX_BOOT_CLEANUP_GATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Process-local diagnostics, not a host or wire ABI. */
struct kobox_linux_cleanup_report {
	size_t size;
	char scenario[24];
	uint32_t cpu;
	uint32_t phase;
	uint32_t line;
	/* Atomic callback/probe timeout snapshots; UINT32_MAX means no snapshot.
	 * Sleeping callback timeouts are recorded only in errors.
	 */
	uint32_t hold_phase;
	uint32_t hold_work_active;
	uint32_t hold_delayed_active;
	uint32_t probe_timeout_phase;
	uint32_t probe_calls;
	uint32_t probe_phase;
	uint32_t probe_cleaner;
	uint32_t probe_active;
	uint32_t cases;
	uint32_t callbacks;
	uint32_t rejected;
	uint32_t probes;
	uint32_t freed;
	uint32_t errors;
	uint64_t warnings;
};

/* Failure retains asynchronous storage and requires immediate process exit. */
int kobox_linux_cleanup_verify(struct kobox_linux_cleanup_report *report);

#endif
