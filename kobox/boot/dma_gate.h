/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DMA_GATE_H
#define KOBOX_BOOT_DMA_GATE_H

#include "dma_host.h"
#include "pci_host.h"

/* Conformance-device controls, never part of the DMA machine port. */
struct kobox_linux_dma_test {
	struct kobox_linux_dma_host host;
	void *context;
	int (*transfer)(void *context, uint64_t iova, void *bytes,
			size_t length, unsigned int write);
	void (*fail_map)(void *context, unsigned int after);
	void (*panic_report)(void *context, unsigned int page_references,
			     unsigned int online_cpus);
};

struct kobox_linux_dma_report {
	size_t size;
	unsigned int cases;
	unsigned int phase;
	unsigned int drained;
	unsigned int cpu_mask;
	unsigned int parallel_rounds;
	unsigned int sole_pins;
	unsigned int pressure_pages;
	unsigned int pressure_reclaimed;
	unsigned int pressure_line;
	int result;
	uint64_t warnings;
};

#ifdef __KERNEL__
#ifdef KOBOX_RUNTIME_GATES
#include <linux/gfp_types.h>
struct xarray;

/* Gate-only bridges exercise the production helpers, not a second lock
 * implementation. The after-lock hook ignores all unarmed DMA requests.
 */
void kobox_linux_dma_gate_xarray_locked(struct xarray *xa,
	unsigned long index, bool releasing);
int kobox_linux_dma_gate_xa_reserve(struct xarray *xa,
	unsigned long index, gfp_t gfp);
void kobox_linux_dma_gate_xa_release(struct xarray *xa, unsigned long index);
int kobox_linux_dma_xarray_verify(void);
#endif
int kobox_linux_dma_verify(const struct kobox_linux_pci_host *pci,
			   const struct kobox_linux_dma_test *test,
			   struct kobox_linux_dma_report *report);
#endif

#endif
