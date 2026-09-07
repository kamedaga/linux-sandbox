/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_BOOT_SERVICE_GATE_H
#define KOBOX_LINUX_BOOT_SERVICE_GATE_H

#include "host.h"

/* Test results, not a runtime service or wire ABI. */
struct kobox_linux_boot_report {
	size_t size;
	uint32_t phase;
	uint32_t irq_callbacks[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint32_t tasklet_runs[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint32_t irq_exit_tasklet_runs[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint32_t ksoftirqd_runs[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint32_t work_runs[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint32_t rcu_callbacks;
	uint32_t rcu_threads;
	uint32_t worker_threads;
	uint64_t warnings;
};

/* Run from PID 1 after boot. On failure the test process must terminate;
 * any outstanding asynchronous test objects remain allocated until then.
 */
int kobox_linux_boot_verify(struct kobox_linux_boot_report *report);

#endif
