/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_MODULE_LAUNCH_H
#define KOBOX_BOOT_MODULE_LAUNCH_H

#include "lifecycle.h"

/* Borrowed immutable package data, local to one sandbox process. One launch
 * attempt per boot; restart creates a fresh process and resource generation.
 */
struct kobox_linux_native_module {
	const void *image;
	size_t length;
	const char *name;
	const char *parameters;
};

struct kobox_linux_module_launch {
	size_t size;
	const struct kobox_linux_native_module *modules;
	size_t count;
	const struct kobox_linux_lifecycle *lifecycle;
};

struct kobox_linux_module_launch_report {
	size_t size;
	size_t loaded;
	size_t unloaded;
	int result;
	int cleanup_result;
};

#ifdef __KERNEL__
int kobox_linux_modules_run(const struct kobox_linux_module_launch *launch,
			   struct kobox_linux_module_launch_report *report);
#endif

#endif
