/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_USER_PROCESS_H
#define KOBOX_LINUX_USER_PROCESS_H

#include "../mm/host.h"

struct kobox_linux_user_process;

/* Absolute, normalized regular-file paths; no links, devices or special bits.
 * Mode is exactly 0600, 0644 or 0755. argv[0] must name a supplied 0755 file.
 * Supervisor copies of 0600 files are erased before freeing them.
 */
struct kobox_linux_user_process_file {
	const char *path;
	const void *data;
	size_t size;
	uint32_t mode;
};

struct kobox_linux_user_process_config {
	size_t size;
	const struct kobox_linux_vm_host_operations *vm_operations;
	void *host_space;
	uint64_t start, length;
	const struct kobox_linux_user_process_file *files;
	size_t file_count;
	const char *const *argv;
	const char *const *envp;
	/* Linux CAP_* bit positions; only NET_ADMIN and NET_RAW are accepted. */
	uint64_t capability_mask;
};

struct kobox_linux_user_process_status {
	size_t size;
	int32_t pid;
	uint32_t exec_ready, exited, reaped, host_closed;
	int32_t launch_error, wait_status, exit_code, term_signal;
};

#ifdef __KERNEL__
/* Single-owner API: all operations run on the Linux task that spawned the
 * instance, with IRQs enabled, outside atomic context. Its SIGCHLD disposition
 * must remain default, without SA_NOCLDWAIT; no other waiter may reap this PID.
 *
 * Spawn copies files/strings/operations and starts asynchronously. Success
 * transfers host_space's CLOSE responsibility, not its backing allocation.
 * Caller must not reuse/free that storage until host_closed and destroy.
 * Failure with *out == NULL leaves host_space owned by the caller. A rare
 * post-clone failure returns a non-NULL handle that must still be stopped.
 * Operations' code and the host storage remain live until destroy succeeds.
 *
 * The instance has its own mount namespace, tmpfs root (64 MiB), RO sysfs,
 * /dev/null, /dev/random and /dev/urandom, and no inherited Linux FDs. stdout/stderr share
 * an 8 MiB limited log; stdin is /dev/null. Files total at most 64 MiB.
 * This foreground single-task runner rejects VM clone, including fork,
 * threads and subsequent exec replacement; the initial ELF uses normal exec.
 * It shares the runtime's network namespace, not a private empty netns.
 */
int kobox_linux_user_process_spawn(
	const struct kobox_linux_user_process_config *config,
	struct kobox_linux_user_process **out);
int kobox_linux_user_process_status(struct kobox_linux_user_process *process,
	struct kobox_linux_user_process_status *status);
int kobox_linux_user_process_read_log(struct kobox_linux_user_process *process,
	uint64_t offset, void *bytes, size_t capacity, size_t *read);
/* Atomically replace one of the supplied 0600 files (at most 8 KiB).
 * Failed staging/rename leaves the old path intact; caller may issue the
 * application's reconfigure command only after success. No namespace switch.
 */
int kobox_linux_user_process_replace_file(struct kobox_linux_user_process *process,
	const char *path, const void *bytes, size_t length);
/* Natural-exit wait, without sending a signal. 1..60000 ms; timeout/error
 * retains every resource and permits a later wait or explicit stop.
 */
int kobox_linux_user_process_wait(struct kobox_linux_user_process *process,
	uint32_t timeout_ms);
/* Each deadline is 1..60000 ms. Timeout/error retains the handle and all
 * resources: never destroy a task or binding merely because time expired.
 * Success proves kernel_wait, TASK_DEAD, no mm/files and retired binding.
 */
int kobox_linux_user_process_stop(struct kobox_linux_user_process *process,
	uint32_t term_timeout_ms, uint32_t kill_timeout_ms);
/* Never implicitly kill a live instance; -EBUSY/error retains ownership. */
int kobox_linux_user_process_destroy(struct kobox_linux_user_process *process);
#endif

#endif
