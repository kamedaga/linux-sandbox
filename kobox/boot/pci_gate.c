// SPDX-License-Identifier: GPL-2.0-only

#include "pci_host.h"
#include "exception.h"

#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <asm/memtype.h>
#include <asm/pgtable.h>

static int small_bars(struct pci_dev *device,
		      struct kobox_linux_pci_report *report,
		      int (*mapping_faults)(void *address), int native_fault)
{
	void __iomem *first = NULL, *second = NULL, *alias = NULL;
	void *retired;
	int result = -EINVAL;

	if (pci_resource_start(device, 2) != 0x30000100 ||
	    pci_resource_start(device, 3) != 0x30000300 ||
	    pci_resource_len(device, 2) != 0x100 ||
	    pci_resource_len(device, 3) != 0x100 ||
	    !device->resource[2].parent || !device->resource[3].parent)
		return -EINVAL;
	first = pci_iomap_range(device, 2, 0x100, 1);
	if (first)
		goto out;
	first = pci_iomap(device, 2, 0);
	second = pci_iomap(device, 3, 0);
	alias = pci_iomap_range(device, 2, 0x80, 0x80);
	if (!first || !second || !alias ||
	    offset_in_page(first) != 0x100 || offset_in_page(second) != 0x300 ||
	    offset_in_page(alias) != 0x180)
		goto out;
	writel(0xabcdef02, first);
	writel(0xabcdef03, second);
	writel(0xabcdef04, alias);
	if (readl(first) != 0xabcdef02 || readl(second) != 0xabcdef03 ||
	    readl(first + 0x80) != 0xabcdef04 ||
	    mapping_faults((void __force *)alias) != native_fault)
		goto out;
	retired = (void __force *)alias;
	pci_iounmap(device, alias);
	alias = NULL;
	vm_unmap_aliases();
	if (mapping_faults(retired) != 1 || readl(first + 0x80) != 0xabcdef04)
		goto out;
	retired = (void __force *)first;
	pci_iounmap(device, first);
	first = NULL;
	vm_unmap_aliases();
	if (mapping_faults(retired) != 1 || readl(second) != 0xabcdef03)
		goto out;
	retired = (void __force *)second;
	pci_iounmap(device, second);
	second = NULL;
	vm_unmap_aliases();
	if (mapping_faults(retired) != 1)
		goto out;
	report->mappings += 3;
	report->revoked_mappings += 3;
	result = 0;
out:
	if (alias)
		pci_iounmap(device, alias);
	if (first)
		pci_iounmap(device, first);
	if (second)
		pci_iounmap(device, second);
	vm_unmap_aliases();
	return result;
}

static int register_accessors(void __iomem *mapping)
{
	u8 source[] = { 0x13, 0x27, 0x42, 0x81, 0xab, 0xcd, 0xef, 0x59, 0x65 };
	u8 copy[sizeof(source)];
	unsigned int i;

	writeq(0x123456789abcdef0ULL, mapping + 64);
	if (readq(mapping + 64) != 0x123456789abcdef0ULL)
		return -EINVAL;
	__raw_writel(0xabcdef01, mapping + 72);
	if (__raw_readl(mapping + 72) != 0xabcdef01)
		return -EINVAL;
	writel_relaxed(0x87654321, mapping + 76);
	if (readl_relaxed(mapping + 76) != 0x87654321)
		return -EINVAL;
	iowrite8(0x87, mapping + 80);
	iowrite16(0x6543, mapping + 82);
	iowrite32(0x210fedcb, mapping + 84);
	if (ioread8(mapping + 80) != 0x87 || ioread16(mapping + 82) != 0x6543 ||
	    ioread32(mapping + 84) != 0x210fedcb)
		return -EINVAL;
	/* A single unaligned x86 transaction may straddle two published pages. */
	writeq(0xfedcba9876543210ULL, mapping + PAGE_SIZE - 3);
	if (readq(mapping + PAGE_SIZE - 3) != 0xfedcba9876543210ULL)
		return -EINVAL;
	memcpy_toio(mapping + 128, source, sizeof(source));
	memcpy_fromio(copy, mapping + 128, sizeof(copy));
	if (memcmp(copy, source, sizeof(copy)))
		return -EINVAL;
	memset_io(mapping + 128, 0x5a, sizeof(copy));
	memcpy_fromio(copy, mapping + 128, sizeof(copy));
	for (i = 0; i < sizeof(copy); i++)
		if (copy[i] != 0x5a)
			return -EINVAL;
	return 0;
}

