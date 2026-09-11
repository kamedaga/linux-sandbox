/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_RESOURCE_REGISTRY_H
#define KOBOX_BOOT_RESOURCE_REGISTRY_H

#include "resource_port.h"

struct kobox_resource_runtime;

/* The runtime remains owned by the process bootstrap. No resource is copied,
 * reimported or released by this adapter. Call before entering hosted Linux.
 */
int kobox_boot_resource_port(struct kobox_resource_runtime *runtime,
			     struct kobox_linux_resource_port *port);

#endif
