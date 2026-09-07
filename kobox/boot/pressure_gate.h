/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_PRESSURE_GATE_H
#define KOBOX_LINUX_PRESSURE_GATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Process-local test diagnostics, not a host or wire ABI. */
struct kobox_linux_pressure_report {
	size_t size;
	uint32_t phase;
	uint32_t line;
	int32_t result;
	uint32_t pressure_pages;
	uint32_t kswapd_freed;
	uint32_t direct_freed;
	uint32_t cache_reused;
	uint32_t dentries_freed;
	uint32_t retained;
	uint32_t failures;
	uint32_t recovered;
	uint32_t allocation_case;
	uint32_t fail_nth;
	uint32_t injected;
	uint32_t rollbacks;
	uint32_t sweeps;
	uint64_t warnings;
};

/* Runs after normal upstream boot. A failed assertion requires process exit. */
int kobox_linux_pressure_verify(struct kobox_linux_pressure_report *report);
int kobox_linux_pressure_inspect(struct kobox_linux_pressure_report *report,
				 int (*inspect)(void *), void *argument);
/* Expected accounting change of the caller's own buffers, fixed before the
 * pressure begins. This is not an observed difference used to forgive leaks.
 */
int kobox_linux_pressure_mutate(struct kobox_linux_pressure_report *report,
		int (*inspect)(void *), void *argument, long committed_delta);
int kobox_linux_allocation_verify(struct kobox_linux_pressure_report *report);

#endif
