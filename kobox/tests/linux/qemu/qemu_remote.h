/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_QEMU_REMOTE_H
#define KOBOX_POSIX_QEMU_REMOTE_H

#include "qemu_pci.h"
#include "../../../host/posix/device_channel.h"
#include "../../../host/posix/device_proxy.h"
#include <stdbool.h>

struct kobox_qemu_remote;

/* Conformance supervision, not a device wire API. Returned native FDs are
 * caller-owned stale capabilities retained across the sandbox's death.
 */
struct kobox_qemu_remote_failure {
	int backing, request, event;
	size_t length;
	void *shared_alias, *private_alias;
	bool checkpoint, killed, reaped, revoked;
	bool dma_pending, dma_blocked;
	unsigned int hardware_terminated;
	struct kobox_device_packet retired_irq;
};

struct kobox_qemu_remote_trial {
	const struct kobox_device_packet *events;
	size_t count;
	unsigned int dma_write;
	void (*before_revoke)(void);
};

/* Fork before runtime boot/threads. The parent owns hardware and observes
 * actual child death; only the child calls entry and loads the Linux core.
 * Object 1 is virtio-gpu; optional object 2 is the independent EDU consumer.
 * Authority is set here, not by a message from the sandbox.
 */
int kobox_qemu_remote_run(const char *executable, uint64_t generation,
			  bool consumer, bool virgl,
			  int (*entry)(void *, struct kobox_qemu_remote *),
			  void *context, struct kobox_qemu_remote_failure *failure,
			  const struct kobox_qemu_remote_trial *trial);
/* A dedicated, pre-authorized test pipe; it cannot issue a device command. */
_Noreturn void kobox_qemu_remote_checkpoint(struct kobox_qemu_remote *remote);
int kobox_qemu_remote_open(struct kobox_qemu_remote *remote, uint64_t object,
			   int ram_descriptor, size_t ram_size, uint64_t delay,
			   struct kobox_posix_proxy_device **out);
int kobox_qemu_remote_start(struct kobox_qemu_remote *remote,
			    int (*notify)(void *, unsigned int), void *context);
/* Close all objects before stop; stop joins delivery before context release. */
int kobox_qemu_remote_stop(struct kobox_qemu_remote *remote);

#endif
