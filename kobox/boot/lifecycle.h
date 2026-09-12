/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_LIFECYCLE_H
#define KOBOX_BOOT_LIFECYCLE_H

#include "../task/host.h"

/* Process-local event port. The owner keeps callbacks/context alive until
 * process exit, including after module unload. ready/pending may not block,
 * allocate, or reenter hosted Linux. pending() runs in hardirq context and
 * returns zero while idle, one for stop, two for work, or a negative error.
 * The host validates authority/generation before publishing a request.
 */
struct kobox_linux_lifecycle {
	size_t size;
	void *context;
	int (*ready)(void *context);
	int (*pending)(void *context);
	/* Optional service callback, unlike ready/pending: runs in the opening
	 * Linux task and may call typed Linux APIs. It claims one published work
	 * item, publishes its completion, then returns zero. Negative values are
	 * infrastructure failures, not ioctl results. service is a borrowed
	 * process-local resource, never a peer pointer. No call after loop exit.
	 */
	int (*dispatch)(void *context, void *service);
};

#ifdef __KERNEL__
int kobox_linux_lifecycle_wait(const struct kobox_linux_lifecycle *host);
int kobox_linux_lifecycle_serve(const struct kobox_linux_lifecycle *host,
			       void *service);
void kobox_linux_lifecycle_interrupt(void);
#endif

#endif
