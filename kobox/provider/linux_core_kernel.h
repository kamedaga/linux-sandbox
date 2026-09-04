/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_LINUX_CORE_KERNEL_H
#define KOBOX_LINUX_CORE_KERNEL_H

#include "../runtime/module_context.h"

int kobox_linux_core_kernel_init(
	const struct kobox_module_context *context);
int kobox_linux_core_kernel_active(
	const struct kobox_module_context *context);

#endif
