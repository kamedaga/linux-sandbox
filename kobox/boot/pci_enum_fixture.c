// SPDX-License-Identifier: GPL-2.0-only

#include "pci_fixture.h"
#include "pci_config_fixture.h"
#include "../provider/device_resource_interfaces.h"

#include <kobox2/pci_function_layout.h>

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

struct enum_mapping {
	struct enum_mapping *next;
	void *address;
	uint64_t physical;
	size_t length;
	uint32_t protection;
};

struct enum_fixture {
	struct kobox_pci_config_fixture config;
	struct kobox_linux_pci_host host;
	struct enum_mapping *mappings;
	uint64_t generation, object_id;
};

static int identity(void *object, struct kobox_pci_function_identity *out)
{
	struct enum_fixture *fixture = object;

	if (!out)
		return -EINVAL;
	*out = (struct kobox_pci_function_identity) {
		.generation = fixture->generation, .object_id = fixture->object_id,
		.segment = fixture->host.segment, .vendor_id = 0x1af4, .device_id = 0x1050,
		.class_code = 0x030000, .revision = 1,
	};
	return 0;
}

static int read_config(void *object, uint32_t offset, uint32_t width, uint32_t *value)
{
	struct enum_fixture *fixture = object;

	return fixture->host.config_read(&fixture->config, offset, width, value);
}

static int write_config(void *object, uint32_t offset, uint32_t width, uint32_t value)
{
	struct enum_fixture *fixture = object;

	return fixture->host.config_write(&fixture->config, offset, width, value);
}

static const struct kobox_linux_pci_window *bar_window(struct enum_fixture *fixture,
						      uint32_t bar)
{
	if (bar == 0)
		return &fixture->host.windows[0];
	if (bar == 2 || bar == 3)
		return &fixture->host.windows[bar - 1];
	return NULL;
}

static int bar_info(void *object, uint32_t bar, uint64_t *address,
		    uint64_t *length, uint32_t *flags)
{
	struct enum_fixture *fixture = object;
	const struct kobox_linux_pci_window *window = bar_window(fixture, bar);

	if (bar >= KB2_PCI_FUNCTION_BAR_COUNT || !address || !length || !flags)
		return -EINVAL;
	if (!window)
		return -ENOENT;
	*address = window->start;
	*length = window->length;
	*flags = KB2_PCI_FUNCTION_BAR_FLAG_MEMORY;
	if (!bar)
		*flags |= KB2_PCI_FUNCTION_BAR_FLAG_PREFETCHABLE | KB2_PCI_FUNCTION_BAR_FLAG_64_BIT;
	return 0;
}

static int map_bar(void *object, uint32_t bar, uint64_t offset, size_t length,
		   uint32_t protection, uint32_t cache, void *requested, void **out)
{
	struct enum_fixture *fixture = object;
	const struct kobox_linux_pci_window *window = bar_window(fixture, bar);
	struct enum_mapping *mapping, *other;
	enum kobox_mmio_cache type;
	uint64_t start, size;
	unsigned int permissions = 0;
	int result;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!window)
		return -EINVAL;
	start = window->start & ~4095ULL;
	size = ((window->start + window->length + 4095) & ~4095ULL) - start;
	if (!requested || (uintptr_t)requested % 4096 ||
	    !length || length % 4096 || offset % 4096 || offset >= size || length > size - offset ||
	    (uintptr_t)requested > UINTPTR_MAX - length || !protection ||
	    protection & ~(KB2_PCI_FUNCTION_MAP_PROTECTION_READ | KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE))
		return -EINVAL;
	switch (cache) {
	case KB2_PCI_FUNCTION_CACHE_UC: type = KOBOX_MMIO_UC; break;
	case KB2_PCI_FUNCTION_CACHE_UC_MINUS: type = KOBOX_MMIO_UC_MINUS; break;
	case KB2_PCI_FUNCTION_CACHE_WC: type = KOBOX_MMIO_WC; break;
	default: return -EOPNOTSUPP;
	}
	for (other = fixture->mappings; other; other = other->next) {
		if ((uintptr_t)requested < (uintptr_t)other->address + other->length &&
		    (uintptr_t)other->address < (uintptr_t)requested + length)
			return -EBUSY;
	}
	/* The hosted MMIO port serializes this fixture's leaf callbacks with
	 * notifications masked. No host allocator lock survives a guest entry.
	 */
	mapping = calloc(1, sizeof(*mapping));
	if (!mapping)
		return -ENOMEM;
	if (protection & KB2_PCI_FUNCTION_MAP_PROTECTION_READ)
		permissions |= KOBOX_LINUX_MEMORY_READ;
	if (protection & KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE)
		permissions |= KOBOX_LINUX_MEMORY_WRITE;
	result = fixture->host.memory_map(&fixture->config, requested,
		start + offset, length, permissions, type);
	if (result) {
		free(mapping);
		return result;
	}
	*mapping = (struct enum_mapping) {
		.next = fixture->mappings, .address = requested, .physical = start + offset,
		.length = length, .protection = protection,
	};
	fixture->mappings = mapping;
	*out = requested;
	return 0;
}

