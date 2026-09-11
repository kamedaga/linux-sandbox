// SPDX-License-Identifier: GPL-2.0-only

#include "bootstrap.h"
#include "exception.h"
#include "../../task/posix_machine.h"

#include <errno.h>

int kobox_posix_bootstrap_prepare(struct kobox_posix_bootstrap *bootstrap,
	struct kobox_posix_core *core, struct kobox_posix_memory_backing *backing,
	const struct kobox_posix_boot_profile *profile)
{
	void *direct_address;
	int status;

	if (!bootstrap || bootstrap->attempted || !core || !backing ||
	    !backing->initialized || !profile ||
	    backing->size != profile->ram_size)
		return EINVAL;
	bootstrap->attempted = true;
	bootstrap->core = core;
	status = kobox_posix_memory_window_init(&bootstrap->direct,
						profile->ram_size);
	if (status)
		return status;
	status = kobox_posix_memory_window_init(&bootstrap->vmemmap,
						profile->vmemmap_size);
	if (status)
		return status;
	status = kobox_posix_memory_window_init(&bootstrap->vmalloc,
						profile->vmalloc_size);
	if (status)
		return status;
	status = kobox_posix_memory_window_map(&bootstrap->direct, 0, backing, 0,
		profile->ram_size, KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE,
		&direct_address);
	if (status)
		return status;
	status = kobox_boot_image_alias(kobox_posix_core_library(core), backing,
		direct_address, profile->image_physical_base, &bootstrap->image);
	if (status)
		return status;
	bootstrap->layout = (struct kobox_linux_boot_layout) {
		.size = sizeof(bootstrap->layout),
		.exceptions_install = kobox_posix_exceptions_install,
		.image_protect = kobox_boot_image_protect,
		.image = &bootstrap->image,
		.task = {
			.size = sizeof(bootstrap->layout.task),
			.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
			.operations = &kobox_task_posix_operations,
			.memory = {
				.size = sizeof(bootstrap->layout.task.memory),
				.identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
				.operations = &kobox_task_posix_memory_operations,
				.ram_backing = backing,
				.direct_window = &bootstrap->direct,
				.vmemmap_window = &bootstrap->vmemmap,
				.vmalloc_window = &bootstrap->vmalloc,
				.direct_map = direct_address,
				.ram_size = profile->ram_size,
				.vmemmap_base = bootstrap->vmemmap.address,
				.vmemmap_size = bootstrap->vmemmap.size,
				.vmalloc_base = bootstrap->vmalloc.address,
				.vmalloc_size = bootstrap->vmalloc.size,
				.kernel_image_physical_base = profile->image_physical_base,
			},
		},
	};
	bootstrap->prepared = true;
	return 0;
}

int kobox_posix_bootstrap_start(struct kobox_posix_bootstrap *bootstrap,
	struct kobox_linux_task_report *report, int *kernel_result)
{
	struct kobox_boot_core *core;
	int status;

	if (!bootstrap || !bootstrap->prepared || bootstrap->entered ||
	    !report || !kernel_result || !bootstrap->layout.kernel_main ||
	    !bootstrap->layout.console_write)
		return EINVAL;
	bootstrap->entered = true;
	core = kobox_posix_core_boot(bootstrap->core);
	status = kobox_task_posix_init(core->dispatch);
	if (!status)
		status = kobox_posix_task_bind_current(&bootstrap->boot_task);
	if (!status)
		status = kobox_task_posix_operations.cpu_enter(0, bootstrap->boot_task);
	if (!status)
		status = kobox_task_posix_operations.cpu_irq_disable(0);
	if (status)
		return status;
	bootstrap->layout.task.boot_task = bootstrap->boot_task;
	return kobox_boot_core_start(core, &bootstrap->layout, report,
		kernel_result) == KOBOX_BOOT_CORE_OK ? 0 : EIO;
}
