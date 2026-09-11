/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_VIRTIO_GATE_H
#define KOBOX_BOOT_VIRTIO_GATE_H

#include "pci_host.h"
#include "dma_host.h"
#include "irq_host.h"
#include "module_launch.h"
#include "exec_gate.h"

enum kobox_virtio_observation {
	KOBOX_VIRTIO_OBSERVE_NONE,
	KOBOX_VIRTIO_OBSERVE_REMOVE,
	KOBOX_VIRTIO_OBSERVE_SHARED,
};

struct kobox_linux_virtio_test {
	size_t size;
	const struct kobox_linux_pci_host *pci;
	const struct kobox_linux_dma_host *dma;
	const struct kobox_linux_irq_host *irq;
	const struct kobox_linux_pci_host *consumer_pci;
	const struct kobox_linux_dma_host *consumer_dma;
	const struct kobox_linux_native_module *modules;
	size_t count;
	const struct kobox_exec_test *client;
	uint32_t observation;
	/* Conformance only: the external owner kills at an observed real wait. */
	uint32_t death_cpu;
	int (*death_checkpoint)(void *context);
	int (*arm_fault)(void *context, uint64_t address);
	void *death_context;
};

struct kobox_linux_virtio_report {
	size_t size;
	uint32_t phase, line, loaded, unloaded, bound, vectors, nodes, drained;
	int result, cleanup;
	uint64_t warnings;
	uint64_t interrupts;
	struct kobox_exec_report client;
	char diagnostics[2048];
};

#endif
