/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_QEMU_PCI_H
#define KOBOX_POSIX_QEMU_PCI_H

#include "../../../boot/pci_host.h"
#include "../../../boot/dma_host.h"
#include "../../../boot/irq_host.h"
#include <stdbool.h>

struct kobox_qemu_pci;

/* QEMU is an external hardware backend, not a guest OS or a replacement for
 * the Linux driver. The RAM descriptor is borrowed and must remain open until
 * close. QEMU must include the qtest accelerator (no guest CPU execution).
 * All entry points serialize hardware access. Join callers before close.
 * Only the authorized function and its BAR aperture are exposed to Linux.
 * virgl selects the real GL device and SDL GL display; initialization failure
 * is returned, with no substitution of the 2D device. The GL Gate must also
 * verify the actual renderer identity to reject software fallback.
 */
int kobox_qemu_pci_create(struct kobox_qemu_pci **out, const char *executable,
			  int ram_descriptor, size_t ram_size, bool virgl);
const struct kobox_linux_pci_host *kobox_qemu_pci_host(struct kobox_qemu_pci *device);
const struct kobox_linux_dma_host *kobox_qemu_pci_dma(struct kobox_qemu_pci *device);
const struct kobox_linux_irq_host *kobox_qemu_pci_irq(struct kobox_qemu_pci *device);
/* notify only rings an OS CPU doorbell; never invoke a Linux handler here. */
int kobox_qemu_pci_irq_start(struct kobox_qemu_pci *device,
			     int (*notify)(void *context, unsigned int cpu), void *context);
/* Stop after route release, before destroying the notify context. */
int kobox_qemu_pci_irq_stop(struct kobox_qemu_pci *device);
/* Test-only delivery latency; the device still generates every MSI itself. */
int kobox_qemu_pci_irq_delay(struct kobox_qemu_pci *device, uint64_t nanoseconds);
/* Host-owner operation, serialized with start/stop/close and never called
 * from notify. Irrevocably rejects driver operations, drains VT-d access
 * and joins IRQ notification delivery. Does not require Linux cleanup.
 * Failure is not revocation proof: keep the backing quarantined. Success
 * says nothing about CPU RAM aliases; their owner must invalidate/reap them
 * separately before backing reuse. MMIO leases still require unmap.
 */
int kobox_qemu_pci_revoke(struct kobox_qemu_pci *device);
/* Emulator-owner fallback: actually terminate and reap the owned hardware
 * process. Unlike failed VT-d invalidation, successful reaping proves its
 * DMA has stopped. CPU alias/lease proof remains a separate obligation.
 */
int kobox_qemu_pci_terminate(struct kobox_qemu_pci *device);
int kobox_qemu_pci_close(struct kobox_qemu_pci *device);

/* Same machine/DMA adapter with QEMU's EDU transfer engine for destructive
 * translation tests. This is never a substitute for the virtio/DRM Gate.
 */
int kobox_qemu_pci_create_dma_test(struct kobox_qemu_pci **out, const char *executable,
				 int ram_descriptor, size_t ram_size);
/* A second hardware process sharing the same RAM, with independent VT-d
 * tables and a disjoint Linux PCI segment/MMIO aperture beside virtio-gpu.
 */
int kobox_qemu_pci_create_dma_consumer(struct kobox_qemu_pci **out, const char *executable,
				      int ram_descriptor, size_t ram_size);
int kobox_qemu_pci_clock_step(struct kobox_qemu_pci *device, uint64_t nanoseconds);
/* Conformance timing only: hold the external device clock while a native
 * driver submits real EDU DMA, then inspect its pending write and VT-d leaf.
 * The checkpoint requires the fixture's 64-byte transfer to page offset 128.
 */
int kobox_qemu_pci_clock_hold(struct kobox_qemu_pci *device);
int kobox_qemu_pci_dma_checkpoint(struct kobox_qemu_pci *device,
				 uint64_t *ram_offset, uint64_t *before);

/* Hardware-backing conformance only; CPU physical accesses are not DMA and
 * must never substitute for the IOMMU port or driver DMA tests.
 */
int kobox_qemu_pci_ram_read(struct kobox_qemu_pci *device, uint64_t offset,
			   uint64_t *value);
int kobox_qemu_pci_ram_write(struct kobox_qemu_pci *device, uint64_t offset,
			    uint64_t value);

#endif
