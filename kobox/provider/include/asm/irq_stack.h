/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_ASM_IRQ_STACK_H
#define KOBOX_PROVIDER_ASM_IRQ_STACK_H

#include <linux/ptrace.h>

/* A provider native thread has no hardware IRQ stack to switch to. */
#define run_sysvec_on_irqstack_cond(func, regs) \
	do { \
		irq_enter_rcu(); \
		func(regs); \
		irq_exit_rcu(); \
	} while (0)

#define run_irq_on_irqstack_cond(func, regs, vector) \
	do { \
		irq_enter_rcu(); \
		func(regs, vector); \
		irq_exit_rcu(); \
	} while (0)

#ifdef CONFIG_SOFTIRQ_ON_OWN_STACK
#define do_softirq_own_stack() __do_softirq()
#endif

#endif /* KOBOX_PROVIDER_ASM_IRQ_STACK_H */
