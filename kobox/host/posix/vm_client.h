/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_VM_CLIENT_H
#define KOBOX_POSIX_VM_CLIENT_H

#include <stdint.h>
#include <stdatomic.h>

#define KOBOX_VM_RAM_FD 3
#define KOBOX_VM_CONTROL_FD 4
#define KOBOX_VM_WINDOW_BASE UINT64_C(0x4000000000)
#define KOBOX_VM_WINDOW_SIZE (16UL * 4096)

enum kobox_vm_client_event {
	KOBOX_VM_CLIENT_READY = 1,
	KOBOX_VM_CLIENT_RUNNING,
	KOBOX_VM_CLIENT_FAULT,
	KOBOX_VM_CLIENT_DONE,
};

/* POSIX machine-test bootstrap, not a controller/DRM wire protocol. All
 * addresses belong to the tracee; no pointer to core or Linux state is sent.
 */
struct kobox_vm_client_control {
	uint64_t syscall_entry;
	uint64_t address;
	uint64_t value;
	uint64_t result;
	uint64_t fault_address;
	uint64_t fault_ip;
	uint64_t fault_sp;
	uint64_t fault_flags;
	uint64_t fault_error;
	uint32_t write;
	_Atomic(uint32_t) event;
	_Atomic(uint32_t) attached;
	_Atomic(uint32_t) loop;
	_Atomic(uint64_t) iterations;
};

void kobox_vm_syscall_entry(void);

#endif
