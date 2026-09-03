/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_PROVIDER_CORE_LIFECYCLE_H
#define KOBOX_PROVIDER_CORE_LIFECYCLE_H

#include "../runtime/module_context.h"

#include <kobox2/core_runtime.h>

#define KOBOX_LINUX_CORE_MEMORY_SLOT_ID 1u

extern const struct kb2_core_directory kobox_linux_core_directory;

int kobox_linux_core_init(const struct kobox_module_context *context);
int kobox_linux_core_quiesce(const struct kobox_module_context *context);
int kobox_linux_core_cleanup(const struct kobox_module_context *context);

#endif
