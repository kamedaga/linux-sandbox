/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_PCI_RESOURCE_H
#define KOBOX_BOOT_PCI_RESOURCE_H

#include "pci_host.h"
#include "resource_port.h"
#include "../provider/device_resource_interfaces.h"

struct kobox_boot_pci_bar {
	uint64_t address;
	uint64_t length;
	uint64_t mapping_start;
	uint64_t mapping_length;
	uint32_t flags;
};

/* Host-local adapter of a fixed-core resource grant. Keep this object at a
 * stable address, and its registry alive, until every Linux bridge user and
 * mapping has drained. Initialization performs no PCI enumeration or binding.
 */
struct kobox_boot_pci_resource {
	struct kobox_linux_pci_host host;
	struct kobox_linux_resource_binding binding;
	const struct kobox_pci_function_resource_operations *operations;
	struct kobox_boot_pci_bar bars[KOBOX_PCI_MEMORY_WINDOWS];
};

int kobox_boot_pci_resource(const struct kobox_linux_resource_port *port,
			    uint32_t slot, size_t index,
			    struct kobox_boot_pci_resource *out);

#endif
