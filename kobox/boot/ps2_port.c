// SPDX-License-Identifier: GPL-2.0-only
#include "ps2_port.h"
#include "../arch/x86_64/host_call.h"

#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <asm/x86_init.h>

static struct kobox_linux_ps2_host ps2_host;
static bool irq_prepared;
static DEFINE_RAW_SPINLOCK(ps2_route_lock);

int kobox_linux_ps2_configure(const struct kobox_linux_ps2_host *host)
{
	if (!host || host->size != sizeof(*host) || !host->read ||
	    !host->write || !host->mask || !host->quiesce || ps2_host.read)
		return -EINVAL;
	ps2_host = *host;
	/* Only this isolated PS/2 core advertises an ISA controller. No PCI
	 * function or DMA authority is invented to satisfy a Linux driver. */
	x86_platform.legacy.i8042 = X86_LEGACY_I8042_EXPECTED_PRESENT;
	return 0;
}

unsigned char kobox_linux_ps2_inb(unsigned short port)
{
	uint8_t value;

	if (!ps2_host.read || (port != 0x60 && port != 0x64) ||
	    kobox_host_call(ps2_host.read(ps2_host.context, port, &value)))
		panic("kobox-ps2: unauthorized or failed port read\n");
	return value;
}
EXPORT_SYMBOL_GPL(kobox_linux_ps2_inb);

void kobox_linux_ps2_outb(unsigned char value, unsigned short port)
{
	if (!ps2_host.write || (port != 0x60 && port != 0x64) ||
	    kobox_host_call(ps2_host.write(ps2_host.context, port, value)))
		panic("kobox-ps2: unauthorized or failed controller write\n");
}
EXPORT_SYMBOL_GPL(kobox_linux_ps2_outb);

static void ps2_mask(struct irq_data *data)
{
	(void)data;
	if (kobox_host_call(ps2_host.mask(ps2_host.context, 1)))
		panic("kobox-ps2: IRQ mask failed\n");
}

static void ps2_unmask(struct irq_data *data)
{
	(void)data;
	if (kobox_host_call(ps2_host.mask(ps2_host.context, 0)))
		panic("kobox-ps2: IRQ unmask failed\n");
}

static void ps2_ack(struct irq_data *data)
{
	/* The native interrupt entry acknowledges the physical LAPIC. Linux
	 * consumes only its virtual edge here, after the IRQ FD was observed. */
	(void)data;
}

static struct irq_chip ps2_chip = {
	.name = "host-ps2", .irq_mask = ps2_mask,
	.irq_unmask = ps2_unmask, .irq_ack = ps2_ack,
};

int kobox_linux_ps2_irq_prepare(void)
{
	int result;

	if (!ps2_host.read || irq_prepared)
		return -EINVAL;
	/* i8042's ISA binding requests Linux IRQ 1. This number stays separate
	 * from the physical GSI and APIC vector owned by the native capability. */
	result = irq_alloc_descs(1, 1, 1, 0);
	if (result < 0)
		return result;
	irq_set_chip_and_handler(1, &ps2_chip, handle_edge_irq);
	irq_clear_status_flags(1, IRQ_NOREQUEST | IRQ_NOPROBE);
	irq_prepared = true;
	return 0;
}

int kobox_linux_ps2_irq_dispatch(void)
{
	unsigned long flags;
	int result;

	/* Exclude descriptor destruction even for a notification already queued
	 * before native quiesce. The handler only drains byte PIO and input state;
	 * it cannot sleep or reenter this route lock. */
	raw_spin_lock_irqsave(&ps2_route_lock, flags);
	result = irq_prepared ? generic_handle_irq(1) : -ENODEV;
	raw_spin_unlock_irqrestore(&ps2_route_lock, flags);
	return result;
}

void kobox_linux_ps2_irq_finish(void)
{
	unsigned long flags;
	if (!irq_prepared)
		return;
	if (kobox_host_call(ps2_host.mask(ps2_host.context, 1)) ||
	    kobox_host_call(ps2_host.quiesce(ps2_host.context)))
		panic("kobox-ps2: IRQ teardown failed\n");
	raw_spin_lock_irqsave(&ps2_route_lock, flags);
	irq_prepared = false;
	raw_spin_unlock_irqrestore(&ps2_route_lock, flags);
	synchronize_irq(1);
	irq_set_chip_and_handler(1, NULL, NULL);
	irq_free_descs(1, 1);
}
