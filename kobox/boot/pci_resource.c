// SPDX-License-Identifier: GPL-2.0-only

#include "pci_resource.h"

#include <kobox2/closure_layout.h>
#include <kobox2/pci_function_layout.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int status(int result)
{
	return result > 0 ? -EPROTO : result;
}

static int config_read(void *context, uint32_t offset, uint32_t width, uint32_t *value)
{
	struct kobox_boot_pci_resource *pci = context;

	return status(pci->operations->config_read(pci->binding.object, offset, width, value));
}

static int config_write(void *context, uint32_t offset, uint32_t width, uint32_t value)
{
	struct kobox_boot_pci_resource *pci = context;

	return status(pci->operations->config_write(pci->binding.object, offset, width, value));
}

static int memory_map(void *context, void *address, uint64_t physical,
		      size_t length, unsigned int protection, enum kobox_mmio_cache cache)
{
	static const uint32_t cache_types[] = {
		[KOBOX_MMIO_UC] = KB2_PCI_FUNCTION_CACHE_UC,
		[KOBOX_MMIO_UC_MINUS] = KB2_PCI_FUNCTION_CACHE_UC_MINUS,
		[KOBOX_MMIO_WC] = KB2_PCI_FUNCTION_CACHE_WC,
		[KOBOX_MMIO_WB] = KB2_PCI_FUNCTION_CACHE_WB,
		[KOBOX_MMIO_WT] = KB2_PCI_FUNCTION_CACHE_WT,
		[KOBOX_MMIO_WP] = KB2_PCI_FUNCTION_CACHE_WP,
	};
	struct kobox_boot_pci_resource *pci = context;
	uint32_t flags = 0;
	unsigned int index;

	if (!address || !length || (uintptr_t)address % KOBOX_LINUX_MEMORY_PAGE_SIZE ||
	    (uintptr_t)address > UINTPTR_MAX - length ||
	    physical % KOBOX_LINUX_MEMORY_PAGE_SIZE || length % KOBOX_LINUX_MEMORY_PAGE_SIZE ||
	    (unsigned int)cache >= sizeof(cache_types) / sizeof(cache_types[0]) ||
	    !protection || protection & ~(KOBOX_LINUX_MEMORY_READ | KOBOX_LINUX_MEMORY_WRITE))
		return -EINVAL;
	if (protection & KOBOX_LINUX_MEMORY_READ)
		flags |= KB2_PCI_FUNCTION_MAP_PROTECTION_READ;
	if (protection & KOBOX_LINUX_MEMORY_WRITE)
		flags |= KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE;
	for (index = 0; index < KOBOX_PCI_MEMORY_WINDOWS; index++) {
		const struct kobox_boot_pci_bar *bar = &pci->bars[index];
		void *mapped = NULL;
		int result;

		if (!bar->length || physical < bar->mapping_start ||
		    physical - bar->mapping_start >= bar->mapping_length ||
		    length > bar->mapping_length - (physical - bar->mapping_start))
			continue;
		result = status(pci->operations->bar_map(pci->binding.object, index,
			physical - bar->mapping_start, length, flags, cache_types[cache], address, &mapped));
		if (result)
			return result;
		if (mapped == address)
			return 0;
		if (mapped && pci->operations->bar_unmap(pci->binding.object, mapped, length))
			abort();
		return -EPROTO;
	}
	return -ERANGE;
}

static int memory_unmap(void *context, void *address, size_t length)
{
	struct kobox_boot_pci_resource *pci = context;

	return status(pci->operations->bar_unmap(pci->binding.object, address, length));
}

int kobox_boot_pci_resource(const struct kobox_linux_resource_port *port,
			    uint32_t slot, size_t index,
			    struct kobox_boot_pci_resource *out)
{
	static const uint8_t digest[32] = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;
	static const uint8_t identity[] = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER;
	struct kobox_boot_pci_resource pci = {0};
	struct kobox_pci_function_identity device;
	unsigned int bar, previous;
	int result;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!port || port->size != sizeof(*port) || !port->generation ||
	    !port->lookup || !port->context || !slot)
		return -EINVAL;
	result = status(port->lookup(port->context, NULL, slot, index,
		KB2_PCI_FUNCTION_REQUIRED_RIGHTS, digest, &pci.binding));
	if (result)
		return result;
	if (pci.binding.generation != port->generation || !pci.binding.object_id ||
	    pci.binding.type != KB2_CLOSURE_RESOURCE_DEVICE || !pci.binding.object ||
	    !pci.binding.operations ||
	    (pci.binding.rights & KB2_PCI_FUNCTION_REQUIRED_RIGHTS) != KB2_PCI_FUNCTION_REQUIRED_RIGHTS)
		return -EPROTO;
	pci.operations = pci.binding.operations;
	if (pci.operations->base.size != sizeof(*pci.operations) ||
	    memcmp(pci.operations->base.identity, identity, sizeof(identity)) ||
	    !pci.operations->identity || !pci.operations->config_read ||
	    !pci.operations->config_write || !pci.operations->bar_info ||
	    !pci.operations->bar_map || !pci.operations->bar_unmap)
		return -EPROTO;
	result = status(pci.operations->identity(pci.binding.object, &device));
	if (result)
		return result;
	if (device.generation != pci.binding.generation ||
	    device.object_id != pci.binding.object_id || device.segment > UINT16_MAX ||
	    device.device > 31 || device.function > 7)
		return -EPROTO;
	pci.host = (struct kobox_linux_pci_host) {
		.size = sizeof(pci.host), .segment = device.segment, .bus = device.bus,
		.devfn = (uint32_t)device.device * 8 + device.function,
		.config_read = config_read, .config_write = config_write,
		.memory_map = memory_map, .memory_unmap = memory_unmap,
	};
	for (bar = 0; bar < KOBOX_PCI_MEMORY_WINDOWS; bar++) {
		struct kobox_boot_pci_bar *range = &pci.bars[bar];
		const uint64_t page_mask = KOBOX_LINUX_MEMORY_PAGE_SIZE - 1;
		uint64_t end;

		result = status(pci.operations->bar_info(pci.binding.object, bar,
					&range->address, &range->length, &range->flags));
		if (result == -ENOENT) {
			memset(range, 0, sizeof(*range));
			continue;
		}
		if (result)
			return result;
		if (!(range->flags & KB2_PCI_FUNCTION_BAR_FLAG_MEMORY) ||
		    range->flags & ~(KB2_PCI_FUNCTION_BAR_FLAG_MEMORY |
			KB2_PCI_FUNCTION_BAR_FLAG_PREFETCHABLE | KB2_PCI_FUNCTION_BAR_FLAG_64_BIT))
			return -EOPNOTSUPP;
		if (!range->length ||
		    range->address > UINT64_MAX - range->length)
			return -EPROTO;
		end = range->address + range->length;
		if (end > UINT64_MAX - page_mask)
			return -EPROTO;
		range->mapping_start = range->address & ~page_mask;
		range->mapping_length = ((end + page_mask) & ~page_mask) - range->mapping_start;
		for (previous = 0; previous < bar; previous++) {
			const struct kobox_boot_pci_bar *other = &pci.bars[previous];

			if (other->length && range->address < other->address + other->length &&
			    other->address < range->address + range->length)
				return -EPROTO;
		}
		pci.host.windows[pci.host.window_count++] = (struct kobox_linux_pci_window) {
			.start = range->address, .length = range->length,
		};
	}
	*out = pci;
	out->host.context = out;
	return 0;
}
