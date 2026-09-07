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

struct kobox_posix_task {
	struct kobox_posix_thread thread;
	struct kobox_posix_permit dispatch;
	void *(*entry)(void *);
	void *argument;
	bool current;
	bool initialized;
};

struct kobox_posix_memory_backing {
	int descriptor;
	size_t size;
	bool initialized;
};

struct kobox_posix_memory_window {
	void *address;
	size_t size;
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
	KOBOX_POSIX_NOTIFICATION_CALL_FUNCTION,
	KOBOX_POSIX_NOTIFICATION_VM_EVENT,
	KOBOX_POSIX_NOTIFICATION_COUNT,
};

struct kobox_posix_cpu;

/*
 * Runs on the owning CPU thread, with logical IRQs masked on entry and return.
 * Enabling IRQs inside the callback permits both queued and future nested IRQs.
 * The host restores the interrupted IRQ state after the callback returns.
 */
typedef void (*kobox_posix_notification_fn)(
	void *context,
	uint32_t logical_cpu,
	enum kobox_posix_notification notification,
	uint64_t count);

struct kobox_posix_cpu {
	pthread_mutex_t owner_lock;
	pthread_cond_t execution_condition;
	pthread_t owner;
	struct kobox_posix_task *owner_task;
	struct sigaction previous_action;
	kobox_posix_notification_fn notification;
	void *notification_context;
	atomic_uint irq_disable_depth;
	atomic_uint_fast64_t notification_sequence;
	atomic_uint_fast64_t pending[KOBOX_POSIX_NOTIFICATION_COUNT];
	uint32_t logical_cpu;
	int signal_number;
	bool handoff_released;
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

int kobox_posix_task_bind_current(struct kobox_posix_task **task_out);
int kobox_posix_task_start(
	struct kobox_posix_task **task_out,
	void *(*entry)(void *),
	void *argument);
int kobox_posix_task_wake(struct kobox_posix_task *task);
int kobox_posix_task_park(struct kobox_posix_task *task);
int kobox_posix_task_join_destroy(struct kobox_posix_task *task);
int kobox_posix_task_destroy_current(struct kobox_posix_task *task);
_Noreturn void kobox_posix_task_exit(void);

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
int kobox_posix_realtime_ns(uint64_t *time_out);

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
int kobox_posix_memory_backing_init(
	struct kobox_posix_memory_backing *backing,
	size_t size);
int kobox_posix_memory_backing_destroy(
	struct kobox_posix_memory_backing *backing);
int kobox_posix_memory_window_init(
	struct kobox_posix_memory_window *window,
	size_t size);
int kobox_posix_memory_window_map(
	struct kobox_posix_memory_window *window,
	size_t window_offset,
	struct kobox_posix_memory_backing *backing,
	size_t backing_offset,
	size_t size,
	unsigned int protection,
	void **address_out);
int kobox_posix_memory_window_reset(
	struct kobox_posix_memory_window *window,
	size_t window_offset,
	size_t size);
int kobox_posix_memory_window_destroy(
	struct kobox_posix_memory_window *window);

int kobox_posix_cpu_init(
	struct kobox_posix_cpu *cpu,
	uint32_t logical_cpu,
	kobox_posix_notification_fn notification,
	void *context);
int kobox_posix_cpu_destroy(struct kobox_posix_cpu *cpu);
int kobox_posix_cpu_enter(struct kobox_posix_cpu *cpu);
int kobox_posix_current_cpu(uint32_t *cpu_out);
int kobox_posix_cpu_enter_task(
	struct kobox_posix_cpu *cpu,
	struct kobox_posix_task *task);
int kobox_posix_cpu_leave(struct kobox_posix_cpu *cpu);
int kobox_posix_cpu_switch(
	struct kobox_posix_cpu *cpu,
	struct kobox_posix_task *previous,
	struct kobox_posix_task *next,
	bool exiting);
int kobox_posix_cpu_wait(
	struct kobox_posix_cpu *cpu,
	uint64_t observed_sequence,
	uint64_t *sequence_out);
int kobox_posix_cpu_irq_disable(struct kobox_posix_cpu *cpu);
int kobox_posix_cpu_irq_enable(struct kobox_posix_cpu *cpu);
int kobox_posix_notifications_save(uint64_t *mask_out);
int kobox_posix_notifications_restore(uint64_t mask);
int kobox_posix_cpu_notify(
	struct kobox_posix_cpu *cpu,
	enum kobox_posix_notification notification);
uint64_t kobox_posix_cpu_pending(
	const struct kobox_posix_cpu *cpu,
	enum kobox_posix_notification notification);
bool kobox_posix_cpu_irq_disabled(const struct kobox_posix_cpu *cpu);
uint64_t kobox_posix_cpu_notification_sequence(
	const struct kobox_posix_cpu *cpu);

#endif
