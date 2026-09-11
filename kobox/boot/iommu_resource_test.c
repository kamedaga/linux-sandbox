// SPDX-License-Identifier: GPL-2.0-only

#include "iommu_resource.h"

#include <kobox2/closure_layout.h>
#include <kobox2/iommu_domain_layout.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fixture {
	struct kobox_linux_resource_binding binding;
	struct kobox_iommu_identity identity;
	int lookup_error, operation_error;
	unsigned int queries, enables, maps, unmaps;
	uint64_t iova, offset, length;
	uint32_t protection;
};

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "IOMMU resource check failed at %d: %s\n", __LINE__, #expression); \
		return 1; \
	} \
} while (0)

static int lookup(void *context, const char *name, uint32_t slot, size_t index,
		  uint64_t rights, const uint8_t digest[32],
		  struct kobox_linux_resource_binding *out)
{
	static const uint8_t expected[32] = KB2_IOMMU_DOMAIN_SCHEMA_SHA256_BYTES;
	struct fixture *fixture = context;

	if (name || slot != 2 || index || rights != KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS ||
	    memcmp(digest, expected, 32))
		return -EACCES;
	*out = fixture->binding;
	return fixture->lookup_error;
}

static int identity(void *object, struct kobox_iommu_identity *out)
{
	struct fixture *fixture = object;

	fixture->queries++;
	*out = fixture->identity;
	return fixture->operation_error;
}

static int enable(void *object, uint32_t enabled)
{
	struct fixture *fixture = object;

	fixture->enables++;
	return enabled > 1 ? -EINVAL : fixture->operation_error;
}

/* Call-shape oracle only. dma_gate.c covers actual page/mapping lifetime. */
static int map(void *object, uint64_t iova, uint64_t offset,
	       uint64_t length, uint32_t protection)
{
	struct fixture *fixture = object;

	fixture->maps++;
	fixture->iova = iova;
	fixture->offset = offset;
	fixture->length = length;
	fixture->protection = protection;
	return fixture->operation_error;
}

static int unmap(void *object, uint64_t iova, uint64_t length)
{
	struct fixture *fixture = object;

	fixture->unmaps++;
	return iova != fixture->iova || length != fixture->length ? -ENOENT :
		fixture->operation_error;
}

int main(void)
{
	struct kobox_iommu_resource_operations operations = {
		.base = {.size = sizeof(operations), .identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER},
		.identity = identity, .set_enabled = enable, .map = map, .unmap = unmap,
	};
	struct fixture fixture = {
		.identity = {.generation = 9, .object_id = 24, .device_object_id = 23,
			.ram_length = 0x8000, .aperture_start = 0x40000000,
			.aperture_end = 0x40ffffff, .page_size = 4096, .coherent = 1},
	};
	struct kobox_linux_resource_port port = {
		.size = sizeof(port), .generation = 9, .context = &fixture, .lookup = lookup,
	};
	struct kobox_boot_iommu_resource iommu;
	unsigned int queries;

	fixture.binding = (struct kobox_linux_resource_binding) {
		.generation = 9, .object_id = 24, .type = KB2_CLOSURE_RESOURCE_DEVICE,
		.rights = KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS,
		.object = &fixture, .operations = &operations,
	};
	CHECK(!kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu));
	CHECK(iommu.host.context == &iommu && !fixture.maps && !fixture.enables);
	CHECK(!iommu.host.enable(&iommu, 1) && fixture.enables == 1);
	CHECK(!iommu.host.map(&iommu, 0x40001000, 0x2000, 4096, KOBOX_DMA_DEVICE_READ));
	CHECK(fixture.maps == 1 && fixture.iova == 0x40001000 && fixture.offset == 0x2000 &&
	      fixture.protection == KB2_IOMMU_DOMAIN_PROTECTION_DEVICE_READ);
	CHECK(!iommu.host.unmap(&iommu, 0x40001000, 4096) && fixture.unmaps == 1);
	CHECK(!iommu.host.map(&iommu, 0x40001000, 0x2000, 4096, KOBOX_DMA_DEVICE_WRITE));
	CHECK(fixture.protection == KB2_IOMMU_DOMAIN_PROTECTION_DEVICE_WRITE);
	CHECK(!iommu.host.unmap(&iommu, 0x40001000, 4096));
	fixture.operation_error = -ENOMEM;
	CHECK(iommu.host.map(&iommu, 0x40001000, 0x2000, 4096, KOBOX_DMA_DEVICE_READ) == -ENOMEM);
	fixture.operation_error = 1;
	CHECK(iommu.host.map(&iommu, 0x40001000, 0x2000, 4096, KOBOX_DMA_DEVICE_READ) == -EPROTO);
	CHECK(iommu.host.unmap(&iommu, 0x40001000, 4096) == -EPROTO);
	CHECK(iommu.host.enable(&iommu, 0) == -EPROTO);
	fixture.operation_error = 0;
	CHECK(iommu.host.enable(&iommu, 2) == -EINVAL);
	CHECK(iommu.host.map(&iommu, 0x3ffff000, 0, 4096, 1) == -EINVAL);
	CHECK(iommu.host.map(&iommu, 0x40fff000, 0, 8192, 1) == -EINVAL);
	CHECK(iommu.host.map(&iommu, 0x40000000, 0x7000, 8192, 1) == -EINVAL);
	CHECK(iommu.host.map(&iommu, 0x40000000, UINT64_MAX - 4095, 4096, 1) == -EINVAL);
	CHECK(iommu.host.map(&iommu, 0x40000001, 0, 4096, 1) == -EINVAL);
	CHECK(iommu.host.map(&iommu, 0x40000000, 1, 4096, 1) == -EINVAL);
	CHECK(iommu.host.map(&iommu, 0x40000000, 0, 0, 1) == -EINVAL);
	CHECK(iommu.host.map(&iommu, 0x40000000, 0, 4096, 4) == -EINVAL);
	CHECK(iommu.host.unmap(&iommu, UINT64_MAX - 4095, 8192) == -EINVAL);
	CHECK(fixture.maps == 4 && fixture.unmaps == 3 && fixture.enables == 2);
	queries = fixture.queries;
	fixture.lookup_error = -EACCES;
	CHECK(kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu) == -EACCES && !iommu.host.context);
	CHECK(fixture.queries == queries);
	fixture.lookup_error = 0;
	fixture.binding.generation--;
	CHECK(kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu) == -EPROTO);
	fixture.binding.generation++;
	fixture.binding.rights = 0;
	CHECK(kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu) == -EPROTO);
	fixture.binding.rights = KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS;
	fixture.identity.device_object_id++;
	CHECK(kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu) == -EPROTO);
	fixture.identity.device_object_id--;
	fixture.identity.ram_length *= 2;
	CHECK(kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu) == -EPROTO);
	fixture.identity.ram_length /= 2;
	fixture.identity.object_id++;
	CHECK(kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu) == -EPROTO);
	fixture.identity.object_id--;
	fixture.identity.coherent = 0;
	CHECK(kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu) == -EOPNOTSUPP);
	fixture.identity.coherent = 1;
	fixture.identity.page_size = 65536;
	CHECK(kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu) == -EOPNOTSUPP);
	fixture.identity.page_size = 4096;
	fixture.identity.aperture_end--;
	CHECK(kobox_boot_iommu_resource(&port, 2, 0, 23, 0x8000, &iommu) == -EPROTO);
	return 0;
}
