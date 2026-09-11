// SPDX-License-Identifier: GPL-2.0-only

#include "pci_host.h"
#include "../arch/x86_64/host_call.h"

#include <linux/ioport.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <asm/pci.h>

struct hosted_pci {
	struct pci_sysdata sysdata;
	struct kobox_linux_pci_host host;
	struct resource memory[KOBOX_PCI_MEMORY_WINDOWS];
	struct resource bus_numbers;
	raw_spinlock_t config_lock;
	struct kobox_mmio_region *mmio[KOBOX_PCI_MEMORY_WINDOWS];
	unsigned int mmio_count;
};

static bool config_valid(int offset, int size)
{
	return (size == 1 || size == 2 || size == 4) && offset >= 0 &&
	       offset <= PCI_CFG_SPACE_EXP_SIZE - size && !(offset & (size - 1));
}

static int config_read(struct pci_bus *bus, unsigned int devfn,
		       int offset, int size, u32 *value)
{
	struct hosted_pci *pci = container_of(bus->sysdata, struct hosted_pci, sysdata);
	unsigned long flags;
	int result;

	*value = ~0U;
	if (bus->number != pci->host.bus || devfn != pci->host.devfn)
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (!config_valid(offset, size))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	raw_spin_lock_irqsave(&pci->config_lock, flags);
	result = kobox_host_call(pci->host.config_read(pci->host.context, offset, size, value));
	raw_spin_unlock_irqrestore(&pci->config_lock, flags);
	if (result)
		*value = ~0U;
	return result ? PCIBIOS_SET_FAILED : PCIBIOS_SUCCESSFUL;
}

static int config_write(struct pci_bus *bus, unsigned int devfn,
			int offset, int size, u32 value)
{
	struct hosted_pci *pci = container_of(bus->sysdata, struct hosted_pci, sysdata);
	unsigned long flags;
	int result;

	if (bus->number != pci->host.bus || devfn != pci->host.devfn)
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (!config_valid(offset, size))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	raw_spin_lock_irqsave(&pci->config_lock, flags);
	result = kobox_host_call(pci->host.config_write(pci->host.context, offset, size, value));
	raw_spin_unlock_irqrestore(&pci->config_lock, flags);
	return result ? PCIBIOS_SET_FAILED : PCIBIOS_SUCCESSFUL;
}

static struct pci_ops hosted_ops = {.read = config_read, .write = config_write};

int kobox_linux_pci_scan(const struct kobox_linux_pci_host *host,
			struct pci_host_bridge **bridge_out)
{
	struct pci_host_bridge *bridge;
	struct hosted_pci *pci;
	u64 end;
	unsigned int index, previous;
	int result;

