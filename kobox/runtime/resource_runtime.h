/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_RESOURCE_RUNTIME_H
#define KOBOX_RESOURCE_RUNTIME_H

#include "module_context.h"

#include <kobox2/closure_manifest.h>
#include <kobox2/resource_grant.h>

#include <stddef.h>
#include <stdint.h>

struct kobox_resource_runtime;

struct kobox_resource_native_handle {
	uint32_t role;
	int handle;
};

typedef int (*kobox_resource_import_fn)(
	void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles,
	size_t handle_count, void **native_object_out,
	const struct kobox_resource_interface_operations **operations_out);
typedef void (*kobox_resource_release_fn)(void *context,
					 void *native_object);

struct kobox_resource_runtime_config {
	const kb2_closure_manifest_t *manifest;
	const kb2_resource_grant_t *grant;
	const int *native_handles;
	size_t native_handle_count;
	kobox_resource_import_fn import_object;
	kobox_resource_release_fn release_object;
	void *object_context;
};

enum kobox_resource_runtime_status {
	KOBOX_RESOURCE_RUNTIME_OK = 0,
	KOBOX_RESOURCE_RUNTIME_INVALID_ARGUMENT,
	KOBOX_RESOURCE_RUNTIME_NO_MEMORY,
	KOBOX_RESOURCE_RUNTIME_MALFORMED,
	KOBOX_RESOURCE_RUNTIME_IMPORT_FAILURE,
};

enum kobox_resource_runtime_status kobox_resource_runtime_open(
	const struct kobox_resource_runtime_config *config,
	struct kobox_resource_runtime **runtime_out);
const void *kobox_resource_runtime_view(
	const struct kobox_resource_runtime *runtime, uint32_t node_id);
uint64_t kobox_resource_runtime_generation(
	const struct kobox_resource_runtime *runtime);
const struct kobox_module_runtime_operations *
kobox_resource_runtime_operations(void);
void kobox_resource_runtime_close(struct kobox_resource_runtime **runtime);

#endif
