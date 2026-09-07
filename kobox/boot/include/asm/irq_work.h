/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_IRQ_WORK_H
#define KOBOX_BOOT_IRQ_WORK_H

/* Delivery uses the registered host CPU-notification transport, not LAPIC. */
static inline bool arch_irq_work_has_interrupt(void)
{
	return true;
}

#endif
