/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_VM_PORT_H
#define KOBOX_LINUX_VM_PORT_H

#include "host.h"

#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

struct pt_regs;

/* Native x86 fault entry has no public header declaration. */
void do_user_addr_fault(struct pt_regs *regs, unsigned long error_code,
			unsigned long address);

struct kobox_vm_space {
	struct list_head entry;
	struct mm_struct *mm;
	const struct kobox_linux_vm_host_operations *operations;
	void *host_space;
	unsigned long start;
	unsigned long end;
	raw_spinlock_t translation_lock;
	u64 invalidations;
	u64 publications;
	wait_queue_head_t events;
};

/* The caller owns worker/task references; it must join all users before
 * destroying a binding. Binding does not start or emulate a Linux task.
 */
struct kobox_vm_space *kobox_vm_space_create(void *host_space,
	const struct kobox_linux_vm_host_operations *operations,
	unsigned long start, unsigned long size);
int kobox_vm_space_destroy(struct kobox_vm_space *space);
int kobox_vm_resolve_fault(struct kobox_vm_space *space, struct kobox_linux_vm_fault *fault);
void kobox_vm_flush_all(void);
void kobox_vm_interrupt(void);

#endif
