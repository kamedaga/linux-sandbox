/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_DIAGNOSTIC_H
#define KOBOX_TASK_DIAGNOSTIC_H

#include <linux/types.h>

#if defined(KOBOX_FS_BENCH_ELAPSED_ONLY) && \
    (defined(KOBOX_FS_SERVICE_PROFILE) || defined(KOBOX_FS_BOUNDARY_CALLERS))
#error "elapsed-only benchmark cannot contain detailed probes"
#endif

/* Keep the benchmark workload without its observer: otherwise every hosted
 * boundary adds an atomic counter even when only total latency is wanted.
 */
#if defined(KOBOX_FS_INTERNAL_BENCH) && !defined(KOBOX_FS_BENCH_ELAPSED_ONLY)
#define KOBOX_FS_INTERNAL_PROFILE 1
#endif

#ifdef KOBOX_FS_INTERNAL_PROFILE
struct task_struct;

enum kobox_task_bench_kind {
	KOBOX_TASK_BENCH_PREEMPT,
	KOBOX_TASK_BENCH_IRQ,
	KOBOX_TASK_BENCH_CLOCK,
	KOBOX_TASK_BENCH_HANDOFF,
	KOBOX_TASK_BENCH_COUNT,
};

/* Core-private observations, not host operations or wire fields. The caller
 * pins each task until both snapshots have completed. Count interrupt entry
 * on that task too; do not confuse these with native syscall counts.
 */
struct kobox_task_bench_counts {
	u64 calls[KOBOX_TASK_BENCH_COUNT];
};

void kobox_task_bench_read(struct task_struct *task,
			   struct kobox_task_bench_counts *counts);
#ifdef KOBOX_FS_BOUNDARY_CALLERS
void kobox_task_bench_trace(struct task_struct *task, bool enabled);
void kobox_task_bench_sites_reset(void);
void kobox_task_bench_sites_report(const char *label, u64 ops);
#endif
#endif

#endif /* KOBOX_TASK_DIAGNOSTIC_H */
