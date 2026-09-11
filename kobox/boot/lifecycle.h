/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_LIFECYCLE_H
#define KOBOX_BOOT_LIFECYCLE_H

#include "../task/host.h"

/* Process-local event port. The owner keeps callbacks/context alive until
 * process exit, including after module unload. Neither callback may block,
 * allocate, or reenter hosted Linux. pending() runs in hardirq context and
 * returns zero while running, one for stop, or a negative terminal error.
 * The host validates authority/generation before publishing a request.
 */
struct kobox_linux_lifecycle {
	size_t size;
	void *context;
	int (*ready)(void *context);
	int (*pending)(void *context);
};

#ifdef __KERNEL__
int kobox_linux_lifecycle_wait(const struct kobox_linux_lifecycle *host);
void kobox_linux_lifecycle_interrupt(void);
#endif

#endif
