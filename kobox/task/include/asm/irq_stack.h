/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_ASM_IRQ_STACK_H
#define KOBOX_TASK_ASM_IRQ_STACK_H

#include_next <asm/irq_stack.h>

/* Each hosted task already has a native pthread stack, including IRQ frames. */
#undef do_softirq_own_stack
#define do_softirq_own_stack() __do_softirq()

#endif