int kobox_linux_pci_verify(const struct kobox_linux_pci_host *host,
			  struct kobox_linux_pci_report *report,
			  int (*mapping_faults)(void *address))
{
	struct pci_host_bridge *bridge = NULL;
	struct pci_dev *device = NULL;
	void __iomem *mapping = NULL, *alias = NULL;
	void *retired;
	unsigned int pass;
	u32 value;
	u16 command;
	unsigned int level;
	pte_t *pte;
	int result;

	if (!report || report->size != sizeof(*report) || !mapping_faults)
		return -EINVAL;
	for (pass = 0; pass < 2; pass++) {
		report->phase = 1 + pass * 16;
		result = kobox_linux_pci_scan(host, &bridge);
		if (result)
			goto out;
		report->scans++;
		device = pci_get_slot(bridge->bus, host->devfn);
		result = -EINVAL;
		if (!device || device->driver || device->dev.driver ||
		    device->vendor != 0x1af4 || device->device != 0x1050 ||
		    device->cfg_size != PCI_CFG_SPACE_EXP_SIZE)
			goto remove;
		report->phase++;
		if (pci_find_capability(device, PCI_CAP_ID_EXP) != 0x40 ||
		    pci_find_capability(device, PCI_CAP_ID_MSI) != 0x80 ||
		    pci_find_capability(device, PCI_CAP_ID_MSIX) != 0xa0 ||
		    pci_find_ext_capability(device, PCI_EXT_CAP_ID_DSN) != 0x100 ||
		    pci_find_ext_capability(device, PCI_EXT_CAP_ID_VNDR) != 0x140)
			goto remove;
		report->capabilities += 5;
		report->bar_start = pci_resource_start(device, 0);
		report->bar_size = pci_resource_len(device, 0);
		if (report->bar_start != 0x120000000ULL || report->bar_size != 0x4000 ||
		    !(pci_resource_flags(device, 0) & IORESOURCE_MEM_64) ||
		    !(pci_resource_flags(device, 0) & IORESOURCE_PREFETCH) ||
		    !device->resource[0].parent)
			goto remove;
		if (small_bars(device, report, mapping_faults, !!host->memory_read))
			goto remove;
		report->phase++;
		if (pci_read_config_word(device, PCI_COMMAND, &command) ||
		    command != PCI_COMMAND_MEMORY ||
		    pci_read_config_dword(device, PCI_VENDOR_ID, &value) || value != 0x10501af4 ||
		    pci_read_config_dword(device, 1, &value) != PCIBIOS_BAD_REGISTER_NUMBER ||
		    pci_bus_read_config_dword(bridge->bus, host->devfn ^ 8,
					PCI_VENDOR_ID, &value) != PCIBIOS_DEVICE_NOT_FOUND)
			goto remove;
		mapping = pci_iomap_range(device, 0, report->bar_size, 1);
		if (mapping)
			goto remove;
		/* This device backend does not grant cached register mappings. */
		mapping = ioremap_cache(report->bar_start, report->bar_size);
		if (mapping)
			goto remove;
		mapping = ioremap(report->bar_start + report->bar_size, PAGE_SIZE);
		if (mapping)
			goto remove;
		mapping = ioremap(report->bar_start + report->bar_size - PAGE_SIZE,
				 2 * PAGE_SIZE);
		if (mapping)
			goto remove;
		vm_unmap_aliases();
		if (pci_write_config_dword(device, 0x148, 2))
			goto remove;
		mapping = pci_iomap(device, 0, 0);
		if (mapping)
			goto remove;
		vm_unmap_aliases();
		report->phase++;
		mapping = pci_iomap(device, 0, 0);
		alias = pci_iomap_range(device, 0, PAGE_SIZE, PAGE_SIZE);
		if (!mapping || !alias)
			goto remove;
		if (register_accessors(mapping))
			goto remove;
		writel(0x51c0ffee, mapping + PAGE_SIZE);
		if (readl(alias) != 0x51c0ffee)
			goto remove;
		writeb(0xa5, alias + 4);
		writew(0xcafe, alias + 6);
		if (readb(mapping + PAGE_SIZE + 4) != 0xa5 ||
		    readw(mapping + PAGE_SIZE + 6) != 0xcafe)
			goto remove;
		/* Removing a mapped aperture must preserve both the bus and mapping. */
		if (kobox_linux_pci_remove(bridge) != -EBUSY ||
		    readl(alias) != 0x51c0ffee)
			goto remove;
		report->mappings += 2;
		report->phase++;
		if (mapping_faults((void __force *)alias) != !!host->memory_read)
			goto remove;
		retired = (void __force *)alias;
		pci_iounmap(device, alias);
		alias = NULL;
		vm_unmap_aliases();
		if (mapping_faults(retired) != 1 ||
		    readl(mapping + PAGE_SIZE) != 0x51c0ffee)
			goto remove;
		report->revoked_mappings++;
		retired = (void __force *)mapping;
		pci_iounmap(device, mapping);
		mapping = NULL;
		vm_unmap_aliases();
		if (mapping_faults(retired) != 1)
			goto remove;
		report->revoked_mappings++;
		/* No UC alias remains when requesting WC. Inspect the Linux PTE as
	 * well as actual access so a silent WC-to-UC encoding fallback fails.
	 */
		mapping = pci_iomap_wc(device, 0, 0);
		if (!mapping)
			goto remove;
		report->phase++;
		pte = lookup_address((unsigned long)mapping, &level);
		if (!pte || level != PG_LEVEL_4K)
			goto remove;
		report->cache_mode = pgprot2cachemode(pte_pgprot(ptep_get(pte)));
		if (report->cache_mode != _PAGE_CACHE_MODE_WC ||
		    readl(mapping + PAGE_SIZE) != 0x51c0ffee)
			goto remove;
		/* Upstream PAT rejects a WT alias of the live WC range. */
		alias = ioremap_wt(report->bar_start, report->bar_size);
		if (alias)
			goto remove;
		report->mappings++;
		result = 0;
remove:
		if (alias)
			pci_iounmap(device, alias);
		if (mapping)
			pci_iounmap(device, mapping);
		alias = NULL;
		mapping = NULL;
		pci_dev_put(device);
		device = NULL;
		if (kobox_linux_pci_remove(bridge))
			return -EBUSY;
		bridge = NULL;
		rcu_barrier();
		report->removals++;
		if (result)
			goto out;
		report->phase++;
	}
out:
	report->warnings = kobox_linux_exception_warnings();
	if (!result && report->warnings)
		result = -EINVAL;
	report->result = result;
	return result;
}
