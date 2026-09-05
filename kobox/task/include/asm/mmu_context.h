/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_ASM_MMU_CONTEXT_H
#define KOBOX_TASK_ASM_MMU_CONTEXT_H

#include_next <asm/mmu_context.h>

extern void kobox_provider_deactivate_mm(struct task_struct *task,
					 struct mm_struct *mm);

#undef deactivate_mm
#define deactivate_mm(task, mm) kobox_provider_deactivate_mm((task), (mm))

#endif
