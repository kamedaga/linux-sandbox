// SPDX-License-Identifier: GPL-2.0-only

#include "pci_resource.h"

#include <kobox2/closure_layout.h>
#include <kobox2/pci_function_layout.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fixture {
	struct kobox_linux_resource_binding binding;
	struct kobox_pci_function_identity identity;
	struct kobox_boot_pci_bar bars[6];
	int lookup_error, map_error;
	unsigned int lookups, queries, reads, writes, maps, unmaps;
	uint32_t last_bar, last_cache, last_protection;
	uint64_t last_offset;
	void *last_address;
	int wrong_address;
};

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "PCI resource check failed at %d: %s\n", __LINE__, #expression); \
		return 1; \
	} \
} while (0)

static int lookup(void *context, const char *name, uint32_t slot, size_t index,
		  uint64_t rights, const uint8_t digest[32],
		  struct kobox_linux_resource_binding *out)
{
	static const uint8_t expected[32] = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;
	struct fixture *fixture = context;

	fixture->lookups++;
	if (name || slot != 7 || index != 2 || rights != KB2_PCI_FUNCTION_REQUIRED_RIGHTS ||
	    memcmp(digest, expected, 32))
		return -EACCES;
	*out = fixture->binding;
	return fixture->lookup_error;
}

static int identity(void *object, struct kobox_pci_function_identity *out)
{
	struct fixture *fixture = object;

	*out = fixture->identity;
	return 0;
}

static int bar_info(void *object, uint32_t bar, uint64_t *address,
		    uint64_t *length, uint32_t *flags)
{
	struct fixture *fixture = object;

	fixture->queries++;
	if (bar >= 6)
		return -EINVAL;
	if (!fixture->bars[bar].length)
		return -ENOENT;
	*address = fixture->bars[bar].address;
	*length = fixture->bars[bar].length;
	*flags = fixture->bars[bar].flags;
	return 0;
}

static int read_config(void *object, uint32_t offset, uint32_t width, uint32_t *value)
{
	struct fixture *fixture = object;

	fixture->reads++;
	*value = offset ^ width;
	return 0;
}

static int write_config(void *object, uint32_t offset, uint32_t width, uint32_t value)
{
	struct fixture *fixture = object;

	fixture->writes++;
	return value == (offset ^ width) ? 0 : -EIO;
}

/* Call-shape oracle only; actual VM access is covered by pci_gate.c. */
static int map_bar(void *object, uint32_t bar, uint64_t offset, size_t length,
		   uint32_t protection, uint32_t cache, void *requested, void **out)
{
	struct fixture *fixture = object;

	if (length != 4096)
		return -EINVAL;
	fixture->maps++;
	fixture->last_bar = bar;
	fixture->last_offset = offset;
	fixture->last_cache = cache;
	fixture->last_protection = protection;
	fixture->last_address = requested;
	*out = fixture->wrong_address ? (char *)requested + 4096 : requested;
	return fixture->map_error;
}

static int unmap_bar(void *object, void *address, size_t length)
{
	struct fixture *fixture = object;

	if (!address || length != 4096)
		return -EINVAL;
	fixture->unmaps++;
	return 0;
}

