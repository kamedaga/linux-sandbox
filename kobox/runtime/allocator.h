/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_RUNTIME_ALLOCATOR_H
#define KOBOX_RUNTIME_ALLOCATOR_H

#include <stddef.h>

/* Pre-boot metadata allocator, not a replacement for upstream Linux MM.
 * Successful storage is zeroed and suitably aligned for any C object.
 * Release accepts NULL. Context outlives all allocations; no guest reentry.
 */
struct kobox_runtime_allocator {
	void *context;
	void *(*allocate_zeroed)(void *context, size_t count, size_t size);
	void (*release)(void *context, void *allocation);
};

#endif
