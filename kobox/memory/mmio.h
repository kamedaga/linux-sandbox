/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_MEMORY_MMIO_H
#define KOBOX_MEMORY_MMIO_H

#include "host.h"

enum kobox_mmio_cache {
	KOBOX_MMIO_UC,
	KOBOX_MMIO_UC_MINUS,
	KOBOX_MMIO_WC,
	KOBOX_MMIO_WB,
	KOBOX_MMIO_WT,
	KOBOX_MMIO_WP,
};

/* Address and length describe a page-aligned span in the host-owned vmalloc
 * reservation. map must install exactly that span, or leave it unchanged.
 * It must enforce the requested cache type, never silently substitute WB.
 * These are leaf VM operations, with guest IRQs and preemption disabled.
 * unmap restores the inaccessible reservation and releases its host lease.
 */
struct kobox_mmio_host {
	void *context;
	uint64_t start;
	uint64_t length;
	int (*map)(void *context, void *address, uint64_t physical,
		   size_t length, unsigned int protection, enum kobox_mmio_cache cache);
	int (*unmap)(void *context, void *address, size_t length);
	/* Supply both for transaction-backed device registers, or neither for
	 * native mapped BARs. The span lease still belongs to map/unmap. These
	 * accesses have the requested width and complete in program order.
	 */
	int (*read)(void *context, uint64_t physical, unsigned int width, uint64_t *value);
	int (*write)(void *context, uint64_t physical, unsigned int width, uint64_t value);
};

#ifdef __KERNEL__
#include <asm/pgtable_types.h>

struct kobox_mmio_region;
int kobox_mmio_register(const struct kobox_mmio_host *host,
			struct kobox_mmio_region **out);
/* EBUSY keeps the region registered: live aliases still own its context. */
int kobox_mmio_unregister(struct kobox_mmio_region **regions, size_t count);
/* Only at final device-generation teardown, after every driver and borrowed
 * file has gone: retire orphaned Linux ioremap VMAs through Linux's own
 * iounmap path before the host BAR capability is released. */
int kobox_mmio_retire_orphan_ioremaps(struct kobox_mmio_region *region);
int kobox_mmio_publish(unsigned long address, pte_t pte, bool create);
int kobox_mmio_publish_range(unsigned long start, unsigned long end, pte_t first);
int kobox_mmio_reset(unsigned long start, unsigned long end, void *context,
		     int (*reset_ram)(void *, unsigned long, unsigned long));
int kobox_linux_memory_publish(unsigned long start, unsigned long end);
#endif

#endif
