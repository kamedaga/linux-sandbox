/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_BOOTSTRAP_H
#define KOBOX_POSIX_BOOTSTRAP_H

#include "core.h"
#include "image.h"

struct kobox_posix_boot_profile {
	size_t ram_size;
	size_t vmemmap_size;
	size_t vmalloc_size;
	size_t image_physical_base;
};

/* Caller-owned, zero-initialized, single-use native process bootstrap.
 * The core and backing are borrowed and must outlive all CPUs and mappings.
 * The caller sets layout's resource, console and PID 1 entry fields after
 * prepare. Workload dispatch and resource authority are not selected here.
 * After any failure, do not enter/retry this core. Once start is attempted,
 * the process owner must terminate/reap it; no live core unload is implied.
 */
struct kobox_posix_bootstrap {
	struct kobox_posix_memory_window direct, vmemmap, vmalloc;
	struct kobox_boot_image image;
	struct kobox_linux_boot_layout layout;
	struct kobox_posix_core *core;
	struct kobox_posix_task *boot_task;
	bool attempted, prepared, entered;
};

int kobox_posix_bootstrap_prepare(struct kobox_posix_bootstrap *bootstrap,
	struct kobox_posix_core *core, struct kobox_posix_memory_backing *backing,
	const struct kobox_posix_boot_profile *profile);
int kobox_posix_bootstrap_start(struct kobox_posix_bootstrap *bootstrap,
	struct kobox_linux_task_report *report, int *kernel_result);

#endif
