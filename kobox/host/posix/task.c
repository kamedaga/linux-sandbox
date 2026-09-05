/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>
#include <stdlib.h>

static void *task_entry(void *argument)
{
	struct kobox_posix_task *task = argument;

	/* The creator publishes the native handle before granting this permit. */
	if (kobox_posix_permit_wait(&task->dispatch, 0))
		return (void *)(uintptr_t)EIO;
	return task->entry(task->argument);
}

static int allocate_task(struct kobox_posix_task **task_out)
{
	struct kobox_posix_task *task;
	int status;

	if (!task_out)
		return EINVAL;
	task = calloc(1, sizeof(*task));
	if (!task)
		return ENOMEM;
	status = kobox_posix_permit_init(&task->dispatch, 0);
	if (status) {
		free(task);
		return status;
	}
	task->initialized = true;
	*task_out = task;
	return 0;
}

int kobox_posix_task_bind_current(struct kobox_posix_task **task_out)
{
	struct kobox_posix_task *task;
	int status;

	if (!task_out)
		return EINVAL;
	status = allocate_task(&task);
	if (status)
		return status;
	task->thread.native = pthread_self();
	task->thread.started = true;
	task->current = true;
	*task_out = task;
	return 0;
}

int kobox_posix_task_start(
	struct kobox_posix_task **task_out,
	void *(*entry)(void *),
	void *argument)
{
	struct kobox_posix_task *task;
	int status;

	if (!task_out || !entry)
		return EINVAL;
	status = allocate_task(&task);
	if (status)
		return status;
	task->entry = entry;
	task->argument = argument;
	status = kobox_posix_thread_start(&task->thread, task_entry, task);
	if (status) {
		(void)kobox_posix_permit_destroy(&task->dispatch);
		free(task);
		return status;
	}
	*task_out = task;
	return 0;
}

int kobox_posix_task_wake(struct kobox_posix_task *task)
{
	if (!task || !task->initialized)
		return EINVAL;
	return kobox_posix_permit_post(&task->dispatch, 1);
}

int kobox_posix_task_park(struct kobox_posix_task *task)
{
	if (!task || !task->initialized ||
	    !pthread_equal(task->thread.native, pthread_self()))
		return EINVAL;
	return kobox_posix_permit_wait(&task->dispatch, 0);
}

int kobox_posix_task_join_destroy(struct kobox_posix_task *task)
{
	void *result = NULL;
	int status;

	if (!task || !task->initialized || task->current)
		return EINVAL;
	status = kobox_posix_thread_join(&task->thread, &result);
	if (!status && result)
		status = (int)(uintptr_t)result;
	if (status)
		return status;
	status = kobox_posix_permit_destroy(&task->dispatch);
	if (status)
		return status;
	task->initialized = false;
	free(task);
	return 0;
}

int kobox_posix_task_destroy_current(struct kobox_posix_task *task)
{
	int status;

	if (!task || !task->initialized || !task->current ||
	    !pthread_equal(task->thread.native, pthread_self()))
		return EINVAL;
	status = kobox_posix_permit_destroy(&task->dispatch);
	if (status)
		return status;
	task->initialized = false;
	free(task);
	return 0;
}

_Noreturn void kobox_posix_task_exit(void)
{
	pthread_exit(NULL);
}
