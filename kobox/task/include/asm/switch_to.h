/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_ASM_SWITCH_TO_H
#define KOBOX_TASK_ASM_SWITCH_TO_H

#include_next <asm/switch_to.h>

extern void kobox_provider_finish_switch(void);
#define finish_arch_post_lock_switch() kobox_provider_finish_switch()

#endif
