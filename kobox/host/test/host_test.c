// SPDX-License-Identifier: GPL-2.0-only

#define _POSIX_C_SOURCE 200809L

#include "../host.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct kobox_host_mutex {
	pthread_mutex_t native;
};

struct kobox_host_event {
	pthread_mutex_t lock;
	pthread_cond_t condition;
	int signaled;
};

struct kobox_host_thread {
	pthread_t native;
	kobox_host_thread_fn function;
	void *argument;
	uint32_t logical_cpu;
	int result;
};

static _Thread_local uint32_t kobox_host_logical_cpu = UINT32_MAX;

void *kobox_host_allocate(size_t size)
{
	if (!size)
		return NULL;
	return calloc(1, size);
}

void kobox_host_deallocate(void *pointer, size_t size)
{
	(void)size;
	free(pointer);
}

int kobox_host_mutex_create(struct kobox_host_mutex **mutex_out)
{
	struct kobox_host_mutex *mutex;

	if (!mutex_out)
		return -1;
	*mutex_out = NULL;
	mutex = malloc(sizeof(*mutex));
	if (!mutex)
		return -1;
	if (pthread_mutex_init(&mutex->native, NULL)) {
		free(mutex);
		return -1;
	}
	*mutex_out = mutex;
	return 0;
}

void kobox_host_mutex_destroy(struct kobox_host_mutex *mutex)
{
	if (!mutex)
		return;
	pthread_mutex_destroy(&mutex->native);
	free(mutex);
}

int kobox_host_mutex_lock(struct kobox_host_mutex *mutex)
{
	return mutex && !pthread_mutex_lock(&mutex->native) ? 0 : -1;
}

int kobox_host_mutex_unlock(struct kobox_host_mutex *mutex)
{
	return mutex && !pthread_mutex_unlock(&mutex->native) ? 0 : -1;
}

int kobox_host_event_create(struct kobox_host_event **event_out, int signaled)
{
	struct kobox_host_event *event;

	if (!event_out || (signaled != 0 && signaled != 1))
		return -1;
	*event_out = NULL;
	event = malloc(sizeof(*event));
	if (!event)
		return -1;
	if (pthread_mutex_init(&event->lock, NULL)) {
		free(event);
		return -1;
	}
	if (pthread_cond_init(&event->condition, NULL)) {
		pthread_mutex_destroy(&event->lock);
		free(event);
		return -1;
	}
	event->signaled = signaled;
	*event_out = event;
	return 0;
}

void kobox_host_event_destroy(struct kobox_host_event *event)
{
	if (!event)
		return;
	pthread_cond_destroy(&event->condition);
	pthread_mutex_destroy(&event->lock);
	free(event);
}

int kobox_host_event_wait(struct kobox_host_event *event)
{
	int status = 0;

	if (!event || pthread_mutex_lock(&event->lock))
		return -1;
	while (!event->signaled && !status)
		status = pthread_cond_wait(&event->condition, &event->lock);
	if (pthread_mutex_unlock(&event->lock))
		return -1;
	return status ? -1 : 0;
}

int kobox_host_event_signal(struct kobox_host_event *event)
{
	int status;

	if (!event || pthread_mutex_lock(&event->lock))
		return -1;
	event->signaled = 1;
	status = pthread_cond_broadcast(&event->condition);
	if (pthread_mutex_unlock(&event->lock))
		return -1;
	return status ? -1 : 0;
}

int kobox_host_event_reset(struct kobox_host_event *event)
{
	if (!event || pthread_mutex_lock(&event->lock))
		return -1;
	event->signaled = 0;
	return pthread_mutex_unlock(&event->lock) ? -1 : 0;
}

static void *kobox_host_thread_entry(void *argument)
{
	struct kobox_host_thread *thread = argument;

	kobox_host_logical_cpu = thread->logical_cpu;
	thread->result = thread->function(thread->argument);
	return NULL;
}

int kobox_host_thread_create(struct kobox_host_thread **thread_out,
			     kobox_host_thread_fn function, void *argument,
			     uint32_t logical_cpu)
{
	struct kobox_host_thread *thread;

	if (!thread_out || !function || logical_cpu == UINT32_MAX)
		return -1;
	*thread_out = NULL;
	thread = calloc(1, sizeof(*thread));
	if (!thread)
		return -1;
	thread->function = function;
	thread->argument = argument;
	thread->logical_cpu = logical_cpu;
	if (pthread_create(&thread->native, NULL, kobox_host_thread_entry, thread)) {
		free(thread);
		return -1;
	}
	*thread_out = thread;
	return 0;
}

int kobox_host_thread_join(struct kobox_host_thread *thread, int *result_out)
{
	if (!thread || !result_out || pthread_join(thread->native, NULL))
		return -1;
	*result_out = thread->result;
	free(thread);
	return 0;
}

uint32_t kobox_host_current_cpu(void)
{
	return kobox_host_logical_cpu;
}

int kobox_host_monotonic_time_ns(uint64_t *time_out)
{
	struct timespec now;
	uint64_t seconds;

	if (!time_out || clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0 ||
	    (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000))
		return -1;
	seconds = (uint64_t)now.tv_sec * UINT64_C(1000000000);
	*time_out = seconds + (uint64_t)now.tv_nsec;
	return 0;
}
