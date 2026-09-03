/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_MEMORY_RESOURCE_INTERFACE_H
#define KOBOX_MEMORY_RESOURCE_INTERFACE_H

#include "module_context.h"

#include <stddef.h>

struct kobox_memory_arena_resource_operations {
	struct kobox_resource_interface_operations base;
	int (*mapped_range)(void *object, void **address_out,
			    size_t *length_out);
};

#endif
