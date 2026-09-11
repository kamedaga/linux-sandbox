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
	struct list_head contexts;
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

/* Bind an upstream-created mm without replacing its VMAs, pages or layout.
 * The caller holds a live mm reference; success takes another mm_users
 * reference for the binding. Failure leaves host_space owned by the caller.
 * One mm corresponds to one host address space; threads reuse that binding.
 */
struct kobox_vm_space *kobox_vm_space_bind(struct mm_struct *mm, void *host_space,
	const struct kobox_linux_vm_host_operations *operations,
	unsigned long start, unsigned long size);

/* Attach another execution context of the SAME host MM, not copied mappings.
 * Takes ownership only on success. One binding mm reference spans all users.
 */
int kobox_vm_space_share(struct kobox_vm_space *space, void *host_context);

/* The caller owns worker/task references; it must join all users before
 * destroying a binding. Binding does not start or emulate a Linux task.
 */
struct kobox_vm_space *kobox_vm_space_create(void *host_space,
	const struct kobox_linux_vm_host_operations *operations,
	unsigned long start, unsigned long size);
int kobox_vm_space_destroy(struct kobox_vm_space *space);
/* Roll back an unstarted copy_process child; its task still owns its mm. */
int kobox_vm_space_cancel(struct kobox_vm_space *space, void *host_context,
			  struct task_struct *task);
/* The exiting owner has detached current->mm; observer mm refs may remain. */
int kobox_vm_space_exit(struct kobox_vm_space *space, void *host_context,
			struct task_struct *task);
/* Current has switched to a different upstream MM. Retire only its old
 * native context; surviving CLONE_VM peers retain the old binding.
 */
int kobox_vm_space_replaced(struct kobox_vm_space *space, void *host_context);
int kobox_vm_resolve_fault(struct kobox_vm_space *space, struct kobox_linux_vm_fault *fault);
void kobox_vm_flush_all(void);
void kobox_vm_interrupt(void);

#endif
