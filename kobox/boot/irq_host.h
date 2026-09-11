/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_IRQ_HOST_H
#define KOBOX_BOOT_IRQ_HOST_H

#include "../task/host.h"

enum kobox_linux_irq_mode {
	KOBOX_IRQ_INTX,
	KOBOX_IRQ_MSI,
	KOBOX_IRQ_MSIX,
};

struct kobox_linux_irq_route {
	uint64_t hwirq;
	uint64_t cookie;
	uint64_t address;
	uint32_t data;
};

struct kobox_linux_irq_event {
	uint64_t hwirq;
	uint64_t cookie;
};

/* One authorized PCI function's interrupt-controller interface, not a wire
 * structure. Linux owns IRQ descriptors, MSI capabilities/Table programming,
 * flow handlers and handler synchronization. The host owns hardware routes.
 *
 * allocate is all-or-nothing; each route has a nonzero, non-recycled cookie.
 * Multi-MSI routes have one address and aligned consecutive data values.
 * All routes start masked. release blocks its source and invalidates/drains the
 * route before returning; an error is fatal, not permission to reuse it.
 * Affinity changes preserve the MSI message and redirect pending notifications
 * as well as future ones. Already executing handlers are synchronized by Linux.
 * next returns one event, or -EAGAIN when that CPU's queue is empty. A doorbell
 * invokes KOBOX_LINUX_TASK_DEVICE_IRQ on the selected execution domain; host
 * threads must never call Linux IRQ handlers directly. All leaf callbacks run
 * with guest IRQs disabled and must not reenter Linux.
 */
struct kobox_linux_irq_host {
	size_t size;
	void *context;
	int (*allocate)(void *context, enum kobox_linux_irq_mode mode,
			uint32_t index, uint32_t count, uint32_t cpu,
			struct kobox_linux_irq_route *routes);
	int (*release)(void *context, uint64_t hwirq, uint64_t cookie);
	int (*mask)(void *context, uint64_t hwirq, uint64_t cookie,
		    unsigned int masked);
	int (*ack)(void *context, uint64_t hwirq, uint64_t cookie);
	/* With the source stopped and the route masked, discard accepted pending
	 * notifications before a new handler may use this still-allocated route.
	 * Must reject an unmasked route or an event still in service.
	 */
	int (*quiesce)(void *context, uint64_t hwirq, uint64_t cookie);
	/* In flight to a CPU, including the interval before the Linux handler. */
	int (*active)(void *context, uint64_t hwirq, uint64_t cookie,
		      unsigned int *active);
	int (*affinity)(void *context, uint64_t hwirq, uint64_t cookie,
			uint32_t cpu);
	int (*retrigger)(void *context, uint64_t hwirq, uint64_t cookie);
	int (*next)(void *context, uint32_t cpu, struct kobox_linux_irq_event *event);
};

#ifdef __KERNEL__
struct pci_dev;
struct kobox_linux_irq_port;

int kobox_linux_irq_attach(struct pci_dev *device,
			   const struct kobox_linux_irq_host *host,
			   struct kobox_linux_irq_port **out);
/* Caller has stopped new requests and freed handlers and MSI vectors. */
int kobox_linux_irq_detach(struct kobox_linux_irq_port *port);
void kobox_linux_irq_dispatch(unsigned int cpu);
void kobox_linux_task_device_irq_raise(unsigned int cpu);
#endif

#endif
