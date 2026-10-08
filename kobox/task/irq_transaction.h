/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_IRQ_TRANSACTION_H
#define KOBOX_TASK_IRQ_TRANSACTION_H

#include "host.h"

enum kobox_irq_operation {
	KOBOX_IRQ_QUERY,
	KOBOX_IRQ_SAVE,
	KOBOX_IRQ_DISABLE,
	KOBOX_IRQ_ENABLE,
};

/* All callbacks preserve the caller's bank, and current_cpu/thread_state are
 * integer-only leaves. A blanket native FP interval here would duplicate the
 * callee's protection even when only atomic IRQ state changes are needed.
 * Enable/restore still protect notification execution and task migration at
 * their actual entry. Never cache IRQ state or read CPU before masking notifications.
 * Operation is constant at each call site, allowing unused branches to fold.
 */
static inline unsigned long kobox_irq_transaction(
	const struct kobox_linux_task_host_operations *host,
	unsigned int (*current_cpu)(void), enum kobox_irq_operation operation)
{
	uint64_t mask;
	unsigned long flags;
	unsigned int cpu;

	/* A failed host transition leaves execution/IRQ ownership unknowable;
	 * continuing would violate exclusion, just as in the original boundary.
	 */
	if (host->notifications_save(&mask))
		__builtin_trap();
	cpu = current_cpu();
	flags = host->cpu_irq_disabled(cpu) != 0;
	if (operation == KOBOX_IRQ_SAVE || operation == KOBOX_IRQ_DISABLE) {
		if (!flags && host->cpu_irq_disable(cpu))
			__builtin_trap();
	} else if (operation == KOBOX_IRQ_ENABLE) {
		if (flags && host->cpu_irq_enable(cpu))
			__builtin_trap();
	}
	if (host->notifications_restore(mask))
		__builtin_trap();
	return flags;
}

#endif
