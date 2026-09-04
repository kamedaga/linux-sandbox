/* SPDX-License-Identifier: GPL-2.0-only */
#include "linux_process_lifecycle.h"

#include <sched.h>
#include <stdlib.h>

void kobox_linux_process_lifecycle_init(
	struct kobox_linux_process_lifecycle *lifecycle)
{
	atomic_init(&lifecycle->state, KOBOX_LINUX_PROCESS_NEW);
}

bool kobox_linux_process_begin_boot(
	struct kobox_linux_process_lifecycle *lifecycle)
{
	int expected = KOBOX_LINUX_PROCESS_NEW;

	return atomic_compare_exchange_strong_explicit(
		&lifecycle->state, &expected, KOBOX_LINUX_PROCESS_BOOTING,
		memory_order_acq_rel, memory_order_acquire);
}

bool kobox_linux_process_mark_running(
	struct kobox_linux_process_lifecycle *lifecycle)
{
	int expected = KOBOX_LINUX_PROCESS_BOOTING;

	return atomic_compare_exchange_strong_explicit(
		&lifecycle->state, &expected, KOBOX_LINUX_PROCESS_RUNNING,
		memory_order_acq_rel, memory_order_acquire);
}

_Noreturn void kobox_linux_process_shutdown(
	struct kobox_linux_process_lifecycle *lifecycle,
	kobox_linux_quiesce_fn quiesce_modules,
	void *context,
	int exit_status)
{
	int expected = KOBOX_LINUX_PROCESS_RUNNING;

	if (atomic_compare_exchange_strong_explicit(
		    &lifecycle->state, &expected, KOBOX_LINUX_PROCESS_QUIESCING,
		    memory_order_acq_rel, memory_order_acquire)) {
		quiesce_modules(context);
		atomic_store_explicit(&lifecycle->state, KOBOX_LINUX_PROCESS_EXITING,
				      memory_order_release);
		_Exit(exit_status);
	}

	/* The thread which owns QUIESCING terminates the entire process. */
	for (;;)
		sched_yield();
}
