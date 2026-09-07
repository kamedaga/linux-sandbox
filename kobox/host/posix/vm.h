/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_VM_H
#define KOBOX_POSIX_VM_H

#include "host.h"
#include "vm_client.h"

#include <sys/types.h>

enum kobox_posix_vm_event_kind {
	KOBOX_POSIX_VM_STOP,
	KOBOX_POSIX_VM_FAULT,
	KOBOX_POSIX_VM_EXIT,
	KOBOX_POSIX_VM_PAUSED,
};

struct kobox_posix_vm_event {
	enum kobox_posix_vm_event_kind kind;
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
	pid_t pid;
	pid_t owner;
	uint64_t syscall_entry;
	size_t ram_size;
	struct kobox_posix_memory_backing control_backing;
	struct kobox_vm_client_control *control;
	bool running;
	bool capturing_fault;
	bool signal_return;
	bool signal_return_allowed;
	bool pause_requested;
	bool event_pending;
	struct kobox_posix_vm_event pending_event;
	int exit_status;
};

int kobox_posix_vm_create(struct kobox_posix_vm *space, const char *client,
			 struct kobox_posix_memory_backing *ram);
int kobox_posix_vm_destroy(struct kobox_posix_vm *space);
int kobox_posix_vm_map(struct kobox_posix_vm *space, uint64_t address,
		      size_t offset, size_t size, unsigned int protection);
int kobox_posix_vm_protect(struct kobox_posix_vm *space, uint64_t address,
			  size_t size, unsigned int protection);
int kobox_posix_vm_reset(struct kobox_posix_vm *space, uint64_t address, size_t size);
int kobox_posix_vm_resume(struct kobox_posix_vm *space);
/* Stops actual user execution, preserving a fault/completion which won the
 * race. map/protect/reset use this internally and restore runnable state.
 */
int kobox_posix_vm_pause(struct kobox_posix_vm *space);
/* EAGAIN means no event yet, not process death or operation completion. */
int kobox_posix_vm_wait(struct kobox_posix_vm *space, bool nonblock,
		       struct kobox_posix_vm_event *event);

#endif
