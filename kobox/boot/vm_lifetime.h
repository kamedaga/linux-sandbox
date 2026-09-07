/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_VM_LIFETIME_H
#define KOBOX_LINUX_VM_LIFETIME_H

struct file;
struct mm_struct;
struct task_struct;
struct kobox_linux_vm_report;
struct kobox_vm_lifetime;

/* Observe live objects before their owners stop tasks and destroy bindings. */
struct kobox_vm_lifetime *kobox_vm_lifetime_begin(struct file *file,
	struct mm_struct *mms[2], struct task_struct *tasks[2],
	struct kobox_linux_vm_report *report);

/* MM/task-only observer for owners that audit their buffers separately. */
struct kobox_vm_lifetime *kobox_vm_lifetime_tasks_begin(
	struct mm_struct *mms[2], struct task_struct *tasks[2],
	struct kobox_linux_vm_report *report);

/* Consumes the caller's file reference, plus all observer references. */
int kobox_vm_lifetime_finish(struct kobox_vm_lifetime *audit);

#endif