int main(void)
{
	struct kobox_pci_function_resource_operations operations = {
		.base = {.size = sizeof(operations), .identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER},
		.identity = identity, .config_read = read_config, .config_write = write_config,
		.bar_info = bar_info, .bar_map = map_bar, .bar_unmap = unmap_bar,
	};
	struct fixture fixture = {
		.identity = {.generation = 9, .object_id = 23, .segment = 1, .bus = 2,
			     .device = 3, .function = 4},
		.bars = {
			[0] = {.address = 0x120000000ULL, .length = 0x4000,
			       .flags = KB2_PCI_FUNCTION_BAR_FLAG_MEMORY | KB2_PCI_FUNCTION_BAR_FLAG_64_BIT},
			[2] = {.address = 0x130000100ULL, .length = 0x100,
			       .flags = KB2_PCI_FUNCTION_BAR_FLAG_MEMORY},
		},
	};
	struct kobox_linux_resource_port port = {
		.size = sizeof(port), .generation = 9, .context = &fixture, .lookup = lookup,
	};
	struct kobox_boot_pci_resource pci;
	_Alignas(4096) unsigned char destination[8192];
	uint32_t value;
	unsigned int queries;

	fixture.binding = (struct kobox_linux_resource_binding) {
		.generation = 9, .object_id = 23, .type = KB2_CLOSURE_RESOURCE_DEVICE,
		.rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
		.object = &fixture, .operations = &operations,
	};
	CHECK(!kobox_boot_pci_resource(&port, 7, 2, &pci));
	CHECK(pci.host.context == &pci && pci.host.segment == 1 &&
	      pci.host.bus == 2 && pci.host.devfn == 28 && pci.host.window_count == 2);
	CHECK(fixture.queries == 6 && !fixture.reads && !fixture.writes && !fixture.maps);
	CHECK(pci.host.windows[0].start == 0x120000000ULL &&
	      pci.host.windows[1].start == 0x130000100ULL && pci.host.windows[1].length == 0x100);
	CHECK(pci.bars[2].mapping_start == 0x130000000ULL && pci.bars[2].mapping_length == 4096);
	CHECK(!pci.host.config_read(&pci, 0x100, 4, &value) && value == 0x104);
	CHECK(!pci.host.config_write(&pci, 0x100, 4, value));
	CHECK(!pci.host.memory_map(&pci, destination, 0x120001000ULL, 4096,
		KOBOX_LINUX_MEMORY_READ | KOBOX_LINUX_MEMORY_WRITE, KOBOX_MMIO_WC));
	CHECK(fixture.maps == 1 && fixture.last_bar == 0 && fixture.last_offset == 4096 &&
	      fixture.last_cache == KB2_PCI_FUNCTION_CACHE_WC && fixture.last_address == destination);
	CHECK(!pci.host.memory_unmap(&pci, destination, 4096) && fixture.unmaps == 1);
	CHECK(!pci.host.memory_map(&pci, destination, 0x130000000ULL, 4096,
		KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC));
	CHECK(fixture.last_bar == 2 && !fixture.last_offset &&
	      fixture.last_protection == KB2_PCI_FUNCTION_MAP_PROTECTION_READ &&
	      fixture.last_cache == KB2_PCI_FUNCTION_CACHE_UC);
	CHECK(!pci.host.memory_unmap(&pci, destination, 4096));
	fixture.map_error = -EACCES;
	CHECK(pci.host.memory_map(&pci, destination, 0x130000000ULL, 4096,
		KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) == -EACCES);
	fixture.map_error = 0;
	CHECK(pci.host.memory_map(&pci, destination, 0x130001000ULL, 4096,
		KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) == -ERANGE);
	CHECK(pci.host.memory_map(&pci, destination, 0x120004000ULL, 4096,
		KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) == -ERANGE);
	CHECK(pci.host.memory_map(&pci, destination, 0x120003000ULL, 8192,
		KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) == -ERANGE);
	CHECK(pci.host.memory_map(&pci, destination, 0x120000000ULL, 4096,
		KOBOX_LINUX_MEMORY_EXECUTE, KOBOX_MMIO_UC) == -EINVAL);
	CHECK(pci.host.memory_map(&pci, (void *)(UINTPTR_MAX - 4095), 0x120000000ULL,
		4096, KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) == -EINVAL);
	fixture.map_error = -ENOMEM;
	CHECK(pci.host.memory_map(&pci, destination, 0x120000000ULL, 4096,
		KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) == -ENOMEM);
	fixture.map_error = 1;
	CHECK(pci.host.memory_map(&pci, destination, 0x120000000ULL, 4096,
		KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) == -EPROTO);
	fixture.map_error = 0;
	fixture.wrong_address = 1;
	CHECK(pci.host.memory_map(&pci, destination, 0x120000000ULL, 4096,
		KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) == -EPROTO && fixture.unmaps == 3);
	queries = fixture.queries;
	fixture.lookup_error = -EACCES;
	CHECK(kobox_boot_pci_resource(&port, 7, 2, &pci) == -EACCES && !pci.host.context);
	CHECK(fixture.queries == queries);
	fixture.lookup_error = 0;
	fixture.binding.generation--;
	CHECK(kobox_boot_pci_resource(&port, 7, 2, &pci) == -EPROTO);
	fixture.binding.generation++;
	fixture.binding.rights = 0;
	CHECK(kobox_boot_pci_resource(&port, 7, 2, &pci) == -EPROTO);
	fixture.binding.rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS;
	fixture.identity.object_id++;
	CHECK(kobox_boot_pci_resource(&port, 7, 2, &pci) == -EPROTO);
	fixture.identity.object_id--;
	fixture.bars[2].address = fixture.bars[0].address;
	CHECK(kobox_boot_pci_resource(&port, 7, 2, &pci) == -EPROTO);
	fixture.bars[2].address = UINT64_MAX - 4095;
	CHECK(kobox_boot_pci_resource(&port, 7, 2, &pci) == -EPROTO);
	fixture.bars[2].address = 0x130000000ULL;
	fixture.bars[2].flags = KB2_PCI_FUNCTION_BAR_FLAG_IO;
	CHECK(kobox_boot_pci_resource(&port, 7, 2, &pci) == -EOPNOTSUPP);
	return 0;
}
