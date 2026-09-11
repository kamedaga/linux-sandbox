/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_VM_CLIENT_H
#define KOBOX_POSIX_VM_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#define KOBOX_VM_RAM_FD 3
#define KOBOX_VM_CONTROL_FD 4
/* Defaults for diagnostics only; production mapping bounds are per MM. */
#define KOBOX_VM_TEST_WINDOW_BASE UINT64_C(0x4000000000)
#define KOBOX_VM_TEST_WINDOW_SIZE (16UL * 4096)
#define KOBOX_VM_CLIENT_PAGE_SIZE 4096UL
#define KOBOX_VM_CLIENT_STACK_OFFSET (2 * KOBOX_VM_CLIENT_PAGE_SIZE)
#define KOBOX_VM_CLIENT_STACK_SIZE (64 * 1024UL)
#define KOBOX_VM_CLIENT_CONTEXT_SIZE (KOBOX_VM_CLIENT_STACK_OFFSET + \
	KOBOX_VM_CLIENT_STACK_SIZE + KOBOX_VM_CLIENT_PAGE_SIZE)

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
	uint64_t client_entry;
	uint64_t control_address;
	uint64_t window_start;
	uint64_t window_size;
	/* Private machine bootstrap scratch, never a guest syscall argument. */
	char transfer_path[96];
	uint64_t address;
	uint64_t value;
	uint64_t result;
	uint64_t syscall_number;
	uint64_t syscall_arguments[6];
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

static inline bool kobox_vm_window_valid(uint64_t start, uint64_t size)
{
	const uint64_t limit = UINT64_C(1) << 47;

	/* The hosted core uses four-level x86 user page tables. */
	return start >= KOBOX_VM_CLIENT_PAGE_SIZE && start < limit && size &&
		!(start & (KOBOX_VM_CLIENT_PAGE_SIZE - 1)) &&
		!(size & (KOBOX_VM_CLIENT_PAGE_SIZE - 1)) && size <= limit - start;
}

void kobox_vm_syscall_entry(void);
uint64_t kobox_vm_clone_entry(uint64_t flags, uint64_t stack, uint64_t base,
			    uint64_t tid_address);
_Noreturn void kobox_vm_child_program(uint64_t base);
_Noreturn void kobox_vm_root_entry(uint64_t base, unsigned int cpu, uint64_t stack);
_Noreturn void kobox_vm_root_program(uint64_t base, unsigned int cpu);

#endif
