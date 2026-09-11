/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_TASK_H
#define KOBOX_X86_TASK_H

#include <linux/sched.h>
#include <linux/sched/task.h>

void *kobox_arch_task_binding(const struct task_struct *task);
void kobox_arch_task_bind(struct task_struct *task, void *binding);
void kobox_arch_task_frame_init(struct task_struct *task,
				const struct kernel_clone_args *arguments);
#ifdef KOBOX_BOOT_RUNTIME
int kobox_arch_task_fp_clone(struct task_struct *task,
			    const struct kernel_clone_args *arguments);
void kobox_arch_task_fp_drop(struct task_struct *task);
void kobox_arch_task_fp_switch(struct task_struct *previous, unsigned int cpu);
void kobox_arch_task_stack_current(struct task_struct *task);
#endif

/* Architecture entry delegates scheduling/host-thread transfer to the port. */
struct task_struct *kobox_task_switch(struct task_struct *previous,
				     struct task_struct *next);

#endif
