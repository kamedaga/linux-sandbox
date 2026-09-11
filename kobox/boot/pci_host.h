/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_PCI_HOST_H
#define KOBOX_BOOT_PCI_HOST_H

#include "../task/host.h"
#include "../memory/mmio.h"

#define KOBOX_PCI_MEMORY_WINDOWS 6U

struct kobox_linux_pci_window {
	uint64_t start;
	uint64_t length;
};

/* Local machine port, not a wire description or a PCI driver. The host
 * supplies one authorized function and its address aperture. Linux owns
 * config interpretation, capability walking and BAR resource discovery.
 * Config callbacks run under the config spinlock; mapping callbacks run
 * under the MMIO lock. All are leaf operations with guest IRQs disabled.
 * Keep context alive until the root bus and all borrowed references drain.
 */
struct kobox_linux_pci_host {
	size_t size;
	void *context;
	uint32_t segment;
	uint32_t bus;
	uint32_t devfn;
	uint32_t window_count;
	struct kobox_linux_pci_window windows[KOBOX_PCI_MEMORY_WINDOWS];
	int (*config_read)(void *context, uint32_t offset, uint32_t width,
			   uint32_t *value);
	int (*config_write)(void *context, uint32_t offset, uint32_t width,
			    uint32_t value);
	int (*memory_map)(void *context, void *address, uint64_t physical,
			  size_t length, unsigned int protection,
			  enum kobox_mmio_cache cache);
	int (*memory_unmap)(void *context, void *address, size_t length);
	int (*memory_read)(void *context, uint64_t physical, unsigned int width, uint64_t *value);
	int (*memory_write)(void *context, uint64_t physical, unsigned int width, uint64_t value);
};

struct kobox_linux_pci_report {
	size_t size;
	uint32_t scans;
	uint32_t capabilities;
	uint32_t removals;
	uint32_t phase;
	uint32_t mappings;
	uint32_t cache_mode;
	uint32_t revoked_mappings;
	uint64_t bar_start;
	uint64_t bar_size;
	uint64_t warnings;
	int result;
};

#ifdef __KERNEL__
struct pci_host_bridge;
int kobox_linux_pci_scan(const struct kobox_linux_pci_host *host,
			struct pci_host_bridge **bridge_out);
int kobox_linux_pci_remove(struct pci_host_bridge *bridge);
int kobox_linux_pci_verify(const struct kobox_linux_pci_host *host,
			  struct kobox_linux_pci_report *report,
			  int (*mapping_faults)(void *address));
#endif

#endif
