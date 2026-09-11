/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_IRQ_GATE_H
#define KOBOX_BOOT_IRQ_GATE_H

#include "irq_host.h"
#include "pci_host.h"

struct kobox_linux_dma_test;

struct kobox_linux_irq_test_state {
	unsigned int emitted[3], pending[3], masked[3], msix_pba, function_mask, live, hold;
	unsigned int retired_deliveries;
	unsigned int queued;
};

enum kobox_linux_irq_test_checkpoint {
	KOBOX_IRQ_HOLD_ENTER,
	KOBOX_IRQ_HOLD_WAIT,
	KOBOX_IRQ_HOLD_RELEASE_AFTER_DELAY,
	KOBOX_IRQ_HOLD_DONE,
	KOBOX_IRQ_HOLD_ABORT,
};

struct kobox_linux_irq_test {
	struct kobox_linux_irq_host host;
	const struct kobox_linux_dma_test *dma;
	int (*fire)(void *context, enum kobox_linux_irq_mode mode, unsigned int index);
	int (*clear)(void *context, enum kobox_linux_irq_mode mode, unsigned int index);
	int (*snapshot)(void *context, struct kobox_linux_irq_test_state *state);
	void (*fail_allocate)(void *context, unsigned int after);
	int (*checkpoint)(void *context, enum kobox_linux_irq_test_checkpoint checkpoint);
	int (*capture)(void *context, enum kobox_linux_irq_mode mode);
	int (*replay_retired)(void *context, enum kobox_linux_irq_mode mode);
	int (*dma_fire)(void *context, enum kobox_linux_irq_mode mode,
			unsigned int index, uint64_t iova, uint32_t value);
	void (*pause_delivery)(void *context, unsigned int paused);
};

/* Hardware-facing IRQ conformance, not physical-device certification. */
struct kobox_linux_irq_report {
	size_t size;
	unsigned int vectors, deliveries, cpu_mask, modes, rounds, masks, migrations;
	unsigned int rollbacks, synchronizations, stale, dma, pending_free, line, drained;
	uint64_t warnings;
	int result;
};

#ifdef __KERNEL__
int kobox_linux_irq_verify(const struct kobox_linux_pci_host *pci,
			   const struct kobox_linux_irq_test *test,
			   struct kobox_linux_irq_report *report);
#endif
#endif
