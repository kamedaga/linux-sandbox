// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "pci_config_fixture.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#define BAR_BASE 0x120000000ULL
#define BAR_SIZE 0x4000U
#define BAR_FLAGS (PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH)
#define SMALL_BASE 0x30000100ULL
#define SMALL_SECOND_BASE 0x30000300ULL
#define SMALL_SIZE 0x100U
#define SMALL_PAGE (SMALL_BASE & ~4095ULL)

static uint32_t get_value(const unsigned char *bytes, uint32_t width)
{
	uint32_t value = 0, i;

	for (i = 0; i < width; i++)
		value |= (uint32_t)bytes[i] << (8 * i);
	return value;
}

static void put_value(unsigned char *bytes, uint32_t width, uint32_t value)
{
	uint32_t i;

	for (i = 0; i < width; i++)
		bytes[i] = value >> (8 * i);
}

static int valid_range(uint32_t offset, uint32_t width)
{
	return (width == 1 || width == 2 || width == 4) &&
	       offset <= PCI_CFG_SPACE_EXP_SIZE - width && !(offset & (width - 1));
}

static int read_config(void *context, uint32_t offset, uint32_t width,
		       uint32_t *value)
{
	struct kobox_pci_config_fixture *fixture = context;

	if (!value || !valid_range(offset, width))
		return -EINVAL;
	fixture->reads++;
	*value = get_value(fixture->bytes + offset, width);
	return 0;
}

static int write_config(void *context, uint32_t offset, uint32_t width,
			uint32_t value)
{
	struct kobox_pci_config_fixture *fixture = context;
	uint32_t i;

	if (!valid_range(offset, width))
		return -EINVAL;
	fixture->writes++;
	if (offset >= PCI_BASE_ADDRESS_0 && offset <= PCI_BASE_ADDRESS_5) {
		uint32_t bit = 1U << ((offset - PCI_BASE_ADDRESS_0) / 4);

		if (width != 4)
			return -EINVAL;
		if (value == UINT32_MAX) {
			fixture->sizing++;
			fixture->sizing_pending |= bit;
			if (get_value(fixture->bytes + PCI_COMMAND, 2) &
			    (PCI_COMMAND_MEMORY | PCI_COMMAND_IO))
				fixture->bad_sizing++;
		} else {
			fixture->sizing_pending &= ~bit;
		}
		if (offset == PCI_BASE_ADDRESS_0)
			value = (value & ~(BAR_SIZE - 1)) | BAR_FLAGS;
		else if (offset == PCI_BASE_ADDRESS_2 || offset == PCI_BASE_ADDRESS_3)
			value &= ~(SMALL_SIZE - 1);
		else if (offset != PCI_BASE_ADDRESS_1)
			value = 0;
		put_value(fixture->bytes + offset, width, value);
		return 0;
	}
	/* Hardware read-only bits ignore writes. In particular absent BARs and
	 * the absent expansion ROM must remain zero during sizing probes.
	 */
	for (i = 0; i < width; i++) {
		unsigned char mask = fixture->writable[offset + i];

		fixture->bytes[offset + i] = (fixture->bytes[offset + i] & ~mask) |
					     ((value >> (8 * i)) & mask);
	}
	/* Decode must not resume while either half of a sized BAR still holds
	 * the probe value. This complements the decode-off check above.
	 */
	if (fixture->sizing_pending &&
	    (get_value(fixture->bytes + PCI_COMMAND, 2) &
	     (PCI_COMMAND_MEMORY | PCI_COMMAND_IO)))
		fixture->bad_sizing++;
	return 0;
}

