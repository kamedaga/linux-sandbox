/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_EXECUTOR_H
#define KOBOX_BOOT_FS_EXECUTOR_H

#include <linux/types.h>

struct kobox_linux_fs_service;
struct kobox_linux_fs_executors;
struct kobox_linux_fs_lane;

/* Process-local prototype, not a transport ABI. take borrows an input lease
 * until its optional snapshot_done notice, otherwise through release.
 * publish and transport-ticket release share one host boundary, then the
 * executor scrubs its private Linux storage and returns the budget. The lane
 * execution ref still pins the binding throughout all of these steps.
 * No callback may call Linux APIs.
 * The trusted binding, not the returned bytes, selects the authority.
 * take returns 0 if empty, 1 for the last known item, 2 if more are queued.
 * A negative take stops this lane; no consumed request is ever retried.
 */
struct kobox_linux_fs_intake {
	const void *input;
	size_t input_size, response_capacity;
	void *cookie;
	/* Supplied by the trusted source, never decoded from peer bytes. A ring
	 * that keeps input pinned until used publication needs no early notice. */
	void (*snapshot_done)(void *cookie);
};

struct kobox_linux_fs_executor_config {
	u32 workers, client_running;
	u64 client_bytes, total_bytes;
	int (*take)(void *binding, struct kobox_linux_fs_intake *item);
	void (*publish)(void *cookie, const void *response, size_t used, int error);
	void (*release)(void *cookie);
};

int kobox_linux_fs_executors_create(struct kobox_linux_fs_service *service,
	const struct kobox_linux_fs_executor_config *config,
	struct kobox_linux_fs_executors **executors);
/* Owner-only management. Binding must outlive pause/drain and unregister.
 * pause is an admission barrier: it stops new intake, then waits through
 * snapshot, VFS, publication, release and budget return. Registry changes
 * happen only between pause and resume. Queued but untaken bytes have not
 * acquired credentials; after resume they acquire the updated credentials.
 */
int kobox_linux_fs_executors_register(struct kobox_linux_fs_executors *executors,
	u64 authenticated_client, void *binding, struct kobox_linux_fs_lane **lane);
int kobox_linux_fs_executors_pause(struct kobox_linux_fs_lane *lane);
int kobox_linux_fs_executors_resume(struct kobox_linux_fs_lane *lane);
int kobox_linux_fs_executors_unregister(struct kobox_linux_fs_lane *lane);
/* kick is concurrent-safe, including while a worker owns the intake lease.
 * Input leases and native pending descriptors are bounded by the source queue;
 * the executor bounds private requests by workers and charged byte budgets.
 */
int kobox_linux_fs_executors_kick(struct kobox_linux_fs_lane *lane);
int kobox_linux_fs_executors_wait_idle(struct kobox_linux_fs_executors *executors);
int kobox_linux_fs_executors_destroy(struct kobox_linux_fs_executors *executors);
#ifdef KOBOX_FS_INTERNAL_BENCH
u64 kobox_linux_fs_executors_switches(struct kobox_linux_fs_executors *executors);
void kobox_linux_fs_executors_counts(struct kobox_linux_fs_executors *executors,
	u64 *taken, u64 *released, u64 *wakes);
int kobox_linux_fs_executors_check_drained(struct kobox_linux_fs_executors *executors);
#endif

#endif
