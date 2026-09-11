/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DMA_HOST_H
#define KOBOX_BOOT_DMA_HOST_H

#include "../memory/host.h"

#define KOBOX_DMA_DEVICE_READ 1U
#define KOBOX_DMA_DEVICE_WRITE 2U

/* A host-authorized, isolated DMA domain. Addresses are IOVAs and offsets
 * in the registered Linux RAM backing, never CPU pointers. Leaf callbacks
 * run with guest IRQs disabled. map publishes synchronously and leaves no
 * mapping on error; unmap invalidates device translations and drains their
 * in-flight accesses before returning success. A failed unmap is fatal:
 * Linux must not reuse that IOVA or drop its RAM references.
 *
 * enable(false) blocks device access and drains it synchronously. The host
 * context outlives detach. This interface does not allocate RAM or IOVAs.
 */
struct kobox_linux_dma_host {
	size_t size;
	void *context;
	uint64_t aperture_start;
	uint64_t aperture_end;
	unsigned int coherent;
	int (*enable)(void *context, unsigned int enabled);
	int (*map)(void *context, uint64_t iova, uint64_t ram_offset,
		   size_t length, unsigned int protection);
	int (*unmap)(void *context, uint64_t iova, size_t length);
};

#ifdef __KERNEL__
struct device;
struct kobox_linux_dma_port;
int kobox_linux_dma_attach(struct device *device,
			   const struct kobox_linux_dma_host *host,
			   struct kobox_linux_dma_port **out);
/* Caller stops new DMA API calls first. EBUSY preserves live mappings. */
int kobox_linux_dma_detach(struct kobox_linux_dma_port *port);
#endif

#endif