static int map_memory(void *context, void *address, uint64_t physical,
		      size_t length, unsigned int protection, enum kobox_mmio_cache cache)
{
	struct kobox_pci_config_fixture *fixture = context;
	int prot = 0;
	void *mapped;
	uint64_t backing_offset;

	if (!length || physical % 4096 || length % 4096 ||
	    protection & ~(KOBOX_LINUX_MEMORY_READ | KOBOX_LINUX_MEMORY_WRITE))
		return -EINVAL;
	if (physical >= BAR_BASE && physical - BAR_BASE < BAR_SIZE &&
	    length <= BAR_SIZE - (physical - BAR_BASE))
		backing_offset = physical - BAR_BASE;
	else if (physical == SMALL_PAGE && length == 4096)
		/* This fixture owns the entire page, including both small BARs
		 * and padding. A physical backend must establish that authority.
		 */
		backing_offset = BAR_SIZE;
	else
		return -ERANGE;
	if (cache != KOBOX_MMIO_UC && cache != KOBOX_MMIO_UC_MINUS &&
	    cache != KOBOX_MMIO_WC)
		return -EOPNOTSUPP;
	/* Device-private conformance register: inject a VM failure on the Nth
	 * page publication, allowing rollback after a partial host mapping.
	 */
	if (get_value(fixture->bytes + 0x148, 4)) {
		uint32_t remaining = get_value(fixture->bytes + 0x148, 4) - 1;

		put_value(fixture->bytes + 0x148, 4, remaining);
		if (!remaining)
			return -ENOMEM;
	}
	if (protection & KOBOX_LINUX_MEMORY_READ)
		prot |= PROT_READ;
	if (protection & KOBOX_LINUX_MEMORY_WRITE)
		prot |= PROT_WRITE;
	/* Transaction apertures are deliberately inaccessible to native loads. */
	if (fixture->transaction_memory)
		prot = PROT_NONE;
	/* This is an emulated register aperture, not physical MMIO. The fixture
	 * records Linux's requested type; memfd RAM cannot certify hardware PAT
	 * or PCI posted-write behavior. A physical host must enforce that type.
	 */
	mapped = mmap(address, length, prot, MAP_SHARED | MAP_FIXED,
		      fixture->backing, fixture->backing_offset + backing_offset);
	if (mapped == MAP_FAILED)
		return -errno;
	fixture->map_calls++;
	fixture->cache_seen |= 1U << cache;
	return 0;
}

static int unmap_memory(void *context, void *address, size_t length)
{
	struct kobox_pci_config_fixture *fixture = context;

	if (!address || !length || length % 4096 || (uintptr_t)address % 4096)
		return -EINVAL;
	if (mmap(address, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		 -1, 0) == MAP_FAILED)
		return -errno;
	fixture->unmap_calls++;
	return 0;
}

static unsigned char *transaction_address(struct kobox_pci_config_fixture *fixture,
					 uint64_t physical, unsigned int width)
{
	if (!fixture->transaction_memory ||
	    (width != 1 && width != 2 && width != 4 && width != 8))
		return NULL;
	if (physical >= BAR_BASE && physical - BAR_BASE <= BAR_SIZE - width)
		return fixture->transaction_memory + physical - BAR_BASE;
	if (physical >= SMALL_PAGE && physical - SMALL_PAGE <= 4096 - width)
		return fixture->transaction_memory + BAR_SIZE + physical - SMALL_PAGE;
	return NULL;
}

static int read_memory(void *context, uint64_t physical, unsigned int width,
		       uint64_t *value)
{
	struct kobox_pci_config_fixture *fixture = context;
	unsigned char *address = transaction_address(fixture, physical, width);
	unsigned int i;

	if (!address || !value)
		return -ERANGE;
	*value = 0;
	for (i = 0; i < width; i++)
		*value |= (uint64_t)address[i] << (8 * i);
	fixture->transaction_reads |= width;
	return 0;
}

static int write_memory(void *context, uint64_t physical, unsigned int width,
			uint64_t value)
{
	struct kobox_pci_config_fixture *fixture = context;
	unsigned char *address = transaction_address(fixture, physical, width);
	unsigned int i;

	if (!address)
		return -ERANGE;
	for (i = 0; i < width; i++)
		address[i] = value >> (8 * i);
	fixture->transaction_writes |= width;
	return 0;
}

void kobox_pci_config_fixture_transactions(struct kobox_linux_pci_host *host)
{
	host->memory_read = read_memory;
	host->memory_write = write_memory;
}

