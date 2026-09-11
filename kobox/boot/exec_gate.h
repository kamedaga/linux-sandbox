/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_EXEC_GATE_H
#define KOBOX_EXEC_GATE_H

#include "vm_gate.h"

struct kobox_exec_node {
	const char *name;
	uint32_t major, minor, mode;
};

struct file;

/* Explicit fixture inputs, copied into tmpfs; never forwarded as host FDs. */
struct kobox_exec_file {
	const char *path;
	const void *data;
	size_t length;
};

struct kobox_exec_test {
	size_t size;
	const struct kobox_linux_vm_test *vm;
	const void *image;
	size_t length;
	const struct kobox_exec_file *files;
	size_t file_count;
	const struct kobox_exec_node *nodes;
	size_t node_count;
	/* In-process fixture supervision while the real ELF client runs. */
	int (*observe)(void *context, struct file *result, unsigned int cpu, int child_pid);
	void *observe_context;
};

/* Fixture result file contents; not a controller or syscall wire format. */
struct kobox_exec_result {
	uint64_t pid, phase, cpu;
};

#define KOBOX_EXEC_FAILURE_OFFSET 512
#define KOBOX_EXEC_DEVICE_OFFSET 1024
#define KOBOX_EXEC_REVOKE_ADDRESS 0x400000000ULL

/* Shared-render fixture: a supervisor verifies these real poll waiters. */
#define KOBOX_EXEC_FENCE_WAIT_OFFSET 1088
struct kobox_exec_fence_wait {
	uint32_t pid, fd;
};

struct kobox_exec_failure {
	uint64_t pid, line;
	int64_t error;
};

struct kobox_exec_report {
	size_t size;
	uint32_t entered, exited, reclaimed, cpu_mask, phase, line;
	int32_t result, program_status;
	uint64_t warnings;
	struct kobox_exec_failure user_failure;
	char diagnostics[2048];
};

#ifdef __KERNEL__
/* Refresh a test namespace's node from an actual newly registered devt. */
int kobox_linux_exec_replace_node(struct file *namespace_file,
				  const struct kobox_exec_node *node);
int kobox_linux_exec_verify(const struct kobox_exec_test *host,
			   struct kobox_exec_report *report);
#endif

#endif