	if (!bridge_out)
		return -EINVAL;
	*bridge_out = NULL;
	if (!host || host->size != sizeof(*host) || !host->context ||
	    !host->config_read || !host->config_write || !host->memory_map ||
	    !host->memory_unmap || (!!host->memory_read != !!host->memory_write) ||
	    host->segment > U16_MAX ||
	    host->bus > U8_MAX || host->devfn > U8_MAX ||
	    host->window_count > KOBOX_PCI_MEMORY_WINDOWS)
		return -EINVAL;
	for (index = 0; index < host->window_count; index++) {
		const struct kobox_linux_pci_window *window = &host->windows[index];

		if (!window->length ||
		    check_add_overflow(window->start, window->length, &end) ||
		    end > U64_MAX - (PAGE_SIZE - 1))
			return -EINVAL;
		for (previous = 0; previous < index; previous++) {
			const struct kobox_linux_pci_window *other = &host->windows[previous];
			u64 other_end = other->start + other->length;

			if (window->start < other_end && other->start < end)
				return -EINVAL;
			/* Distinct sub-page BARs may share one host page. Partial
			 * aperture overlaps cannot be registered independently.
			 */
			if (round_down(window->start, PAGE_SIZE) < PAGE_ALIGN(other_end) &&
			    round_down(other->start, PAGE_SIZE) < PAGE_ALIGN(end) &&
			    (round_down(window->start, PAGE_SIZE) != round_down(other->start, PAGE_SIZE) ||
			     PAGE_ALIGN(end) != PAGE_ALIGN(other_end)))
				return -EINVAL;
		}
	}
	bridge = pci_alloc_host_bridge(sizeof(*pci));
	if (!bridge)
		return -ENOMEM;
	pci = pci_host_bridge_priv(bridge);
	pci->host = *host;
	pci->sysdata.domain = host->segment;
	pci->sysdata.node = NUMA_NO_NODE;
	raw_spin_lock_init(&pci->config_lock);
	for (index = 0; index < host->window_count; index++) {
		const struct kobox_linux_pci_window *window = &host->windows[index];

		pci->memory[index] = (struct resource) {
			.name = "host PCI memory", .start = window->start,
			.end = window->start + window->length - 1, .flags = IORESOURCE_MEM,
		};
		pci_add_resource(&bridge->windows, &pci->memory[index]);
	}
	pci->bus_numbers = (struct resource) {
		.name = "host PCI bus", .start = host->bus,
		.end = host->bus, .flags = IORESOURCE_BUS,
	};
	bridge->sysdata = &pci->sysdata;
	bridge->ops = &hosted_ops;
	bridge->busnr = host->bus;
	pci_add_resource(&bridge->windows, &pci->bus_numbers);
	if (list_count_nodes(&bridge->windows) != host->window_count + 1) {
		pci_free_resource_list(&bridge->windows);
		pci_free_host_bridge(bridge);
		return -ENOMEM;
	}
	pci_lock_rescan_remove();
	result = pci_scan_root_bus_bridge(bridge);
	if (!result)
		pci_bus_claim_resources(bridge->bus);
	pci_unlock_rescan_remove();
	if (result) {
		pci_free_host_bridge(bridge);
		return result;
	}
	for (index = 0; index < host->window_count; index++) {
		const struct kobox_linux_pci_window *window = &host->windows[index];
		u64 start = round_down(window->start, PAGE_SIZE);
		u64 length = PAGE_ALIGN(window->start + window->length) - start;

		for (previous = 0; previous < index; previous++) {
			const struct kobox_linux_pci_window *other = &host->windows[previous];

			if (round_down(other->start, PAGE_SIZE) == start &&
			    PAGE_ALIGN(other->start + other->length) == start + length)
				break;
		}
		if (previous != index)
			continue;
		result = kobox_mmio_register(&(struct kobox_mmio_host) {
			.context = host->context, .start = start,
			.length = length, .map = host->memory_map,
			.unmap = host->memory_unmap,
			.read = host->memory_read, .write = host->memory_write,
		}, &pci->mmio[pci->mmio_count]);
		if (result) {
			kobox_linux_pci_remove(bridge);
			return result;
		}
		pci->mmio_count++;
	}
	/* Discovery deliberately stops before pci_bus_add_devices(), which
	 * allows driver binding. DMA and IRQ prerequisites are not ready yet.
	 */
	*bridge_out = bridge;
	return 0;
}

int kobox_linux_pci_remove(struct pci_host_bridge *bridge)
{
	struct hosted_pci *pci;
	int result;

	if (!bridge)
		return -EINVAL;
	pci = pci_host_bridge_priv(bridge);
	/* Linux may defer a vmalloc TLB flush after iounmap. Publish that
	 * invalidation before testing whether any live alias still pins the host.
	 */
	vm_unmap_aliases();
	if (pci->mmio_count) {
		result = kobox_mmio_unregister(pci->mmio, pci->mmio_count);
		if (result)
			return result;
		pci->mmio_count = 0;
	}
	pci_lock_rescan_remove();
	pci_stop_root_bus(bridge->bus);
	pci_remove_root_bus(bridge->bus);
	pci_unlock_rescan_remove();
	pci_free_host_bridge(bridge);
	return 0;
}
