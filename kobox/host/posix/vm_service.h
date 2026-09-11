/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_VM_SERVICE_H
#define KOBOX_POSIX_VM_SERVICE_H

#include "vm.h"

struct kobox_posix_vm_service;
struct kobox_posix_vm_remote;

struct kobox_posix_vm_completion {
	struct kobox_posix_vm_event event;
	uint64_t sequence;
	uint64_t value;
	int error;
};

/* Create before other threads so SIGCHLD remains blocked in all of them.
 * Destroy on the creator, after all callers have joined and remotes closed.
 * A closed remote stays allocated until service destruction: delayed calls
 * cannot accidentally target a recycled PID or a new remote object.
 * notify runs on the host service, never in a Linux execution domain.
 */
int kobox_posix_vm_service_create(struct kobox_posix_vm_service **out,
	void (*notify)(void *), void *context);
int kobox_posix_vm_service_destroy(struct kobox_posix_vm_service *service);
/* Read-only: success requires every tracked context to be closed and reaped. */
int kobox_posix_vm_service_quiescent(struct kobox_posix_vm_service *service);
/* Conformance only: one-shot checkpoint after an actual user fault reached
 * the machine map operation, before publishing its native mapping. The
 * callback must not return; no mapping success is synthesized by this hook.
 */
int kobox_posix_vm_service_fault_checkpoint(struct kobox_posix_vm_service *service,
	uint64_t address, int (*checkpoint)(void *), void *context);
int kobox_posix_vm_remote_create(struct kobox_posix_vm_service *service,
	const char *client, struct kobox_posix_memory_backing *ram,
	uint64_t window_start, size_t window_size,
	struct kobox_posix_vm_remote **out, pid_t *pid);
/* The child starts stopped at operation sequence zero, with the supplied
 * syscall sequence pending. The caller supplies Linux's child return frame
 * before resuming it. Closing the parent does not close the child.
 */
int kobox_posix_vm_remote_clone(struct kobox_posix_vm_remote *parent,
	uint64_t sequence, uint64_t syscall_sequence, bool share_mm,
	struct kobox_posix_vm_remote **out, pid_t *pid, struct kobox_x86_fp_state *fp);
int kobox_posix_vm_remote_close(struct kobox_posix_vm_remote *remote);
int kobox_posix_vm_remote_map(struct kobox_posix_vm_remote *remote,
	uint64_t address, size_t offset, size_t size, unsigned int protection);
int kobox_posix_vm_remote_reset(struct kobox_posix_vm_remote *remote,
	uint64_t address, size_t size);
int kobox_posix_vm_remote_resume(struct kobox_posix_vm_remote *remote, uint64_t sequence);
int kobox_posix_vm_remote_enable_syscalls(struct kobox_posix_vm_remote *remote);
int kobox_posix_vm_remote_start(struct kobox_posix_vm_remote *remote,
	const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp);
int kobox_posix_vm_remote_write_fpregs(struct kobox_posix_vm_remote *remote,
	uint64_t sequence, const struct kobox_x86_fp_state *fp);
int kobox_posix_vm_remote_snapshot(struct kobox_posix_vm_remote *remote,
	uint64_t sequence, struct kobox_x86_user_regs *registers, struct kobox_x86_fp_state *fp);
int kobox_posix_vm_remote_restore(struct kobox_posix_vm_remote *remote,
	uint64_t sequence, const struct kobox_x86_user_regs *registers,
	const struct kobox_x86_fp_state *fp);
int kobox_posix_vm_remote_syscall_return(struct kobox_posix_vm_remote *remote,
	uint64_t sequence, uint64_t syscall_sequence,
	const struct kobox_x86_user_regs *registers);
int kobox_posix_vm_remote_event(struct kobox_posix_vm_remote *remote,
	struct kobox_posix_vm_completion *event);

/* Trusted machine-test client command, not a guest mapping policy or wire API.
 * Configures an access only at READY/DONE, then starts it asynchronously.
 */
int kobox_posix_vm_remote_probe(struct kobox_posix_vm_remote *remote,
	uint64_t address, unsigned int write, uint64_t value, uint64_t sequence);
/* Configures the trusted test program to execute a real syscall instruction.
 * No Linux FD operation or syscall implementation lives in this service.
 */
int kobox_posix_vm_remote_syscall_probe(struct kobox_posix_vm_remote *remote,
	uint64_t number, const uint64_t arguments[6], uint64_t sequence);

#endif
