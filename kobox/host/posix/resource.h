/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_RESOURCE_H
#define KOBOX_POSIX_RESOURCE_H

#include "../../runtime/resource_runtime.h"

struct kobox_resource_native_handle {
	uint32_t role;
	int handle;
};

typedef int (*kobox_posix_resource_import_fn)(
	void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles, size_t handle_count,
	void **native_object_out,
	const struct kobox_resource_interface_operations **operations_out);

/* Descriptors and callback inputs are borrowed only during import. Imported
 * objects and release context remain live until the common registry closes.
 */
struct kobox_posix_resource_config {
	const kb2_closure_manifest_t *manifest;
	const kb2_resource_grant_t *grant;
	const int *native_handles;
	size_t native_handle_count;
	kobox_posix_resource_import_fn import_object;
	kobox_resource_release_fn release_object;
	void *object_context;
};

enum kobox_resource_runtime_status kobox_posix_resource_open(
	const struct kobox_posix_resource_config *config,
	struct kobox_resource_runtime **runtime_out);

#endif
