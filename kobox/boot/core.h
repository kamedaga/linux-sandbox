/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_CORE_H
#define KOBOX_BOOT_CORE_H

#include "host.h"
#include "../runtime/host.h"
#include <stdbool.h>

enum kobox_boot_core_result {
	KOBOX_BOOT_CORE_OK,
	KOBOX_BOOT_CORE_INVALID,
	KOBOX_BOOT_CORE_MISSING_ENTRY,
	KOBOX_BOOT_CORE_BIND_FAILURE,
	KOBOX_BOOT_CORE_ALREADY_STARTED,
};

/* The OS backend owns the fixed image mapping and symbol lookup. Per-thread
 * runtime state is supplied explicitly through kobox_runtime_host, so this
 * interface is independent of either libc's TLS or a PachaOS TLS layout.
 * Caller serializes prepare/start; an initialized core cannot be copied or
 * unloaded while its threads are live.
 */
struct kobox_boot_core {
	void *loader;
	void *(*lookup)(void *loader, const char *name);
	int (*start)(const struct kobox_linux_boot_layout *layout,
		     struct kobox_linux_task_report *report);
	kobox_linux_task_notification_fn dispatch;
	bool started;
};

enum kobox_boot_core_result kobox_boot_core_prepare(
	struct kobox_boot_core *core, void *loader,
	void *(*lookup)(void *loader, const char *name),
	const struct kobox_runtime_host *runtime);
enum kobox_boot_core_result kobox_boot_core_start(
	struct kobox_boot_core *core,
	const struct kobox_linux_boot_layout *layout,
	struct kobox_linux_task_report *report, int *kernel_result);

#endif
