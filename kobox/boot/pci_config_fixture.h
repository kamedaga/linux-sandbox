/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_PCI_CONFIG_FIXTURE_H
#define KOBOX_BOOT_PCI_CONFIG_FIXTURE_H

#include "pci_host.h"
#include "../../include/uapi/linux/pci_regs.h"

/* Emulated device registers for conformance, never used for physical PCI.
 * The PCI core must discover these through ordinary config transactions.
 * Its port serializes callers; no allocation or blocking in these callbacks.
 */
struct kobox_pci_config_fixture {
	unsigned char bytes[PCI_CFG_SPACE_EXP_SIZE];
	unsigned char writable[PCI_CFG_SPACE_EXP_SIZE];
	uint32_t reads;
	uint32_t writes;
	uint32_t sizing;
	uint32_t bad_sizing;
	uint32_t sizing_pending;
	int backing;
	uint64_t backing_offset;
	uint32_t map_calls;
	uint32_t unmap_calls;
	uint32_t cache_seen;
	unsigned char *transaction_memory;
	uint32_t transaction_reads;
	uint32_t transaction_writes;
};

void kobox_pci_config_fixture_init(struct kobox_pci_config_fixture *fixture,
				 struct kobox_linux_pci_host *host);
void kobox_pci_config_fixture_transactions(struct kobox_linux_pci_host *host);

#endif
