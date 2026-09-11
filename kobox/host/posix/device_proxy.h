/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_DEVICE_PROXY_H
#define KOBOX_POSIX_DEVICE_PROXY_H

#include "../../boot/pci_host.h"
#include "../../boot/dma_host.h"
#include "../../boot/irq_host.h"

struct kobox_posix_device_proxy;
struct kobox_posix_proxy_device;

/* Serialized preboot construction. Channels are borrowed, native process-local
 * handles and must outlive the proxy. The trusted owner supplies generation
 * and admitted object/CPU counts; messages cannot extend that authority.
 * Close device users before stop; stop joins notification delivery. Destroy
 * refuses live leases. The channel owner closes the FDs after destroy.
 */
int kobox_posix_device_proxy_create(int request, int event, uint64_t generation,
	size_t objects, unsigned int cpus, struct kobox_posix_device_proxy **out);
int kobox_posix_device_proxy_open(struct kobox_posix_device_proxy *remote,
	uint64_t object, int ram_descriptor, size_t ram_size, uint64_t delay,
	struct kobox_posix_proxy_device **out);
int kobox_posix_device_proxy_start(struct kobox_posix_device_proxy *remote,
	int (*notify)(void *, unsigned int), void *context);
int kobox_posix_device_proxy_close(struct kobox_posix_proxy_device *device);
int kobox_posix_device_proxy_stop(struct kobox_posix_device_proxy *remote);
/* Statistics are stable only after delivery is joined. No verdict here. */
int kobox_posix_device_proxy_statistics(struct kobox_posix_device_proxy *remote,
	size_t *retired, size_t *delivered);
int kobox_posix_device_proxy_destroy(struct kobox_posix_device_proxy **remote);
const struct kobox_linux_pci_host *kobox_posix_device_proxy_pci(struct kobox_posix_proxy_device *device);
const struct kobox_linux_dma_host *kobox_posix_device_proxy_dma(struct kobox_posix_proxy_device *device);
const struct kobox_linux_irq_host *kobox_posix_device_proxy_irq(struct kobox_posix_proxy_device *device);

#endif
