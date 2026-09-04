/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>
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

static void *timer_thread(void *argument)
{
	struct kobox_posix_oneshot_timer *timer = argument;

	if (pthread_mutex_lock(&timer->lock))
		return (void *)(uintptr_t)EIO;
	while (!timer->stopping) {
		struct timespec deadline;
		int status;

		while (!timer->armed && !timer->stopping) {
			status = pthread_cond_wait(&timer->condition, &timer->lock);
			if (status) {
				pthread_mutex_unlock(&timer->lock);
				return (void *)(uintptr_t)status;
			}
		}
		if (timer->stopping)
			break;
		status = deadline_timespec(timer->deadline_ns, &deadline);
		if (status) {
			pthread_mutex_unlock(&timer->lock);
			return (void *)(uintptr_t)status;
		}
		status = pthread_cond_timedwait(
			&timer->condition, &timer->lock, &deadline);
		if (status == ETIMEDOUT && timer->armed) {
			kobox_posix_timer_fn function = timer->function;
			void *context = timer->context;

			timer->armed = false;
			pthread_mutex_unlock(&timer->lock);
			function(context);
			if (pthread_mutex_lock(&timer->lock))
				return (void *)(uintptr_t)EIO;
		} else if (status) {
			pthread_mutex_unlock(&timer->lock);
			return (void *)(uintptr_t)status;
		}
	}
	pthread_mutex_unlock(&timer->lock);
	return NULL;
}

int kobox_posix_oneshot_timer_init(
	struct kobox_posix_oneshot_timer *timer,
	kobox_posix_timer_fn function,
	void *context)
{
	pthread_condattr_t attributes;
	int status;

	if (!timer || !function || timer->initialized)
		return EINVAL;
	status = pthread_mutex_init(&timer->lock, NULL);
	if (status)
		return status;
	status = pthread_condattr_init(&attributes);
	if (status) {
		pthread_mutex_destroy(&timer->lock);
		return status;
	}
	status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (!status)
		status = pthread_cond_init(&timer->condition, &attributes);
	pthread_condattr_destroy(&attributes);
	if (status) {
		pthread_mutex_destroy(&timer->lock);
		return status;
	}
	timer->function = function;
	timer->context = context;
	timer->deadline_ns = 0;
	timer->armed = false;
	timer->stopping = false;
	status = kobox_posix_thread_start(&timer->thread, timer_thread, timer);
	if (status) {
		pthread_cond_destroy(&timer->condition);
		pthread_mutex_destroy(&timer->lock);
		return status;
	}
	timer->initialized = true;
	return 0;
}

int kobox_posix_oneshot_timer_arm(
	struct kobox_posix_oneshot_timer *timer,
	uint64_t monotonic_deadline_ns)
{
	int status;
	int signal_status;

	if (!timer || !timer->initialized || !monotonic_deadline_ns)
		return EINVAL;
	status = pthread_mutex_lock(&timer->lock);
	if (status)
		return status;
	if (timer->stopping) {
		status = ECANCELED;
	} else {
		timer->deadline_ns = monotonic_deadline_ns;
		timer->armed = true;
		status = 0;
	}
	signal_status = pthread_cond_signal(&timer->condition);
	if (pthread_mutex_unlock(&timer->lock) && !status)
		status = EINVAL;
	return status ? status : signal_status;
}

int kobox_posix_oneshot_timer_cancel(
	struct kobox_posix_oneshot_timer *timer)
{
	int status;
	int signal_status;

	if (!timer || !timer->initialized)
		return EINVAL;
	status = pthread_mutex_lock(&timer->lock);
	if (status)
		return status;
	timer->armed = false;
	signal_status = pthread_cond_signal(&timer->condition);
	if (pthread_mutex_unlock(&timer->lock))
		return EINVAL;
	return signal_status;
}

int kobox_posix_oneshot_timer_destroy(
	struct kobox_posix_oneshot_timer *timer)
{
	void *thread_result = NULL;
	int status;

	if (!timer || !timer->initialized)
		return EINVAL;
	status = pthread_mutex_lock(&timer->lock);
	if (status)
		return status;
	timer->armed = false;
	timer->stopping = true;
	pthread_cond_signal(&timer->condition);
	if (pthread_mutex_unlock(&timer->lock))
		return EINVAL;
	status = kobox_posix_thread_join(&timer->thread, &thread_result);
	if (!status && thread_result)
		status = (int)(uintptr_t)thread_result;
	if (status)
		return status;
	status = pthread_cond_destroy(&timer->condition);
	if (status)
		return status;
	status = pthread_mutex_destroy(&timer->lock);
	if (status)
		return status;
	timer->initialized = false;
	return 0;
}
