/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_MODULE_CONTEXT_H
#define KOBOX_MODULE_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#define KOBOX_MODULE_INTERFACE_IDENTITY_SIZE 4u
#define KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER { 'd', 'e', 'v', '\0' }
#define KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE 32u
#define KOBOX_MODULE_CONTEXT_SIZE 56u

enum kobox_module_resource_status {
	KOBOX_MODULE_RESOURCE_OK = 0,
	KOBOX_MODULE_RESOURCE_INVALID_ARGUMENT,
	KOBOX_MODULE_RESOURCE_NOT_VISIBLE,
	KOBOX_MODULE_RESOURCE_ABSENT,
	KOBOX_MODULE_RESOURCE_STALE,
	KOBOX_MODULE_RESOURCE_RIGHTS,
	KOBOX_MODULE_RESOURCE_INTERFACE,
};

enum kobox_module_resource_state {
	KOBOX_MODULE_RESOURCE_ABSENT_STATE = 0,
	KOBOX_MODULE_RESOURCE_PRESENT_STATE = 1,
};

struct kobox_module_resource_handle {
	uint64_t generation;
	uint64_t object_id;
};

struct kobox_module_resource_info {
	uint32_t resource_type;
	uint32_t reserved;
	uint64_t granted_rights;
};

struct kobox_resource_interface_operations {
	uint32_t size;
	uint8_t identity[KOBOX_MODULE_INTERFACE_IDENTITY_SIZE];
};

struct kobox_module_resource_binding {
	const struct kobox_resource_interface_operations *operations;
	void *object;
};

struct kobox_module_context;

struct kobox_module_runtime_operations {
	uint32_t size;
	uint8_t identity[KOBOX_MODULE_INTERFACE_IDENTITY_SIZE];

	int (*resource_count)(const struct kobox_module_context *context,
			      uint32_t slot_id, uint32_t *state_out,
			      size_t *count_out);
	int (*resource_acquire)(const struct kobox_module_context *context,
				uint32_t slot_id, size_t object_index,
				uint64_t required_rights,
				struct kobox_module_resource_handle *handle_out);
	int (*resource_bind)(
		const struct kobox_module_context *context,
		struct kobox_module_resource_handle handle,
		const uint8_t expected_interface_digest
			[KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE],
		struct kobox_module_resource_binding *binding_out);
	int (*resource_info)(const struct kobox_module_context *context,
			     struct kobox_module_resource_handle handle,
			     struct kobox_module_resource_info *info_out);
};

struct kobox_module_context {
	uint32_t size;
	uint8_t identity[KOBOX_MODULE_INTERFACE_IDENTITY_SIZE];
	uint64_t generation;
	uint32_t node_id;
	uint32_t reserved;
	const void *resource_view;
	const struct kobox_module_runtime_operations *runtime_operations;
	const void *core_operations;
	uint32_t logical_cpu_count;
	uint32_t reserved2;
};

_Static_assert(sizeof(struct kobox_module_context) ==
	       KOBOX_MODULE_CONTEXT_SIZE,
	       "module context layout mismatch");
_Static_assert(offsetof(struct kobox_module_context, logical_cpu_count) == 48,
	       "module context CPU count offset mismatch");

typedef int (*kobox_module_lifecycle_fn)(
	const struct kobox_module_context *context);

#endif
