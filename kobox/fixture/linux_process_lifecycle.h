/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_PROCESS_LIFECYCLE_H
#define KOBOX_LINUX_PROCESS_LIFECYCLE_H

#include <stdbool.h>
#include <stdatomic.h>

enum kobox_linux_process_state {
	KOBOX_LINUX_PROCESS_NEW = 0,
	KOBOX_LINUX_PROCESS_BOOTING,
	KOBOX_LINUX_PROCESS_RUNNING,
	KOBOX_LINUX_PROCESS_QUIESCING,
	KOBOX_LINUX_PROCESS_EXITING,
};

struct kobox_linux_process_lifecycle {
	atomic_int state;
};

typedef void (*kobox_linux_quiesce_fn)(void *context);

void kobox_linux_process_lifecycle_init(
	struct kobox_linux_process_lifecycle *lifecycle);
bool kobox_linux_process_begin_boot(
	struct kobox_linux_process_lifecycle *lifecycle);
bool kobox_linux_process_mark_running(
	struct kobox_linux_process_lifecycle *lifecycle);
_Noreturn void kobox_linux_process_shutdown(
	struct kobox_linux_process_lifecycle *lifecycle,
	kobox_linux_quiesce_fn quiesce_modules,
	void *context,
	int exit_status);

#endif
