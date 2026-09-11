/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_RESOURCE_PORT_H
#define KOBOX_BOOT_RESOURCE_PORT_H

#include "../mm/host.h"

struct kobox_linux_resource_binding {
	uint64_t generation;
	uint64_t object_id;
	uint64_t rights;
	uint32_t type;
	void *object;
	const void *operations;
};

/* Process-local, immutable bootstrap attachment. lookup is a nonblocking
 * metadata lookup: it must not perform I/O, allocate, or enter hosted Linux.
 * Errors use negative Linux errno, translated by each host adapter.
 * The launch owner retains all objects until native modules and asynchronous
 * users are gone. This interface grants no reset or generation-change right.
 */
struct kobox_linux_resource_port {
	size_t size;
	uint64_t generation;
	void *context;
	int (*lookup)(void *context, const char *module_name, uint32_t slot,
		      size_t index, uint64_t rights, const uint8_t digest[32],
		      struct kobox_linux_resource_binding *binding);
};

#ifdef __KERNEL__
struct module;

int kobox_linux_resource_port_install(const struct kobox_linux_resource_port *port);
/* NULL module selects the fixed core; module identity is native Linux's name,
 * not a caller-created kobox init(context) record.
 */
int kobox_linux_resource_bind(const struct module *module, uint32_t slot,
			     size_t index, uint64_t rights,
			     const uint8_t digest[32],
			     struct kobox_linux_resource_binding *binding);
#endif

#endif
