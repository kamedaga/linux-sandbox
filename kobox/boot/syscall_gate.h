/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_SYSCALL_GATE_H
#define KOBOX_SYSCALL_GATE_H

#include "vm_gate.h"

struct vfsmount;

struct kobox_syscall_test {
	size_t size;
	const struct kobox_linux_vm_test *vm;
	int (*issue)(void *space, uint64_t number,
		     const uint64_t arguments[6], uint64_t sequence);
};

struct kobox_syscall_report {
	size_t size;
	uint32_t calls, faults, tls, nested, invalid, descriptors, exited, reclaimed;
	uint32_t transfers, truncated, queued_exit;
	uint32_t rendezvous, race_sent, race_rejected;
	uint32_t inherited, cow, binding_rollbacks;
	uint32_t native_forks, shared_clones, threads, group_exits;
	uint32_t autonomous;
	uint32_t drm_files, gem_handles;
	uint32_t cpu_mask, line;
	uint64_t number, warnings;
	int64_t returned;
	int32_t result;
};

/* Borrowed fixture mount; requests still enter through real user syscalls. */
int kobox_linux_drm_syscall_verify(const struct kobox_syscall_test *host,
	struct kobox_syscall_report *report, struct vfsmount *mount);
int kobox_linux_drm_rights_verify(const struct kobox_syscall_test *host,
	struct kobox_syscall_report *report, struct vfsmount *mount);

#endif
