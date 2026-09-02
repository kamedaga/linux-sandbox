/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_HOST_H
#define KOBOX_HOST_H

#include <stddef.h>
#include <stdint.h>

struct kobox_host_event;
struct kobox_host_mutex;
struct kobox_host_thread;

typedef int (*kobox_host_thread_fn)(void *argument);

void *kobox_host_allocate(size_t size);
void kobox_host_deallocate(void *pointer, size_t size);

int kobox_host_mutex_create(struct kobox_host_mutex **mutex_out);
void kobox_host_mutex_destroy(struct kobox_host_mutex *mutex);
int kobox_host_mutex_lock(struct kobox_host_mutex *mutex);
int kobox_host_mutex_unlock(struct kobox_host_mutex *mutex);

int kobox_host_event_create(struct kobox_host_event **event_out, int signaled);
void kobox_host_event_destroy(struct kobox_host_event *event);
int kobox_host_event_wait(struct kobox_host_event *event);
int kobox_host_event_signal(struct kobox_host_event *event);
int kobox_host_event_reset(struct kobox_host_event *event);

int kobox_host_thread_create(struct kobox_host_thread **thread_out,
			     kobox_host_thread_fn function, void *argument,
			     uint32_t logical_cpu);
int kobox_host_thread_join(struct kobox_host_thread *thread, int *result_out);
uint32_t kobox_host_current_cpu(void);

int kobox_host_monotonic_time_ns(uint64_t *time_out);

#endif
