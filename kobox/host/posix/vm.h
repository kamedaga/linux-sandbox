/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_VM_H
#define KOBOX_POSIX_VM_H

#include "host.h"
#include "vm_client.h"
#include "../../arch/x86_64/user.h"

#include <sys/types.h>

enum kobox_posix_vm_event_kind {
	KOBOX_POSIX_VM_STOP,
	KOBOX_POSIX_VM_FAULT,
	KOBOX_POSIX_VM_SYSCALL,
	KOBOX_POSIX_VM_EXIT,
	KOBOX_POSIX_VM_PAUSED,
};

struct kobox_posix_vm_group;

/* Register values from this tracee, not pointers into the runtime. */
struct kobox_posix_vm_syscall {
	uint64_t sequence;
	uint64_t number;
	uint64_t arguments[6];
};

struct kobox_posix_vm_event {
	enum kobox_posix_vm_event_kind kind;
	struct kobox_posix_vm_syscall syscall;
	struct kobox_x86_user_regs user;
	uint64_t address;
	uint64_t ip;
	uint64_t sp;
	uint64_t flags;
	uint64_t error;
	int exit_status;
};

/* All ptrace operations run on the creating native thread. A machine service
 * may serialize calls for guest CPUs; no guest progress is needed for a map
 * or invalidation to finish. destroy() succeeds only after actual reaping.
 */
struct kobox_posix_vm {
	struct kobox_posix_vm_group *group;
	struct kobox_posix_vm *group_next;
	pid_t pid;
	pid_t owner;
	uint64_t syscall_entry;
	uint64_t control_address;
	size_t ram_size;
	uint64_t window_start;
	size_t window_size;
	struct kobox_posix_memory_backing control_backing;
	struct kobox_vm_client_control *control;
	bool running;
	bool capturing_fault;
	bool signal_return;
	bool signal_return_allowed;
	bool pause_requested;
	bool event_pending;
	bool syscalls_enabled;
	bool user_started;
	bool syscall_pending;
	bool syscall_failed;
	uint64_t syscall_sequence;
	struct kobox_posix_vm_event pending_event;
	int exit_status;
};

/* Reserve caller-selected user VA without replacing existing host mappings.
 * The Linux MM binding must describe this same interval. Bounds stay in the
 * owning service, not in client-writable bootstrap state, and clone inherits
 * them along with the actual host MM. Invalid ranges create no process.
 */
int kobox_posix_vm_create(struct kobox_posix_vm *space, const char *client,
			 struct kobox_posix_memory_backing *ram,
			 uint64_t window_start, size_t window_size);
int kobox_posix_vm_destroy(struct kobox_posix_vm *space);
/* Clone only at a captured syscall's stable return stop. The child remains
 * stopped, with independent bootstrap storage and the same pending sequence.
 * Its native parent is this owner thread; Linux owns the guest process tree.
 * No other thread may create/reap this owner's native children during a call.
 * Failure reaps every native child created by the attempted clone.
 * share_mm selects real native CLONE_VM, with a distinct control/altstack.
 * The caller must supply an appropriate guest stack before resuming it.
 * Native group identity does not implement Linux CLONE_THREAD semantics.
 */
int kobox_posix_vm_clone(struct kobox_posix_vm *parent, uint64_t sequence,
			bool share_mm, struct kobox_posix_vm *child);
int kobox_posix_vm_map(struct kobox_posix_vm *space, uint64_t address,
		      size_t offset, size_t size, unsigned int protection);
int kobox_posix_vm_protect(struct kobox_posix_vm *space, uint64_t address,
			  size_t size, unsigned int protection);
int kobox_posix_vm_reset(struct kobox_posix_vm *space, uint64_t address, size_t size);
int kobox_posix_vm_resume(struct kobox_posix_vm *space);
/* Enable only at the trusted bootstrap stop. A captured syscall is skipped
 * in the host kernel and remains stopped until its exact reply is supplied.
 * Reply restores user registers; resume is a separate operation.
 */
int kobox_posix_vm_enable_syscalls(struct kobox_posix_vm *space);
/* Install the initial user frame at a fresh READY stop. No host-side ELF
 * interpretation or task/FD setup is performed. Resume remains separate.
 */
int kobox_posix_vm_start(struct kobox_posix_vm *space,
	const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp);
int kobox_posix_vm_write_fpregs(struct kobox_posix_vm *space,
	const struct kobox_x86_fp_state *state);
int kobox_posix_vm_syscall_return(struct kobox_posix_vm *space,
	uint64_t sequence, const struct kobox_x86_user_regs *registers);
int kobox_posix_vm_read_registers(struct kobox_posix_vm *space,
	struct kobox_x86_user_regs *registers);
int kobox_posix_vm_read_fpregs(struct kobox_posix_vm *space,
	struct kobox_x86_fp_state *state);
/* Stops actual user execution, preserving a fault/completion which won the
 * race. map/protect/reset use this internally and restore runnable state.
 */
int kobox_posix_vm_pause(struct kobox_posix_vm *space);
/* Stop at a client frame. A racing event remains queued and returns EBUSY.
 * Native fault-handler return is drained before reading the client state.
 */
int kobox_posix_vm_snapshot(struct kobox_posix_vm *space,
	struct kobox_x86_user_regs *registers, struct kobox_x86_fp_state *fp);
int kobox_posix_vm_restore(struct kobox_posix_vm *space,
	const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp);
/* EAGAIN means no event yet, not process death or operation completion. */
int kobox_posix_vm_wait(struct kobox_posix_vm *space, bool nonblock,
		       struct kobox_posix_vm_event *event);

#endif
