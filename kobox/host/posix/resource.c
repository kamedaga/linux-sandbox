// SPDX-License-Identifier: GPL-2.0-only
#include "resource.h"

#include <stdlib.h>

static void *allocate_zeroed(void *context, size_t count, size_t size)
{
	(void)context;
	return calloc(count, size);
}

static void release(void *context, void *allocation)
{
	(void)context;
	free(allocation);
}

static int import_object(void *context, const kb2_resource_grant_slot_t *slot,
			 const kb2_resource_grant_object_t *object,
			 const struct kobox_resource_handle *handles, size_t count,
			 void **out,
			 const struct kobox_resource_interface_operations **operations)
{
	const struct kobox_posix_resource_config *config = context;
	struct kobox_resource_native_handle *native = NULL;
	size_t index;
	int result;

	if (count) {
		native = calloc(count, sizeof(*native));
		if (!native)
			return -1;
	}
	for (index = 0; index < count; index++) {
		if (!handles[index].handle) {
			free(native);
			return -1;
		}
		native[index].role = handles[index].role;
		native[index].handle = *(const int *)handles[index].handle;
	}
	result = config->import_object(config->object_context, slot, object,
				       native, count, out, operations);
	free(native);
	return result;
}

enum kobox_resource_runtime_status kobox_posix_resource_open(
	const struct kobox_posix_resource_config *config,
	struct kobox_resource_runtime **runtime_out)
{
	struct kobox_resource_runtime_config common;
	const void **handles = NULL;
	enum kobox_resource_runtime_status result;
	size_t index;

	if (!runtime_out)
		return KOBOX_RESOURCE_RUNTIME_INVALID_ARGUMENT;
	*runtime_out = NULL;
	if (!config || !config->manifest || !config->grant)
		return KOBOX_RESOURCE_RUNTIME_INVALID_ARGUMENT;
	if (config->native_handle_count !=
	    kb2_resource_grant_handle_binding_count(config->grant) ||
	    (config->native_handle_count && !config->native_handles) ||
	    (kb2_resource_grant_object_count(config->grant) &&
	     (!config->import_object || !config->release_object)))
		return KOBOX_RESOURCE_RUNTIME_MALFORMED;
	if (config->native_handle_count) {
		handles = calloc(config->native_handle_count, sizeof(*handles));
		if (!handles)
			return KOBOX_RESOURCE_RUNTIME_NO_MEMORY;
	}
	for (index = 0; index < config->native_handle_count; index++)
		handles[index] = &config->native_handles[index];
	common = (struct kobox_resource_runtime_config) {
		.manifest = config->manifest, .grant = config->grant,
		.native_handles = handles,
		.native_handle_count = config->native_handle_count,
		.import_object = import_object,
		.import_context = (void *)config,
		.release_object = config->release_object,
		.release_context = config->object_context,
		.allocator = {
			.allocate_zeroed = allocate_zeroed, .release = release,
		},
	};
	result = kobox_resource_runtime_open(&common, runtime_out);
	free(handles);
	return result;
}
