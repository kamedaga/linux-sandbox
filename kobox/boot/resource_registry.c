// SPDX-License-Identifier: GPL-2.0-only

#include "resource_registry.h"
#include "../runtime/resource_runtime.h"

/* This boundary returns Linux errors, independent of the native host errno. */
#include <asm-generic/errno.h>

static int lookup(void *context, const char *module_name, uint32_t slot,
		  size_t index, uint64_t rights, const uint8_t digest[32],
		  struct kobox_linux_resource_binding *binding)
{
	struct kobox_resource_snapshot snapshot;
	enum kobox_resource_lookup_result error;

	if (!binding)
		return -EINVAL;
	*binding = (struct kobox_linux_resource_binding) {0};
	error = kobox_resource_runtime_native_lookup(context, module_name, slot,
						    index, rights, digest, &snapshot);
	switch (error) {
	case KOBOX_RESOURCE_LOOKUP_OK: break;
	case KOBOX_RESOURCE_LOOKUP_INVALID: return -EINVAL;
	case KOBOX_RESOURCE_LOOKUP_ABSENT: return -ENOENT;
	case KOBOX_RESOURCE_LOOKUP_RIGHTS: return -EACCES;
	case KOBOX_RESOURCE_LOOKUP_INTERFACE: return -EPROTOTYPE;
	default: return -EINVAL;
	}
	*binding = (struct kobox_linux_resource_binding) {
		.generation = snapshot.generation, .object_id = snapshot.object_id,
		.rights = snapshot.rights, .type = snapshot.type,
		.object = snapshot.binding.object, .operations = snapshot.binding.operations,
	};
	return 0;
}

int kobox_boot_resource_port(struct kobox_resource_runtime *runtime,
			     struct kobox_linux_resource_port *port)
{
	if (!runtime || !port || !kobox_resource_runtime_generation(runtime))
		return -EINVAL;
	*port = (struct kobox_linux_resource_port) {
		.size = sizeof(*port), .generation = kobox_resource_runtime_generation(runtime),
		.context = runtime, .lookup = lookup,
	};
	return 0;
}
