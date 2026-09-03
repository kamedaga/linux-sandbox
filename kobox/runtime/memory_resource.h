/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_MEMORY_RESOURCE_H
#define KOBOX_MEMORY_RESOURCE_H

#include "memory_resource_interface.h"
#include "resource_runtime.h"

#include <stddef.h>

int kobox_linux_memory_resource_import(
	void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles,
	size_t handle_count, void **native_object_out,
	const struct kobox_resource_interface_operations **operations_out);
void kobox_linux_memory_resource_release(void *context, void *native_object);

#endif
