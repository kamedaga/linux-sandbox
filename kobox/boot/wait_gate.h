/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_BOOT_WAIT_GATE_H
#define KOBOX_LINUX_BOOT_WAIT_GATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Process-local test diagnostics, not a host contract or wire ABI. */
struct kobox_linux_wait_report {
	size_t size;
	char api[48];
	char scenario[32];
	uint32_t cpu;
	uint32_t passed;
	uint32_t irq_callbacks;
	uint32_t failure_line;
	int64_t result;
	uint64_t elapsed_ns;
	uint64_t expected_switches;
	uint64_t observed_switches;
	uint32_t invalid;
	uint64_t warnings;
};

/* Failure requires immediate test-process exit; pending objects stay owned. */
int kobox_linux_wait_verify(struct kobox_linux_wait_report *report);

#endif
