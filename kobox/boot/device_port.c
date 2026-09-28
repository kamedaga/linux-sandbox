// SPDX-License-Identifier: GPL-2.0-only

#include "device_port.h"
#include "diagnostic.h"

#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/slab.h>

struct kobox_linux_device_port {
	struct pci_host_bridge *bridge;
	struct pci_dev *pci;
	struct kobox_linux_dma_port *dma;
	struct kobox_linux_irq_port *irq;
};

int kobox_linux_device_port_prepare(
	const struct kobox_linux_device_port_config *config,
	struct kobox_linux_device_port **out)
{
	struct kobox_linux_device_port *port;
	int result;

	if (!out || *out || !config || config->size != sizeof(*config) ||
	    !config->pci || !config->dma || !config->irq)
		return -EINVAL;
	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	*out = port;
	result = kobox_linux_pci_scan(config->pci, &port->bridge);
	if (result)
		return result;
	pci_assign_unassigned_bus_resources(port->bridge->bus);
	port->pci = pci_get_slot(port->bridge->bus, config->pci->devfn);
	if (!port->pci || port->pci->driver ||
	    (config->expected_class &&
	     port->pci->class != config->expected_class))
		return -ENODEV;
	result = kobox_linux_dma_attach(&port->pci->dev, config->dma,
				       &port->dma);
	if (result)
		return result;
	result = kobox_linux_irq_attach(port->pci, config->irq, &port->irq);
	if (result)
		return result;
	pci_bus_add_devices(port->bridge->bus);
	return 0;
}

struct pci_dev *kobox_linux_device_port_pci(struct kobox_linux_device_port *port)
{
	return port ? port->pci : NULL;
}

struct kobox_linux_dma_port *kobox_linux_device_port_dma(
	struct kobox_linux_device_port *port)
{
	return port ? port->dma : NULL;
}

int kobox_linux_device_port_finish(struct kobox_linux_device_port *port)
{
	int result;

	if (!port)
		return -EINVAL;
	if (port->pci && port->pci->driver)
		return -EBUSY;
	if (port->irq) {
		result = kobox_linux_irq_detach(port->irq);
		if (result) {
			kobox_linux_boot_diagnostic(
				"kobox-device: IRQ detach status=%d\n", result);
			return result;
		}
		port->irq = NULL;
	}
	if (port->dma) {
		result = kobox_linux_dma_detach(port->dma);
		if (result) {
			kobox_linux_boot_diagnostic(
				"kobox-device: DMA detach status=%d\n", result);
			return result;
		}
		port->dma = NULL;
	}
	if (port->pci) {
		pci_dev_put(port->pci);
		port->pci = NULL;
	}
	if (port->bridge) {
		/* This is the final owner path after module unload, file closure,
		 * IRQ detach and DMA revoke. Any remaining ioremap is an orphan;
		 * let Linux tear down its VMA before retiring the host BAR grant. */
		result = kobox_linux_pci_retire_orphan_ioremaps(port->bridge);
		if (result) {
			kobox_linux_boot_diagnostic(
				"kobox-device: orphan MMIO retire status=%d\n",
				result);
			return result;
		}
		result = kobox_linux_pci_remove(port->bridge);
		if (result) {
			kobox_linux_boot_diagnostic(
				"kobox-device: PCI bridge remove status=%d\n", result);
			return result;
		}
	}
	kfree(port);
	return 0;
}
