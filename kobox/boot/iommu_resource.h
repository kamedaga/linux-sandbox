/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_IOMMU_RESOURCE_H
#define KOBOX_BOOT_IOMMU_RESOURCE_H

#include "dma_host.h"
#include "resource_port.h"
#include "../runtime/module_context.h"

struct kobox_iommu_identity {
	uint64_t generation;
	uint64_t object_id;
	uint64_t device_object_id;
	uint64_t ram_length;
	uint64_t aperture_start;
	uint64_t aperture_end;
	uint64_t page_size;
	uint32_t coherent;
};

/* Process-local calls for the serialized iommu-domain contract. No Linux
 * structures, page allocation, IOVA selection or DMA API live here.
 * The importer binds the authorized device and the actual boot RAM backing
 * before publishing this object. Callbacks obey dma_host.h's leaf rules.
 */
struct kobox_iommu_resource_operations {
	struct kobox_resource_interface_operations base;
	int (*identity)(void *object, struct kobox_iommu_identity *out);
	int (*set_enabled)(void *object, uint32_t enabled);
	int (*map)(void *object, uint64_t iova, uint64_t ram_offset,
		   uint64_t length, uint32_t protection);
	int (*unmap)(void *object, uint64_t iova, uint64_t length);
};

struct kobox_boot_iommu_resource {
	struct kobox_linux_dma_host host;
	struct kobox_linux_resource_binding binding;
	const struct kobox_iommu_resource_operations *operations;
	uint64_t ram_length;
};

/* Keep out at a stable address and the resource registry alive until Linux
 * has detached the device. device_object_id comes from the PCI grant, not
 * from the IOMMU object being checked. ram_length is the boot RAM size.
 */
int kobox_boot_iommu_resource(const struct kobox_linux_resource_port *port,
			      uint32_t slot, size_t index,
			      uint64_t device_object_id, uint64_t ram_length,
			      struct kobox_boot_iommu_resource *out);

#endif
