/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_DEVICE_PCI_BRIDGE_H
#define KOBOX_DEVICE_PCI_BRIDGE_H

#include "../runtime/module_context.h"

int kobox_linux_device_pci_core_init(
	const struct kobox_module_context *context);
int kobox_linux_device_pci_core_active(
	const struct kobox_module_context *context);
int kobox_linux_device_pci_bridge_probe(
	const struct kobox_module_context *context);
int kobox_linux_device_pci_bridge_bound(
	const struct kobox_module_context *context);
int kobox_linux_device_pci_bridge_remove(
	const struct kobox_module_context *context);

#endif
