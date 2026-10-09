/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_ASM_CURRENT_H
#define KOBOX_TASK_ASM_CURRENT_H

#include <linux/cache.h>
#include <asm/percpu.h>

struct task_struct;
DECLARE_PER_CPU_CACHE_HOT(struct task_struct *, current_task);
DECLARE_PER_CPU_CACHE_HOT(struct task_struct * const, const_current_task);
extern struct task_struct *kobox_provider_current_task(void);

#ifdef KOBOX_PERCPU_SEGMENT
/* As upstream x86: one %gs-relative load of the logical CPU's current_task,
 * which kobox_task_switch() writes before handing the CPU to the next task,
 * and the idle/boot paths set before a CPU first runs. The task-switch and
 * bootstrap paths check it against the per-task host state under gates.
 * The read may be reused within a function, as the running task cannot
 * change under it; that saves the host-state call Linux makes dozens of
 * times per filesystem request. */
#define current this_cpu_read_stable(current_task)
#else
/* A task keeps its identity when its pthread resumes on another CPU. */
#define current kobox_provider_current_task()
#endif

#endif
