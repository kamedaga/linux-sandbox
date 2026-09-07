/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_BOOT_RCU_GATE_H
#define KOBOX_LINUX_BOOT_RCU_GATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Process-local test results, not a runtime or wire ABI. */
struct kobox_linux_rcu_report {
	size_t size;
	char flavor[24];
	char scenario[24];
	uint32_t cpu;
	uint32_t expedited;
	uint32_t phase;
	uint32_t failure_line;
	uint32_t passed;
	uint32_t normal_gps;
	uint32_t expedited_gps;
	uint32_t callbacks;
	uint32_t barrier_probes;
	uint32_t reclaimed;
	uint32_t preemptions;
	uint32_t migrations;
	uint32_t idle_observations;
	uint32_t errors;
	uint64_t warnings;
};

/* Failure retains asynchronous objects; the test process must exit at once. */
int kobox_linux_rcu_verify(struct kobox_linux_rcu_report *report);

#endif
