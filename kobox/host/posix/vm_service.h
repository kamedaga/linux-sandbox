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
int kobox_posix_vm_remote_create(struct kobox_posix_vm_service *service,
	const char *client, struct kobox_posix_memory_backing *ram,
	struct kobox_posix_vm_remote **out, pid_t *pid);
int kobox_posix_vm_remote_close(struct kobox_posix_vm_remote *remote);
int kobox_posix_vm_remote_map(struct kobox_posix_vm_remote *remote,
	uint64_t address, size_t offset, size_t size, unsigned int protection);
int kobox_posix_vm_remote_reset(struct kobox_posix_vm_remote *remote,
	uint64_t address, size_t size);
int kobox_posix_vm_remote_resume(struct kobox_posix_vm_remote *remote, uint64_t sequence);
int kobox_posix_vm_remote_event(struct kobox_posix_vm_remote *remote,
	struct kobox_posix_vm_completion *event);

/* Trusted machine-test client command, not a guest mapping policy or wire API.
 * Configures an access only at READY/DONE, then starts it asynchronously.
 */
int kobox_posix_vm_remote_probe(struct kobox_posix_vm_remote *remote,
	uint64_t address, unsigned int write, uint64_t value, uint64_t sequence);

#endif
