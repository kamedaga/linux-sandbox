/* SPDX-License-Identifier: GPL-2.0-only */
#include "task.h"

#include <linux/sched/task_stack.h>
#ifdef KOBOX_BOOT_RUNTIME
#include <asm/fpu/sched.h>
#include <asm/shstk.h>
#include <asm/tlbflush.h>
#endif

#undef BUG
#define BUG() __builtin_trap()

void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *task)
{
	/* Kernel tasks borrow an active mm; external process translations stay
	 * owned by the host address-space binding, not the native pthread's CR3.
	 */
	if (task->mm || !mm)
		BUG();
#ifdef KOBOX_BOOT_RUNTIME
	this_cpu_write(cpu_tlbstate_shared.is_lazy, true);
#endif
}

void kobox_provider_deactivate_mm(struct task_struct *task, struct mm_struct *mm)
{
	/* Clearing native FS/GS here would destroy the host pthread's TLS. */
	if (task != current)
		BUG();
#ifdef KOBOX_BOOT_RUNTIME
	shstk_free(task);
	/* These are guest register caches, never the native pthread's TLS. */
	task->thread.fsindex = 0;
	task->thread.gsindex = 0;
	task->thread.fsbase = 0;
	task->thread.gsbase = 0;
#else
	if (mm || task->mm)
		BUG();
#endif
}

#ifndef KOBOX_BOOT_RUNTIME
void fpu_thread_struct_whitelist(unsigned long *offset, unsigned long *size)
{
	/* Hosted kernel tasks never save host FPU state into task_struct. */
	*offset = 0;
	*size = 0;
}
#endif

#ifndef KOBOX_BOOT_RUNTIME
void flush_thread(void)
{
	BUG();
}
#endif


void *kobox_arch_task_binding(const struct task_struct *task)
{
	return (void *)task->thread.sp;
}

void kobox_arch_task_bind(struct task_struct *task, void *binding)
{
	task->thread.sp = (unsigned long)binding;
}

void kobox_arch_task_frame_init(struct task_struct *task,
				const struct kernel_clone_args *arguments)
{
	/* Match the architecture's kernel-start frame: no inherited user IP
	 * or SP. A later native user entry must populate its actual registers.
	 */
	if (task->flags & PF_KTHREAD)
		memset(task_pt_regs(task), 0, sizeof(struct pt_regs));
	else {
		*task_pt_regs(task) = *task_pt_regs(current);
		task_pt_regs(task)->ax = 0;
		if (arguments->fn) {
			task_pt_regs(task)->ip = 0;
			task_pt_regs(task)->sp = 0;
		} else if (arguments->stack) {
			task_pt_regs(task)->sp = arguments->stack;
		}
	}
}

#ifdef KOBOX_BOOT_RUNTIME
int kobox_arch_task_fp_clone(struct task_struct *task,
			    const struct kernel_clone_args *arguments)
{
	return fpu_clone(task, arguments->flags, !!arguments->fn, 0);
}

void kobox_arch_task_fp_drop(struct task_struct *task)
{
	fpu__drop(task);
}

void kobox_arch_task_fp_switch(struct task_struct *previous, unsigned int cpu)
{
	switch_fpu(previous, cpu);
	raw_cpu_write(fpu_fpregs_owner_ctx, NULL);
}

void kobox_arch_task_stack_current(struct task_struct *task)
{
	raw_cpu_write(cpu_current_top_of_stack, task_top_of_stack(task));
}
#endif

struct task_struct *__switch_to_asm(struct task_struct *previous,
				    struct task_struct *next)
{
	return kobox_task_switch(previous, next);
}
