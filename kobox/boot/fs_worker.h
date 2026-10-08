/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_WORKER_H
#define KOBOX_BOOT_FS_WORKER_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

struct kobox_linux_fs_service;
struct kobox_linux_fs_workers;
struct kobox_linux_fs_work;

/* Borrowed response and cookie remain pinned until this callback returns.
 * Called outside pool locks, after VFS refs/task_work, before collect can see
 * the job. The host must not release the job from inside this callback. */
typedef void (*kobox_linux_fs_publish_fn)(void *cookie,
	const void *response, size_t used, int error);

/* Process-local Linux task API, not a wire structure. Budgets include queued,
 * running and completed-but-unreleased jobs. Bytes account private input and
 * output leases plus cleared idle storage within the same total byte budget;
 * idle buffers are evicted before allocation, not charged to former clients.
 * Request counts separately bound fixed job/client metadata.
 * A client cannot occupy every worker. A finite pool can still exhaust its
 * execution slots if several clients block: admission is bounded and control
 * operations must remain outside this data pool, not wait for a data reply.
 */
struct kobox_linux_fs_worker_config {
	uint32_t workers;
	uint32_t client_running;
	uint32_t client_requests;
	uint32_t total_requests;
	uint64_t client_bytes;
	uint64_t total_bytes;
	uint64_t control_bytes;
	kobox_linux_fs_publish_fn publish;
};

/* All owner entries run on the creating Linux task. Workers only hand private
 * results to the optional host publisher; they never decode a shared request.
 * Completion still wakes the owner for bounded resource reclamation.
 * Revoke rejects further admission, but accepted jobs retain their original
 * authority and drain; client_busy stays true until completion is released.
 * Close rejects every new submission and drains queued/running work. Destroy
 * requires close, no outstanding jobs, then joins persistent Linux kthreads.
 */
int kobox_linux_fs_workers_create(struct kobox_linux_fs_service *service,
	const struct kobox_linux_fs_worker_config *config,
	struct kobox_linux_fs_workers **workers);
int kobox_linux_fs_workers_submit(struct kobox_linux_fs_workers *workers,
	uint64_t client, const void *input, size_t input_size,
	size_t response_capacity, void *cookie);
/* Independent persistent management task and one reserved request/byte lease.
 * Registry updates need an owner admission barrier for the affected client;
 * blocked exec/device VFS cannot consume data workers or block ring reception.
 */
int kobox_linux_fs_workers_submit_control(struct kobox_linux_fs_workers *workers,
	const void *input, size_t input_size, size_t response_capacity, void *cookie);
int kobox_linux_fs_workers_collect(struct kobox_linux_fs_workers *workers,
	struct kobox_linux_fs_work **work, void **cookie, const void **response,
	size_t *used, int *error);
int kobox_linux_fs_workers_release(struct kobox_linux_fs_workers *workers,
	struct kobox_linux_fs_work *work);
int kobox_linux_fs_workers_revoke(struct kobox_linux_fs_workers *workers,
	uint64_t client);
int kobox_linux_fs_workers_client_busy(struct kobox_linux_fs_workers *workers,
	uint64_t client);
int kobox_linux_fs_workers_close(struct kobox_linux_fs_workers *workers);
int kobox_linux_fs_workers_destroy(struct kobox_linux_fs_workers *workers);
#if defined(__KERNEL__) && defined(KOBOX_RUNTIME_GATES)
void kobox_linux_fs_workers_gate_fail_scope(void);
/* Enable/disable inline owner execution; report its hit/punt counts. */
void kobox_linux_fs_workers_gate_inline(struct kobox_linux_fs_workers *workers,
	bool enabled, u64 *hits, u64 *punts);
int kobox_linux_fs_workers_gate_cached(struct kobox_linux_fs_workers *workers,
	unsigned int *works, unsigned int *clients);
int kobox_linux_fs_workers_gate_storage(struct kobox_linux_fs_workers *workers,
	u64 *bytes, u64 *reused, u64 *evicted);
#endif
#if defined(__KERNEL__) && defined(KOBOX_FS_INTERNAL_BENCH)
#ifndef KOBOX_FS_BENCH_ELAPSED_ONLY
struct kobox_task_bench_counts;
void kobox_linux_fs_workers_boundaries(struct kobox_linux_fs_workers *workers,
	struct kobox_task_bench_counts *counts);
#ifdef KOBOX_FS_BOUNDARY_CALLERS
void kobox_linux_fs_workers_trace(struct kobox_linux_fs_workers *workers, bool enabled);
#endif
#endif
uint64_t kobox_linux_fs_workers_runtime(struct kobox_linux_fs_workers *workers);
uint64_t kobox_linux_fs_workers_switches(struct kobox_linux_fs_workers *workers);
void kobox_linux_fs_workers_completion_counts(struct kobox_linux_fs_workers *workers,
	uint64_t *wakes, uint64_t *batches);
#ifndef KOBOX_FS_BENCH_ELAPSED_ONLY
void kobox_linux_fs_workers_profile_reset(struct kobox_linux_fs_workers *workers);
void kobox_linux_fs_workers_profile_report(struct kobox_linux_fs_workers *workers);
#endif
#endif

#endif /* KOBOX_BOOT_FS_WORKER_H */
