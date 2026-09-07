/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_VM_HOST_H
#define KOBOX_LINUX_VM_HOST_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

enum kobox_linux_vm_protection {
	KOBOX_VM_READ = 1U << 0,
	KOBOX_VM_WRITE = 1U << 1,
	KOBOX_VM_EXECUTE = 1U << 2,
};

struct kobox_linux_vm_event;

/* Process-local machine operations, not a wire ABI. The caller owns the host
 * address space until close has reaped it. map/reset are leaf operations:
 * synchronous and safe with Linux page-table locks/IRQs held. They must never
 * wait for guest task execution or return success before mappings change.
 */
struct kobox_linux_vm_host_operations {
	size_t size;
	int (*map)(void *space, uint64_t address, uint64_t physical,
		   size_t size, unsigned int protection);
	int (*reset)(void *space, uint64_t address, size_t size);
	int (*close)(void *space);
	int (*resume)(void *space, uint64_t sequence);
	/* Nonblocking dequeue: -EAGAIN is no event, never completion/death. */
	int (*event)(void *space, struct kobox_linux_vm_event *event);
};

/* Actual machine-fault context. Linux decides permissions, pages and signals. */
struct kobox_linux_vm_fault {
	uint64_t address;
	uint64_t ip;
	uint64_t sp;
	uint64_t flags;
	uint64_t error;
	uint32_t signal;
	int32_t signal_code;
};

enum kobox_linux_vm_event_kind {
	KOBOX_VM_EVENT_STOP,
	KOBOX_VM_EVENT_FAULT,
	KOBOX_VM_EVENT_EXIT,
};

struct kobox_linux_vm_event {
	struct kobox_linux_vm_fault fault;
	uint64_t sequence;
	uint64_t value;
	uint32_t kind;
	int32_t error;
	int32_t exit_status;
};

#endif
