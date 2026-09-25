/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DEVICE_PORT_H
#define KOBOX_BOOT_DEVICE_PORT_H

#include "pci_host.h"
#include "dma_host.h"
#include "irq_host.h"

/* A single exclusively granted PCI function. Driver-specific readiness and
 * device-class services belong to the caller, not this resource attachment.
 */
struct kobox_linux_device_port_config {
	size_t size;
	const struct kobox_linux_pci_host *pci;
	const struct kobox_linux_dma_host *dma;
	const struct kobox_linux_irq_host *irq;
	/* Optional PCI class for readiness; zero leaves matching to the caller. */
	uint32_t expected_class;
};

#ifdef __KERNEL__
struct kobox_linux_device_port;
struct pci_dev;
struct kobox_linux_dma_port;

/* A failed prepare may still publish a partially owned port. The caller must
 * finish it before retiring the sandbox, and must never reuse its grant.
 */
int kobox_linux_device_port_prepare(
	const struct kobox_linux_device_port_config *config,
	struct kobox_linux_device_port **out);
struct pci_dev *kobox_linux_device_port_pci(struct kobox_linux_device_port *port);
struct kobox_linux_dma_port *kobox_linux_device_port_dma(
	struct kobox_linux_device_port *port);
int kobox_linux_device_port_finish(struct kobox_linux_device_port *port);
#endif

#endif
