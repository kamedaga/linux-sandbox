/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_TIMEOUT_NS UINT64_C(2000000000)
#define SHORT_DELAY_NS UINT64_C(20000000)

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "check failed at line %d: %s\n", \
			__LINE__, #expression); \
		return 1; \
	} \
} while (0)

struct notification_state {
	atomic_uint_fast64_t count[KOBOX_POSIX_NOTIFICATION_COUNT];
	atomic_uint cpu_mismatch;
};

struct same_cpu_case {
	struct kobox_posix_cpu *cpu;
	atomic_uint *active;
	atomic_uint *maximum;
	struct kobox_posix_permit *first_inside;
	struct kobox_posix_permit *release_first;
	struct kobox_posix_permit *second_attempting;
	bool first;
};

struct parallel_case {
	struct kobox_posix_cpu *cpu;
	atomic_uint *entered_mask;
	unsigned int bit;
};

struct tick_case {
	struct kobox_posix_cpu *cpu;
	struct notification_state *notifications;
	struct kobox_posix_permit ready;
	enum kobox_posix_notification notification;
};

struct timer_notify_case {
	struct kobox_posix_cpu *cpu;
	enum kobox_posix_notification notification;
	atomic_uint fires;
};

struct irq_case {
	struct kobox_posix_cpu *cpu;
	struct notification_state *notifications;
	struct kobox_posix_permit ready;
	atomic_bool enable;
};

struct timer_case {
	struct kobox_posix_permit fired;
	atomic_uint fire_count;
};

static int deadline_after(uint64_t interval_ns, uint64_t *deadline_out)
{
	uint64_t now;
	int status = kobox_posix_monotonic_ns(&now);

	if (status)
		return status;
	if (UINT64_MAX - now < interval_ns)
		return EOVERFLOW;
	*deadline_out = now + interval_ns;
	return 0;
}

static void delay_ns(uint64_t interval_ns)
{
	struct timespec duration = {
		.tv_sec = (time_t)(interval_ns / UINT64_C(1000000000)),
		.tv_nsec = (long)(interval_ns % UINT64_C(1000000000)),
	};

	while (nanosleep(&duration, &duration) != 0 && errno == EINTR)
		;
}

static void update_maximum(atomic_uint *maximum, unsigned int value)
{
	unsigned int current = atomic_load_explicit(maximum, memory_order_relaxed);

	while (current < value && !atomic_compare_exchange_weak_explicit(
		maximum, &current, value, memory_order_relaxed,
		memory_order_relaxed))
		;
}

