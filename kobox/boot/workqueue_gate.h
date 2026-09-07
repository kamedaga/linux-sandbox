/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_BOOT_WORKQUEUE_GATE_H
#define KOBOX_LINUX_BOOT_WORKQUEUE_GATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Process-local diagnostics, not a host or wire ABI. */
struct kobox_linux_workqueue_report {
	size_t size;
	char queue[24];
	char scenario[32];
	uint32_t cpu;
	uint32_t phase;
	uint32_t line;
	uint32_t cases;
	uint32_t callbacks;
	uint32_t errors;
	uint32_t rescued;
	uint32_t pressure_pages;
	uint64_t warnings;
};

/* Failure retains asynchronous storage and requires immediate process exit. */
int kobox_linux_workqueue_verify(struct kobox_linux_workqueue_report *report);

#endif
