/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_DEVICE_PCI_LIFECYCLE_H
#define KOBOX_DEVICE_PCI_LIFECYCLE_H

#include "device_resource_interfaces.h"

#define KOBOX_DEVICE_PCI_FUNCTION_SLOT_ID 1u
#define KOBOX_DEVICE_PCI_DMA_DOMAIN_SLOT_ID 2u
#define KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID 3u

struct kobox_linux_irq_endpoint_resource {
	kobox_abi_u64 generation;
	kobox_abi_u64 object_id;
	struct kobox_module_resource_binding binding;
};

struct kobox_linux_device_pci_resources {
	struct kobox_pci_function_identity pci_identity;
	struct kobox_dma_domain_constraints dma_constraints;
	struct kobox_module_resource_binding pci;
	struct kobox_module_resource_binding dma;
	const struct kobox_linux_irq_endpoint_resource *irq_endpoints;
	size_t irq_endpoint_count;
};

/* Lifecycle calls are serialized by the closure owner. */
int kobox_linux_device_pci_init(const struct kobox_module_context *context);
int kobox_linux_device_pci_quiesce(const struct kobox_module_context *context);
int kobox_linux_device_pci_cleanup(const struct kobox_module_context *context);

/* Returns the immutable hardware-resource snapshot at the probe boundary. */
int kobox_linux_device_pci_probe_resources(
	const struct kobox_module_context *context,
	struct kobox_linux_device_pci_resources *resources_out);

#endif