static void notification_callback(
	void *context,
	uint32_t logical_cpu,
	enum kobox_posix_notification notification,
	uint64_t count)
{
	struct notification_state *state = context;

	if (logical_cpu > 1)
		atomic_fetch_add_explicit(
			&state->cpu_mismatch, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(
		&state->count[notification], count, memory_order_release);
}

static void *same_cpu_worker(void *argument)
{
	struct same_cpu_case *test = argument;
	unsigned int active;
	int status;

	status = kobox_posix_cpu_enter(test->cpu);
	if (status)
		return (void *)(uintptr_t)status;
	active = atomic_fetch_add_explicit(
		&*test->active, 1, memory_order_acq_rel) + 1;
	update_maximum(test->maximum, active);
	if (test->first) {
		uint64_t deadline;

		status = kobox_posix_permit_post(test->first_inside, 1);
		if (!status)
			status = deadline_after(TEST_TIMEOUT_NS, &deadline);
		if (!status)
			status = kobox_posix_permit_wait(
				test->release_first, deadline);
	}
	atomic_fetch_sub_explicit(&*test->active, 1, memory_order_release);
	if (kobox_posix_cpu_leave(test->cpu) && !status)
		status = EIO;
	return (void *)(uintptr_t)status;
}

static void *second_same_cpu_worker(void *argument)
{
	struct same_cpu_case *test = argument;
	int status = kobox_posix_permit_post(test->second_attempting, 1);

	if (status)
		return (void *)(uintptr_t)status;
	return same_cpu_worker(argument);
}

static void *parallel_worker(void *argument)
{
	struct parallel_case *test = argument;
	uint64_t deadline;
	uint64_t now;
	int status;

	status = kobox_posix_cpu_enter(test->cpu);
	if (status)
		return (void *)(uintptr_t)status;
	atomic_fetch_or_explicit(
		test->entered_mask, test->bit, memory_order_release);
	status = deadline_after(TEST_TIMEOUT_NS, &deadline);
	while (!status && atomic_load_explicit(
		       test->entered_mask, memory_order_acquire) != 3U) {
		status = kobox_posix_monotonic_ns(&now);
		if (!status && now >= deadline)
			status = ETIMEDOUT;
	}
	if (kobox_posix_cpu_leave(test->cpu) && !status)
		status = EIO;
	return (void *)(uintptr_t)status;
}

static void timer_notify(void *argument)
{
	struct timer_notify_case *test = argument;

	atomic_fetch_add_explicit(&test->fires, 1, memory_order_relaxed);
	(void)kobox_posix_cpu_notify(test->cpu, test->notification);
}

static void *cpu_bound_tick_worker(void *argument)
{
	struct tick_case *test = argument;
	uint64_t deadline;
	uint64_t now = 0;
	uint64_t iterations = 0;
	int status;

	status = kobox_posix_cpu_enter(test->cpu);
	if (status)
		return (void *)(uintptr_t)status;
	status = kobox_posix_permit_post(&test->ready, 1);
	if (!status)
		status = deadline_after(TEST_TIMEOUT_NS, &deadline);
	while (!status && !atomic_load_explicit(
		       &test->notifications->count[test->notification],
		       memory_order_acquire)) {
		iterations++;
		if ((iterations & UINT64_C(0xfffff)) == 0) {
			status = kobox_posix_monotonic_ns(&now);
			if (!status && now >= deadline)
				status = ETIMEDOUT;
		}
	}
	if (!iterations && !status)
		status = EIO;
	if (kobox_posix_cpu_leave(test->cpu) && !status)
		status = EIO;
	return (void *)(uintptr_t)status;
}

static void *irq_disabled_worker(void *argument)
{
	struct irq_case *test = argument;
	uint64_t deadline;
	uint64_t now = 0;
	uint64_t iterations = 0;
	int status;

	status = kobox_posix_cpu_enter(test->cpu);
	if (!status)
		status = kobox_posix_cpu_irq_disable(test->cpu);
	if (!status)
		status = kobox_posix_cpu_irq_disable(test->cpu);
	if (!status)
		status = kobox_posix_permit_post(&test->ready, 1);
	if (!status)
		status = deadline_after(TEST_TIMEOUT_NS, &deadline);
	while (!status && !atomic_load_explicit(
		       &test->enable, memory_order_acquire)) {
		iterations++;
		if ((iterations & UINT64_C(0xfffff)) == 0) {
			status = kobox_posix_monotonic_ns(&now);
			if (!status && now >= deadline)
				status = ETIMEDOUT;
		}
	}
	if (!status)
		status = kobox_posix_cpu_irq_enable(test->cpu);
	if (!status && (atomic_load_explicit(
		    &test->notifications->count[KOBOX_POSIX_NOTIFICATION_IRQ],
		    memory_order_acquire) != 0 ||
		    kobox_posix_cpu_pending(
			    test->cpu, KOBOX_POSIX_NOTIFICATION_IRQ) != 3))
		status = EIO;
	if (!status)
		status = kobox_posix_cpu_irq_enable(test->cpu);
	if (!status && atomic_load_explicit(
		    &test->notifications->count[KOBOX_POSIX_NOTIFICATION_IRQ],
		    memory_order_acquire) != 3)
		status = EIO;
	if (kobox_posix_cpu_leave(test->cpu) && !status)
		status = EIO;
	return (void *)(uintptr_t)status;
}

static void timer_fire(void *argument)
{
	struct timer_case *test = argument;

	atomic_fetch_add_explicit(&test->fire_count, 1, memory_order_relaxed);
	(void)kobox_posix_permit_post(&test->fired, 1);
}

static int join_success(struct kobox_posix_thread *thread)
{
	void *result = NULL;
	int status = kobox_posix_thread_join(thread, &result);

	return status ? status : (int)(uintptr_t)result;
}

static int test_basic_primitives(void)
{
	struct kobox_posix_permit permit = {0};
	struct kobox_posix_oneshot_timer timer = {0};
	struct timer_case timer_test = {0};
	uint64_t before;
	uint64_t after;
	uint64_t deadline;
	long page_size;
	unsigned char *mapping;
	unsigned int index;

	CHECK(kobox_posix_monotonic_ns(&before) == 0);
	delay_ns(UINT64_C(1000000));
	CHECK(kobox_posix_monotonic_ns(&after) == 0 && after > before);

	page_size = sysconf(_SC_PAGESIZE);
	CHECK(page_size > 0);
	CHECK(kobox_posix_memory_map(
		(size_t)page_size * 2,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE,
		(void **)&mapping) == 0);
	CHECK((uintptr_t)mapping % (uintptr_t)page_size == 0);
	mapping[0] = 0x5a;
	mapping[(size_t)page_size * 2 - 1] = 0xa5;
	CHECK(mapping[0] == 0x5a &&
	      mapping[(size_t)page_size * 2 - 1] == 0xa5);
	CHECK(kobox_posix_memory_protect(
		mapping, (size_t)page_size * 2, KOBOX_POSIX_MEMORY_READ) == 0);
	CHECK(kobox_posix_memory_unmap(
		mapping, (size_t)page_size * 2) == 0);

	CHECK(kobox_posix_permit_init(&permit, 0) == 0);
	CHECK(kobox_posix_permit_post(&permit, 1) == 0);
	CHECK(deadline_after(TEST_TIMEOUT_NS, &deadline) == 0);
	CHECK(kobox_posix_permit_wait(&permit, deadline) == 0);
	CHECK(kobox_posix_permit_post(&permit, 128) == 0);
	for (index = 0; index < 128; index++)
		CHECK(kobox_posix_permit_wait(&permit, deadline) == 0);
	CHECK(kobox_posix_permit_destroy(&permit) == 0);

	CHECK(kobox_posix_permit_init(&timer_test.fired, 0) == 0);
	atomic_init(&timer_test.fire_count, 0);
	CHECK(kobox_posix_oneshot_timer_init(
		&timer, timer_fire, &timer_test) == 0);
	CHECK(deadline_after(SHORT_DELAY_NS, &deadline) == 0);
	CHECK(kobox_posix_oneshot_timer_arm(&timer, deadline) == 0);
	CHECK(deadline_after(TEST_TIMEOUT_NS, &deadline) == 0);
	CHECK(kobox_posix_permit_wait(&timer_test.fired, deadline) == 0);
	delay_ns(SHORT_DELAY_NS * 2);
	CHECK(atomic_load_explicit(
		&timer_test.fire_count, memory_order_acquire) == 1);
	CHECK(deadline_after(SHORT_DELAY_NS * 2, &deadline) == 0);
	CHECK(kobox_posix_oneshot_timer_arm(&timer, deadline) == 0);
	CHECK(kobox_posix_oneshot_timer_cancel(&timer) == 0);
	CHECK(deadline_after(SHORT_DELAY_NS * 2, &deadline) == 0);
	CHECK(kobox_posix_permit_wait(
		&timer_test.fired, deadline) == ETIMEDOUT);
	CHECK(kobox_posix_oneshot_timer_destroy(&timer) == 0);
	CHECK(kobox_posix_permit_destroy(&timer_test.fired) == 0);
	return 0;
}

static int test_same_cpu_serialization(struct kobox_posix_cpu *cpu)
{
	struct kobox_posix_thread threads[2] = {{0}};
	struct kobox_posix_permit first_inside = {0};
	struct kobox_posix_permit release_first = {0};
	struct kobox_posix_permit second_attempting = {0};
	struct same_cpu_case tests[2];
	atomic_uint active;
	atomic_uint maximum;
	uint64_t deadline;

	atomic_init(&active, 0);
	atomic_init(&maximum, 0);
	CHECK(kobox_posix_permit_init(&first_inside, 0) == 0);
	CHECK(kobox_posix_permit_init(&release_first, 0) == 0);
	CHECK(kobox_posix_permit_init(&second_attempting, 0) == 0);
	tests[0] = (struct same_cpu_case) {
		.cpu = cpu,
		.active = &active,
		.maximum = &maximum,
		.first_inside = &first_inside,
		.release_first = &release_first,
		.second_attempting = &second_attempting,
		.first = true,
	};
	tests[1] = tests[0];
	tests[1].first = false;
	CHECK(kobox_posix_thread_start(
		&threads[0], same_cpu_worker, &tests[0]) == 0);
	CHECK(deadline_after(TEST_TIMEOUT_NS, &deadline) == 0);
	CHECK(kobox_posix_permit_wait(&first_inside, deadline) == 0);
	CHECK(kobox_posix_thread_start(
		&threads[1], second_same_cpu_worker, &tests[1]) == 0);
	CHECK(kobox_posix_permit_wait(&second_attempting, deadline) == 0);
	delay_ns(SHORT_DELAY_NS);
	CHECK(atomic_load_explicit(&active, memory_order_acquire) == 1);
	CHECK(kobox_posix_permit_post(&release_first, 1) == 0);
	CHECK(join_success(&threads[0]) == 0);
	CHECK(join_success(&threads[1]) == 0);
	CHECK(atomic_load_explicit(&maximum, memory_order_acquire) == 1);
	CHECK(atomic_load_explicit(&active, memory_order_acquire) == 0);
	CHECK(kobox_posix_permit_destroy(&second_attempting) == 0);
	CHECK(kobox_posix_permit_destroy(&release_first) == 0);
	CHECK(kobox_posix_permit_destroy(&first_inside) == 0);
	return 0;
}

static int test_different_cpu_parallel(
	struct kobox_posix_cpu *cpu0,
	struct kobox_posix_cpu *cpu1)
{
	struct kobox_posix_thread threads[2] = {{0}};
	atomic_uint entered_mask;
	struct parallel_case tests[2];

	atomic_init(&entered_mask, 0);
	tests[0] = (struct parallel_case) {
		.cpu = cpu0, .entered_mask = &entered_mask, .bit = 1U,
	};
	tests[1] = (struct parallel_case) {
		.cpu = cpu1, .entered_mask = &entered_mask, .bit = 2U,
	};
	CHECK(kobox_posix_thread_start(
		&threads[0], parallel_worker, &tests[0]) == 0);
	CHECK(kobox_posix_thread_start(
		&threads[1], parallel_worker, &tests[1]) == 0);
	CHECK(join_success(&threads[0]) == 0);
	CHECK(join_success(&threads[1]) == 0);
	CHECK(atomic_load_explicit(&entered_mask, memory_order_acquire) == 3U);
	return 0;
}

static int test_cpu_bound_notification(
	struct kobox_posix_cpu *cpu,
	struct notification_state *notifications,
	enum kobox_posix_notification notification)
{
	struct kobox_posix_thread thread = {0};
	struct kobox_posix_oneshot_timer timer = {0};
	struct tick_case tick = {
		.cpu = cpu,
		.notifications = notifications,
		.notification = notification,
	};
	struct timer_notify_case notify = {
		.cpu = cpu,
		.notification = notification,
	};
	uint64_t deadline;

	atomic_store_explicit(
		&notifications->count[notification], 0,
		memory_order_release);
	atomic_init(&notify.fires, 0);
	CHECK(kobox_posix_permit_init(&tick.ready, 0) == 0);
	CHECK(kobox_posix_oneshot_timer_init(&timer, timer_notify, &notify) == 0);
	CHECK(kobox_posix_thread_start(
		&thread, cpu_bound_tick_worker, &tick) == 0);
	CHECK(deadline_after(TEST_TIMEOUT_NS, &deadline) == 0);
	CHECK(kobox_posix_permit_wait(&tick.ready, deadline) == 0);
	CHECK(deadline_after(SHORT_DELAY_NS, &deadline) == 0);
	CHECK(kobox_posix_oneshot_timer_arm(&timer, deadline) == 0);
	CHECK(join_success(&thread) == 0);
	CHECK(atomic_load_explicit(&notify.fires, memory_order_acquire) == 1);
	CHECK(atomic_load_explicit(
		&notifications->count[notification],
		memory_order_acquire) == 1);
	CHECK(kobox_posix_oneshot_timer_destroy(&timer) == 0);
	CHECK(kobox_posix_permit_destroy(&tick.ready) == 0);
	return 0;
}

static int test_irq_disable_pending(
	struct kobox_posix_cpu *cpu,
	struct notification_state *notifications)
{
	struct kobox_posix_thread thread = {0};
	struct irq_case test = {.cpu = cpu, .notifications = notifications};
	uint64_t deadline;

	atomic_store_explicit(
		&notifications->count[KOBOX_POSIX_NOTIFICATION_IRQ], 0,
		memory_order_release);
	atomic_init(&test.enable, false);
	CHECK(kobox_posix_permit_init(&test.ready, 0) == 0);
	CHECK(kobox_posix_thread_start(
		&thread, irq_disabled_worker, &test) == 0);
	CHECK(deadline_after(TEST_TIMEOUT_NS, &deadline) == 0);
	CHECK(kobox_posix_permit_wait(&test.ready, deadline) == 0);
	CHECK(kobox_posix_cpu_notify(
		cpu, KOBOX_POSIX_NOTIFICATION_IRQ) == 0);
	CHECK(kobox_posix_cpu_notify(
		cpu, KOBOX_POSIX_NOTIFICATION_IRQ) == 0);
	CHECK(kobox_posix_cpu_notify(
		cpu, KOBOX_POSIX_NOTIFICATION_IRQ) == 0);
	delay_ns(SHORT_DELAY_NS);
	CHECK(atomic_load_explicit(
		&notifications->count[KOBOX_POSIX_NOTIFICATION_IRQ],
		memory_order_acquire) == 0);
	CHECK(kobox_posix_cpu_pending(
		cpu, KOBOX_POSIX_NOTIFICATION_IRQ) == 3);
	atomic_store_explicit(&test.enable, true, memory_order_release);
	CHECK(join_success(&thread) == 0);
	CHECK(kobox_posix_cpu_pending(
		cpu, KOBOX_POSIX_NOTIFICATION_IRQ) == 0);
	CHECK(kobox_posix_permit_destroy(&test.ready) == 0);
	return 0;
}

int main(void)
{
	struct kobox_posix_cpu cpus[2] = {{0}};
	struct notification_state notifications = {0};
	unsigned int index;

	CHECK(test_basic_primitives() == 0);
	for (index = 0; index < KOBOX_POSIX_NOTIFICATION_COUNT; index++)
		atomic_init(&notifications.count[index], 0);
	atomic_init(&notifications.cpu_mismatch, 0);
	CHECK(kobox_posix_cpu_init(
		&cpus[0], 0, notification_callback, &notifications) == 0);
	CHECK(kobox_posix_cpu_init(
		&cpus[1], 1, notification_callback, &notifications) == 0);
	CHECK(test_same_cpu_serialization(&cpus[0]) == 0);
	CHECK(test_different_cpu_parallel(&cpus[0], &cpus[1]) == 0);
	CHECK(test_cpu_bound_notification(
		&cpus[0], &notifications, KOBOX_POSIX_NOTIFICATION_TICK) == 0);
	CHECK(test_cpu_bound_notification(
		&cpus[1], &notifications, KOBOX_POSIX_NOTIFICATION_IRQ) == 0);
	CHECK(test_irq_disable_pending(&cpus[1], &notifications) == 0);
	CHECK(atomic_load_explicit(
		&notifications.cpu_mismatch, memory_order_acquire) == 0);
	CHECK(kobox_posix_cpu_destroy(&cpus[1]) == 0);
	CHECK(kobox_posix_cpu_destroy(&cpus[0]) == 0);
	return 0;
}
