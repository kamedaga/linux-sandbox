// SPDX-License-Identifier: GPL-2.0-only

#include "iommu_resource.h"

#include <kobox2/closure_layout.h>
#include <kobox2/iommu_domain_layout.h>

#include <errno.h>
#include <string.h>

static int status(int result)
{
	return result > 0 ? -EPROTO : result;
}

static int enable(void *context, unsigned int enabled)
{
	struct kobox_boot_iommu_resource *iommu = context;

	if (enabled > 1)
		return -EINVAL;
	return status(iommu->operations->set_enabled(iommu->binding.object, enabled));
}

static int valid_iova(const struct kobox_boot_iommu_resource *iommu,
		      uint64_t iova, size_t length)
{
	return length && !(iova % KOBOX_LINUX_MEMORY_PAGE_SIZE) &&
		!(length % KOBOX_LINUX_MEMORY_PAGE_SIZE) &&
		iova >= iommu->host.aperture_start && iova <= iommu->host.aperture_end &&
		length - 1 <= iommu->host.aperture_end - iova;
}

static int map(void *context, uint64_t iova, uint64_t offset,
	       size_t length, unsigned int protection)
{
	struct kobox_boot_iommu_resource *iommu = context;
	uint32_t flags = 0;

	if (!valid_iova(iommu, iova, length) ||
	    offset % KOBOX_LINUX_MEMORY_PAGE_SIZE || offset >= iommu->ram_length ||
	    length > iommu->ram_length - offset || !protection ||
	    protection & ~(KOBOX_DMA_DEVICE_READ | KOBOX_DMA_DEVICE_WRITE))
		return -EINVAL;
	if (protection & KOBOX_DMA_DEVICE_READ)
		flags |= KB2_IOMMU_DOMAIN_PROTECTION_DEVICE_READ;
	if (protection & KOBOX_DMA_DEVICE_WRITE)
		flags |= KB2_IOMMU_DOMAIN_PROTECTION_DEVICE_WRITE;
	return status(iommu->operations->map(iommu->binding.object, iova, offset, length, flags));
}

static int unmap(void *context, uint64_t iova, size_t length)
{
	struct kobox_boot_iommu_resource *iommu = context;

	if (!valid_iova(iommu, iova, length))
		return -EINVAL;
	return status(iommu->operations->unmap(iommu->binding.object, iova, length));
}

int kobox_boot_iommu_resource(const struct kobox_linux_resource_port *port,
			      uint32_t slot, size_t index,
			      uint64_t device_object_id, uint64_t ram_length,
			      struct kobox_boot_iommu_resource *out)
{
	static const uint8_t digest[32] = KB2_IOMMU_DOMAIN_SCHEMA_SHA256_BYTES;
	static const uint8_t identity[] = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER;
	struct kobox_boot_iommu_resource iommu = {0};
	struct kobox_iommu_identity domain;
	int result;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!port || port->size != sizeof(*port) || !port->generation ||
	    !port->lookup || !port->context || !slot || !device_object_id ||
	    !ram_length || ram_length % KOBOX_LINUX_MEMORY_PAGE_SIZE)
		return -EINVAL;
	result = status(port->lookup(port->context, NULL, slot, index,
		KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS, digest, &iommu.binding));
	if (result)
		return result;
	if (iommu.binding.generation != port->generation || !iommu.binding.object_id ||
	    iommu.binding.type != KB2_CLOSURE_RESOURCE_DEVICE || !iommu.binding.object ||
	    !iommu.binding.operations ||
	    (iommu.binding.rights & KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS) != KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS)
		return -EPROTO;
	iommu.operations = iommu.binding.operations;
	if (iommu.operations->base.size != sizeof(*iommu.operations) ||
	    memcmp(iommu.operations->base.identity, identity, sizeof(identity)) ||
	    !iommu.operations->identity || !iommu.operations->set_enabled ||
	    !iommu.operations->map || !iommu.operations->unmap)
		return -EPROTO;
	result = status(iommu.operations->identity(iommu.binding.object, &domain));
	if (result)
		return result;
	if (domain.generation != iommu.binding.generation ||
	    domain.object_id != iommu.binding.object_id ||
	    domain.device_object_id != device_object_id || domain.ram_length != ram_length ||
	    domain.aperture_start > domain.aperture_end ||
	    domain.aperture_start % KOBOX_LINUX_MEMORY_PAGE_SIZE ||
	    domain.aperture_end % KOBOX_LINUX_MEMORY_PAGE_SIZE != KOBOX_LINUX_MEMORY_PAGE_SIZE - 1)
		return -EPROTO;
	if (domain.page_size != KOBOX_LINUX_MEMORY_PAGE_SIZE || domain.coherent != 1)
		return -EOPNOTSUPP;
	iommu.ram_length = ram_length;
	iommu.host = (struct kobox_linux_dma_host) {
		.size = sizeof(iommu.host), .aperture_start = domain.aperture_start,
		.aperture_end = domain.aperture_end, .coherent = domain.coherent,
		.enable = enable, .map = map, .unmap = unmap,
	};
	*out = iommu;
	out->host.context = out;
	return 0;
}