static int unmap_bar(void *object, void *address, size_t length)
{
	struct enum_fixture *fixture = object;
	struct enum_mapping **entry = &fixture->mappings, *mapping;
	int result;

	for (; (mapping = *entry); entry = &mapping->next) {
		if (mapping->address != address || mapping->length != length)
			continue;
		result = fixture->host.memory_unmap(&fixture->config, address, length);
		if (result)
			return result;
		*entry = mapping->next;
		free(mapping);
		return 0;
	}
	return -EINVAL;
}

static void *access_address(struct enum_fixture *fixture, uint32_t bar,
			    uint64_t offset, uint32_t width, uint32_t protection)
{
	struct enum_mapping *mapping;
	const struct kobox_linux_pci_window *window = bar_window(fixture, bar);
	uint64_t physical;

	if (!window || (width != 1 && width != 2 && width != 4) || offset % width ||
	    offset >= window->length || width > window->length - offset)
		return NULL;
	physical = window->start + offset;
	for (mapping = fixture->mappings; mapping; mapping = mapping->next) {
		if ((mapping->protection & protection) != protection || physical < mapping->physical ||
		    physical - mapping->physical >= mapping->length ||
		    width > mapping->length - (physical - mapping->physical))
			continue;
		return (char *)mapping->address + (physical - mapping->physical);
	}
	return NULL;
}

static int read_bar(void *object, uint32_t bar, uint64_t offset, uint32_t width, uint32_t *out)
{
	void *address = access_address(object, bar, offset, width, KB2_PCI_FUNCTION_MAP_PROTECTION_READ);

	if (!out || !address)
		return -EACCES;
	if (width == 1)
		*out = *(volatile uint8_t *)address;
	else if (width == 2)
		*out = *(volatile uint16_t *)address;
	else
		*out = *(volatile uint32_t *)address;
	return 0;
}

static int write_bar(void *object, uint32_t bar, uint64_t offset, uint32_t width, uint32_t value)
{
	void *address = access_address(object, bar, offset, width, KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE);

	if (!address)
		return -EACCES;
	if (width == 1)
		*(volatile uint8_t *)address = value;
	else if (width == 2)
		*(volatile uint16_t *)address = value;
	else
		*(volatile uint32_t *)address = value;
	return 0;
}

static const struct kobox_pci_function_resource_operations operations = {
	.base = {.size = sizeof(operations), .identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER},
	.identity = identity, .config_read = read_config, .config_write = write_config,
	.bar_info = bar_info, .bar_map = map_bar, .bar_unmap = unmap_bar,
	.bar_read = read_bar, .bar_write = write_bar,
};

int kobox_pci_enum_fixture_import(void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles, size_t handle_count,
	void **object_out, const struct kobox_resource_interface_operations **operations_out)
{
	struct kobox_pci_fixture_owner *owner = context;
	struct enum_fixture *fixture;
	int descriptor, result;

	if (!object_out || !operations_out)
		return -EINVAL;
	*object_out = NULL;
	*operations_out = NULL;
	result = kobox_pci_fixture_descriptor(context, slot, object, handles, handle_count,
					      KOBOX_PCI_ENUM_FIXTURE_SIZE, &descriptor);
	if (result)
		return result;
	fixture = calloc(1, sizeof(*fixture));
	if (!fixture) {
		close(descriptor);
		return -ENOMEM;
	}
	kobox_pci_config_fixture_init(&fixture->config, &fixture->host);
	fixture->config.backing = descriptor;
	fixture->config.backing_offset = 4096;
	fixture->generation = owner->generation;
	fixture->object_id = owner->object_id;
	owner->imported++;
	*object_out = fixture;
	*operations_out = &operations.base;
	return 0;
}

void kobox_pci_enum_fixture_release(void *context, void *object)
{
	struct kobox_pci_fixture_owner *owner = context;
	struct enum_fixture *fixture = object;

	if (fixture->mappings || fixture->config.map_calls != fixture->config.unmap_calls ||
	    fixture->config.bad_sizing || fixture->config.sizing_pending)
		abort();
	close(fixture->config.backing);
	free(fixture);
	owner->released++;
}
