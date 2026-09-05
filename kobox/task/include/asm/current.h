/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_ASM_CURRENT_H
#define KOBOX_TASK_ASM_CURRENT_H

#include <linux/cache.h>
#include <asm/percpu.h>

struct task_struct;
DECLARE_PER_CPU_CACHE_HOT(struct task_struct *, current_task);
DECLARE_PER_CPU_CACHE_HOT(struct task_struct * const, const_current_task);
extern struct task_struct *kobox_provider_current_task(void);

/* A task keeps its identity when its pthread resumes on another CPU. */
#define current kobox_provider_current_task()

#endif
