// SPDX-License-Identifier: GPL-2.0-only
#include "core.h"

#include <string.h>

enum kobox_boot_core_result kobox_boot_core_prepare(
	struct kobox_boot_core *core, void *loader,
	void *(*lookup)(void *loader, const char *name),
	const struct kobox_runtime_host *runtime)
{
	struct kobox_boot_core prepared = {.loader = loader, .lookup = lookup};
	int (*bind)(const struct kobox_runtime_host *host);
	void *address;

	if (!core || core->start || !lookup || !runtime)
		return KOBOX_BOOT_CORE_INVALID;
	address = lookup(loader, "kobox_linux_boot_start");
	if (!address)
		return KOBOX_BOOT_CORE_MISSING_ENTRY;
	memcpy(&prepared.start, &address, sizeof(prepared.start));
	address = lookup(loader, "kobox_linux_task_dispatch");
	if (!address)
		return KOBOX_BOOT_CORE_MISSING_ENTRY;
	memcpy(&prepared.dispatch, &address, sizeof(prepared.dispatch));
	address = lookup(loader, "kobox_linux_runtime_bind");
	if (!address)
		return KOBOX_BOOT_CORE_MISSING_ENTRY;
	memcpy(&bind, &address, sizeof(bind));
	if (bind(runtime))
		return KOBOX_BOOT_CORE_BIND_FAILURE;
	*core = prepared;
	return KOBOX_BOOT_CORE_OK;
}

enum kobox_boot_core_result kobox_boot_core_start(
	struct kobox_boot_core *core,
	const struct kobox_linux_boot_layout *layout,
	struct kobox_linux_task_report *report, int *kernel_result)
{
	if (!core || !core->start || !layout || !report || !kernel_result)
		return KOBOX_BOOT_CORE_INVALID;
	if (core->started)
		return KOBOX_BOOT_CORE_ALREADY_STARTED;
	core->started = true;
	/* Initialization order and services belong to upstream start_kernel(). */
	*kernel_result = core->start(layout, report);
	return KOBOX_BOOT_CORE_OK;
}
