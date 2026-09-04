/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>
#include <limits.h>
#include <time.h>

static int deadline_timespec(uint64_t deadline_ns, struct timespec *time)
{
	uint64_t seconds = deadline_ns / UINT64_C(1000000000);
	time_t native_seconds = (time_t)seconds;

	if (native_seconds < 0 || (uint64_t)native_seconds != seconds)
		return EOVERFLOW;
	time->tv_sec = native_seconds;
	time->tv_nsec = (long)(deadline_ns % UINT64_C(1000000000));
	return 0;
}

int kobox_posix_permit_init(
	struct kobox_posix_permit *permit,
	uint64_t initial_count)
{
	pthread_condattr_t attributes;
	int status;

	if (!permit || permit->initialized)
		return EINVAL;
	status = pthread_mutex_init(&permit->lock, NULL);
	if (status)
		return status;
	status = pthread_condattr_init(&attributes);
	if (status) {
		pthread_mutex_destroy(&permit->lock);
		return status;
	}
	status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (!status)
		status = pthread_cond_init(&permit->condition, &attributes);
	pthread_condattr_destroy(&attributes);
	if (status) {
		pthread_mutex_destroy(&permit->lock);
		return status;
	}
	permit->count = initial_count;
	permit->initialized = true;
	return 0;
}

int kobox_posix_permit_post(
	struct kobox_posix_permit *permit,
	uint64_t count)
{
	int status;
	int wake_status = 0;

	if (!permit || !permit->initialized || !count)
		return EINVAL;
	status = pthread_mutex_lock(&permit->lock);
	if (status)
		return status;
	if (UINT64_MAX - permit->count < count) {
		status = EOVERFLOW;
	} else {
		permit->count += count;
		wake_status = count == 1 ?
			pthread_cond_signal(&permit->condition) :
			pthread_cond_broadcast(&permit->condition);
	}
	if (pthread_mutex_unlock(&permit->lock) && !status)
		status = EINVAL;
	return status ? status : wake_status;
}

int kobox_posix_permit_wait(
	struct kobox_posix_permit *permit,
	uint64_t monotonic_deadline_ns)
{
	struct timespec deadline;
	int status;

	if (!permit || !permit->initialized)
		return EINVAL;
	if (monotonic_deadline_ns) {
		status = deadline_timespec(monotonic_deadline_ns, &deadline);
		if (status)
			return status;
	}
	status = pthread_mutex_lock(&permit->lock);
	if (status)
		return status;
	while (!permit->count && !status) {
		if (monotonic_deadline_ns)
			status = pthread_cond_timedwait(
				&permit->condition, &permit->lock, &deadline);
		else
			status = pthread_cond_wait(
				&permit->condition, &permit->lock);
	}
	if (!status)
		permit->count--;
	if (pthread_mutex_unlock(&permit->lock) && !status)
		status = EINVAL;
	return status;
}

int kobox_posix_permit_destroy(struct kobox_posix_permit *permit)
{
	int status;

	if (!permit || !permit->initialized)
		return EINVAL;
	status = pthread_cond_destroy(&permit->condition);
	if (status)
		return status;
	status = pthread_mutex_destroy(&permit->lock);
	if (status)
		return status;
	permit->initialized = false;
	return 0;
}
