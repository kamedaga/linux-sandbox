/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_USER_H
#define KOBOX_TASK_USER_H

#include "../arch/x86_64/user.h"

struct kobox_vm_space;
struct kobox_linux_vm_event;
struct kernel_clone_args;
struct task_struct;
struct kobox_user_context;

/* Called on the client's real Linux task with IRQs enabled. Linux owns all
 * syscall selection, credentials, files and entry/exit work. This transfers
 * GPR/TLS state; FP state and user process creation have separate boundaries.
 * A successful exit syscall does not return.
 */
int kobox_user_syscall(struct kobox_vm_space *space, void *host_context,
		       const struct kobox_linux_vm_event *event,
		       struct kobox_x86_user_regs *output);
struct kobox_user_context *kobox_user_clone(struct task_struct *task,
	const struct kernel_clone_args *arguments);
void __noreturn kobox_user_enter(struct kobox_user_context *context);
void kobox_user_release(struct kobox_user_context *context, struct task_struct *task);

/* Hand an exclusively owned, bound native context to current at its first
 * captured syscall. Current must already be an upstream-created user task
 * using space->mm. On success this never returns: exit_thread closes the
 * native context and releases the binding. Failure retains caller ownership.
 */
int kobox_user_adopt(struct kobox_vm_space *space, void *host_context,
		     const struct kobox_linux_vm_event *event);
/* Enter the current task's upstream-created initial register/FP state.
 * The bound native context must be fresh. Ownership follows adopt().
 */
int kobox_user_start(struct kobox_vm_space *space, void *host_context);
/* Internal task-port ownership hook; does not create/select a Linux task. */
int kobox_task_user_attach(struct kobox_user_context *context);

#endif
