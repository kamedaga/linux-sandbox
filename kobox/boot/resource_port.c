// SPDX-License-Identifier: GPL-2.0-only

#include "resource_port.h"

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>

static struct kobox_linux_resource_port resource_port;
static bool installed;

int kobox_linux_resource_port_install(const struct kobox_linux_resource_port *port)
{
	if (installed)
		return -EBUSY;
	if (port && (port->size != sizeof(*port) || !port->generation ||
		     !port->context || !port->lookup))
		return -EINVAL;
	if (port)
		resource_port = *port;
	installed = true;
	return 0;
}

int kobox_linux_resource_bind(const struct module *module, uint32_t slot,
			     size_t index, uint64_t rights,
			     const uint8_t digest[32],
			     struct kobox_linux_resource_binding *binding)
{
	struct kobox_linux_resource_binding result = {0};
	int error;

	if (!binding)
		return -EINVAL;
	*binding = result;
	if (!slot || !digest)
		return -EINVAL;
	if (!resource_port.lookup)
		return -ENODEV;
	error = resource_port.lookup(resource_port.context,
				     module ? module->name : NULL, slot, index,
				     rights, digest, &result);
	if (error)
		return error < 0 ? error : -EPROTO;
	if (result.generation != resource_port.generation || !result.object_id ||
	    !result.object || !result.operations || (rights & ~result.rights))
		return -EPROTO;
	*binding = result;
	return 0;
}
