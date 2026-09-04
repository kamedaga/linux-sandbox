/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>

static void *kobox_posix_thread_entry(void *argument)
{
	struct kobox_posix_thread *thread = argument;

	return thread->entry(thread->argument);
}

int kobox_posix_thread_start(
	struct kobox_posix_thread *thread,
	void *(*entry)(void *),
	void *argument)
{
	int result;

	if (!thread || !entry || thread->started)
		return EINVAL;
	thread->entry = entry;
	thread->argument = argument;
	result = pthread_create(
		&thread->native, NULL, kobox_posix_thread_entry, thread);
	if (result)
		return result;
	thread->started = true;
	return 0;
}

int kobox_posix_thread_join(
	struct kobox_posix_thread *thread,
	void **result_out)
{
	void *result;
	int status;

	if (!thread || !thread->started)
		return EINVAL;
	status = pthread_join(thread->native, &result);
	if (status)
		return status;
	thread->started = false;
	if (result_out)
		*result_out = result;
	return 0;
}
