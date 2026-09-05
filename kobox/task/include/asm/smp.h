/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_ASM_SMP_H
#define KOBOX_TASK_ASM_SMP_H

#include_next <asm/smp.h>

#ifndef __ASSEMBLY__
extern unsigned int kobox_provider_current_cpu_id(void);

/* CPU identity must not enter preemption while IRQ state is being read. */
#undef raw_smp_processor_id
#undef __smp_processor_id
#define raw_smp_processor_id() kobox_provider_current_cpu_id()
#define __smp_processor_id() kobox_provider_current_cpu_id()
#endif

#endif