void kobox_pci_config_fixture_init(struct kobox_pci_config_fixture *fixture,
				 struct kobox_linux_pci_host *host)
{
	memset(fixture, 0, sizeof(*fixture));
	fixture->backing = -1;
	put_value(fixture->bytes + PCI_VENDOR_ID, 4, 0x10501af4);
	put_value(fixture->bytes + PCI_COMMAND, 2, PCI_COMMAND_MEMORY);
	put_value(fixture->writable + PCI_COMMAND, 2,
		  PCI_COMMAND_MEMORY | PCI_COMMAND_IO | PCI_COMMAND_MASTER |
		  PCI_COMMAND_INTX_DISABLE);
	put_value(fixture->bytes + PCI_STATUS, 2, PCI_STATUS_CAP_LIST);
	put_value(fixture->bytes + PCI_CLASS_REVISION, 4, 0x03000001);
	put_value(fixture->bytes + PCI_BASE_ADDRESS_0, 4,
		  (uint32_t)BAR_BASE | BAR_FLAGS);
	put_value(fixture->bytes + PCI_BASE_ADDRESS_1, 4, BAR_BASE >> 32);
	put_value(fixture->bytes + PCI_BASE_ADDRESS_2, 4, SMALL_BASE);
	put_value(fixture->bytes + PCI_BASE_ADDRESS_3, 4, SMALL_SECOND_BASE);
	fixture->bytes[PCI_CAPABILITY_LIST] = 0x40;
	fixture->bytes[0x40 + PCI_CAP_LIST_ID] = PCI_CAP_ID_EXP;
	fixture->bytes[0x40 + PCI_CAP_LIST_NEXT] = 0x80;
	put_value(fixture->bytes + 0x40 + PCI_EXP_FLAGS, 2, 2);
	put_value(fixture->writable + 0x40 + PCI_EXP_DEVCTL, 2, 0x7fff);
	put_value(fixture->writable + 0x40 + PCI_EXP_LNKCTL, 2, 0x0fff);
	put_value(fixture->writable + 0x40 + PCI_EXP_DEVCTL2, 2, 0xffff);
	fixture->bytes[0x80 + PCI_CAP_LIST_ID] = PCI_CAP_ID_MSI;
	fixture->bytes[0x80 + PCI_CAP_LIST_NEXT] = 0xa0;
	put_value(fixture->bytes + 0x80 + PCI_MSI_FLAGS, 2, PCI_MSI_FLAGS_64BIT);
	put_value(fixture->writable + 0x80 + PCI_MSI_FLAGS, 2,
		  PCI_MSI_FLAGS_ENABLE | PCI_MSI_FLAGS_QSIZE);
	fixture->bytes[0xa0 + PCI_CAP_LIST_ID] = PCI_CAP_ID_MSIX;
	put_value(fixture->bytes + 0xa0 + PCI_MSIX_FLAGS, 2, 3);
	put_value(fixture->writable + 0xa0 + PCI_MSIX_FLAGS, 2,
		  PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);
	put_value(fixture->bytes + 0xa0 + PCI_MSIX_TABLE, 4, 0x2000);
	put_value(fixture->bytes + 0xa0 + PCI_MSIX_PBA, 4, 0x3000);
	put_value(fixture->bytes + 0x100, 4,
		  PCI_EXT_CAP_ID_DSN | (1U << 16) | (0x140U << 20));
	put_value(fixture->bytes + 0x104, 4, 0x12345678);
	put_value(fixture->bytes + 0x108, 4, 0x9abcdef0);
	put_value(fixture->bytes + 0x140, 4, PCI_EXT_CAP_ID_VNDR | (1U << 16));
	put_value(fixture->bytes + 0x144, 4, 0x00c10001);
	put_value(fixture->writable + 0x148, 4, UINT32_MAX);
	*host = (struct kobox_linux_pci_host) {
		.size = sizeof(*host), .context = fixture, .segment = 1,
		.window_count = 3, .windows = {
			{.start = BAR_BASE, .length = BAR_SIZE},
			{.start = SMALL_BASE, .length = SMALL_SIZE},
			{.start = SMALL_SECOND_BASE, .length = SMALL_SIZE},
		},
		.config_read = read_config, .config_write = write_config,
		.memory_map = map_memory,
		.memory_unmap = unmap_memory,
	};
}
