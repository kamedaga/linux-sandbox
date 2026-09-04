/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_HOST_H
#define KOBOX_POSIX_HOST_H

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#define KOBOX_POSIX_MAX_LOGICAL_CPUS 2U

struct kobox_posix_thread {
	pthread_t native;
	void *(*entry)(void *);
	void *argument;
	bool started;
};

struct kobox_posix_permit {
	pthread_mutex_t lock;
	pthread_cond_t condition;
	uint64_t count;
	bool initialized;
};

typedef void (*kobox_posix_timer_fn)(void *context);

struct kobox_posix_oneshot_timer {
	pthread_mutex_t lock;
	pthread_cond_t condition;
	struct kobox_posix_thread thread;
	kobox_posix_timer_fn function;
	void *context;
	uint64_t deadline_ns;
	bool armed;
	bool stopping;
	bool initialized;
};

enum kobox_posix_memory_protection {
	KOBOX_POSIX_MEMORY_READ = 1U << 0,
	KOBOX_POSIX_MEMORY_WRITE = 1U << 1,
	KOBOX_POSIX_MEMORY_EXECUTE = 1U << 2,
};

enum kobox_posix_notification {
	KOBOX_POSIX_NOTIFICATION_TICK = 0,
	KOBOX_POSIX_NOTIFICATION_IRQ,
	KOBOX_POSIX_NOTIFICATION_COUNT,
};

struct kobox_posix_cpu;

typedef void (*kobox_posix_notification_fn)(
	void *context,
	uint32_t logical_cpu,
	enum kobox_posix_notification notification,
	uint64_t count);

struct kobox_posix_cpu {
	pthread_mutex_t execution_lock;
	pthread_mutex_t owner_lock;
	pthread_t owner;
	struct sigaction previous_action;
	kobox_posix_notification_fn notification;
	void *notification_context;
	atomic_uint irq_disable_depth;
	atomic_uint_fast64_t pending[KOBOX_POSIX_NOTIFICATION_COUNT];
	uint32_t logical_cpu;
	int signal_number;
	bool owner_valid;
	bool accepting_notifications;
	bool initialized;
};

int kobox_posix_thread_start(
	struct kobox_posix_thread *thread,
	void *(*entry)(void *),
	void *argument);
int kobox_posix_thread_join(
	struct kobox_posix_thread *thread,
	void **result_out);

int kobox_posix_permit_init(
	struct kobox_posix_permit *permit,
	uint64_t initial_count);
int kobox_posix_permit_post(
	struct kobox_posix_permit *permit,
	uint64_t count);
int kobox_posix_permit_wait(
	struct kobox_posix_permit *permit,
	uint64_t monotonic_deadline_ns);
int kobox_posix_permit_destroy(struct kobox_posix_permit *permit);

int kobox_posix_monotonic_ns(uint64_t *time_out);

int kobox_posix_oneshot_timer_init(
	struct kobox_posix_oneshot_timer *timer,
	kobox_posix_timer_fn function,
	void *context);
int kobox_posix_oneshot_timer_arm(
	struct kobox_posix_oneshot_timer *timer,
	uint64_t monotonic_deadline_ns);
int kobox_posix_oneshot_timer_cancel(
	struct kobox_posix_oneshot_timer *timer);
int kobox_posix_oneshot_timer_destroy(
	struct kobox_posix_oneshot_timer *timer);

int kobox_posix_memory_map(
	size_t size,
	unsigned int protection,
	void **address_out);
int kobox_posix_memory_protect(
	void *address,
	size_t size,
	unsigned int protection);
int kobox_posix_memory_unmap(void *address, size_t size);

int kobox_posix_cpu_init(
	struct kobox_posix_cpu *cpu,
	uint32_t logical_cpu,
	kobox_posix_notification_fn notification,
	void *context);
int kobox_posix_cpu_destroy(struct kobox_posix_cpu *cpu);
int kobox_posix_cpu_enter(struct kobox_posix_cpu *cpu);
int kobox_posix_cpu_leave(struct kobox_posix_cpu *cpu);
int kobox_posix_cpu_irq_disable(struct kobox_posix_cpu *cpu);
int kobox_posix_cpu_irq_enable(struct kobox_posix_cpu *cpu);
int kobox_posix_cpu_notify(
	struct kobox_posix_cpu *cpu,
	enum kobox_posix_notification notification);
uint64_t kobox_posix_cpu_pending(
	const struct kobox_posix_cpu *cpu,
	enum kobox_posix_notification notification);

#endif
