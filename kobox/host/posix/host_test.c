/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
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

struct nested_irq_case {
	struct kobox_posix_cpu cpu;
	atomic_uint phase;
	atomic_uint nested;
	atomic_uint errors;
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

struct handoff_case {
	struct kobox_posix_cpu *cpu;
	struct kobox_posix_task *main_task;
	struct kobox_posix_task *worker_task;
	atomic_uint entered;
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
	enum kobox_posix_notification notification;
};

struct timer_case {
	struct kobox_posix_permit fired;
	atomic_uint fire_count;
	atomic_uint_fast64_t fired_ns;
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

static void *handoff_worker(void *argument)
{
	struct handoff_case *test = argument;
	int status;

	status = kobox_posix_cpu_enter_task(test->cpu, test->worker_task);
	if (!status)
		atomic_fetch_add_explicit(
			&test->entered, 1, memory_order_release);
	if (!status)
		status = kobox_posix_cpu_switch(
			test->cpu, test->worker_task, test->main_task, false);
	if (!status)
		status = kobox_posix_cpu_enter_task(
			test->cpu, test->worker_task);
	if (!status)
		atomic_fetch_add_explicit(
			&test->entered, 1, memory_order_release);
	if (!status)
		status = kobox_posix_cpu_switch(
			test->cpu, test->worker_task, test->main_task, true);
	if (!status)
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
		    &test->notifications->count[test->notification],
		    memory_order_acquire) != 0 ||
		    kobox_posix_cpu_pending(
			    test->cpu, test->notification) != 3))
		status = EIO;
	if (!status)
		status = kobox_posix_cpu_irq_enable(test->cpu);
	if (!status && atomic_load_explicit(
		    &test->notifications->count[test->notification],
		    memory_order_acquire) != 3)
		status = EIO;
	if (kobox_posix_cpu_leave(test->cpu) && !status)
		status = EIO;
	return (void *)(uintptr_t)status;
}

static void timer_fire(void *argument)
{
	struct timer_case *test = argument;
	uint64_t now;

	if (kobox_posix_monotonic_ns(&now))
		__builtin_trap();
	atomic_store_explicit(&test->fired_ns, now, memory_order_relaxed);
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
	struct kobox_posix_memory_backing backing = {0};
	struct kobox_posix_memory_window windows[2] = {{0}};
	struct kobox_posix_permit permit = {0};
	struct kobox_posix_oneshot_timer timer = {0};
	struct timer_case timer_test = {0};
	uint64_t before;
	uint64_t after;
	uint64_t deadline;
	long page_size;
	unsigned char *mapping;
	unsigned char *aliases[2];
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

	CHECK(kobox_posix_memory_backing_init(
		&backing, (size_t)page_size * 2) == 0);
	CHECK(kobox_posix_memory_window_init(
		&windows[0], (size_t)page_size * 2) == 0);
	CHECK(kobox_posix_memory_window_init(
		&windows[1], (size_t)page_size * 2) == 0);
	CHECK(kobox_posix_memory_window_map(
		&windows[0], 0, &backing, 0, (size_t)page_size * 2,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE,
		(void **)&aliases[0]) == 0);
	CHECK(kobox_posix_memory_window_map(
		&windows[1], 0, &backing, 0, (size_t)page_size * 2,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE,
		(void **)&aliases[1]) == 0);
	aliases[0][0] = 0x39;
	aliases[1][(size_t)page_size + 7] = 0xc4;
	CHECK(aliases[1][0] == 0x39 &&
	      aliases[0][(size_t)page_size + 7] == 0xc4);
	CHECK(kobox_posix_memory_window_reset(
		&windows[1], 0, (size_t)page_size) == 0);
	CHECK(aliases[0][0] == 0x39);
	CHECK(kobox_posix_memory_window_destroy(&windows[1]) == 0);
	CHECK(kobox_posix_memory_window_destroy(&windows[0]) == 0);
	CHECK(kobox_posix_memory_backing_destroy(&backing) == 0);

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
	atomic_init(&timer_test.fired_ns, 0);
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
	/* Cancel must leave the device usable; reprogram in both directions. */
	CHECK(deadline_after(SHORT_DELAY_NS, &before) == 0);
	CHECK(kobox_posix_oneshot_timer_arm(&timer, before) == 0);
	CHECK(deadline_after(SHORT_DELAY_NS * 4, &after) == 0);
	CHECK(kobox_posix_oneshot_timer_arm(&timer, after) == 0);
	CHECK(kobox_posix_permit_wait(&timer_test.fired, before) == ETIMEDOUT);
	CHECK(deadline_after(TEST_TIMEOUT_NS, &deadline) == 0);
	CHECK(kobox_posix_permit_wait(&timer_test.fired, deadline) == 0);
	CHECK(atomic_load_explicit(&timer_test.fired_ns, memory_order_acquire) >= after);
	CHECK(deadline_after(TEST_TIMEOUT_NS, &after) == 0);
	CHECK(kobox_posix_oneshot_timer_arm(&timer, after) == 0);
	CHECK(deadline_after(SHORT_DELAY_NS, &before) == 0);
	CHECK(kobox_posix_oneshot_timer_arm(&timer, before) == 0);
	CHECK(kobox_posix_permit_wait(&timer_test.fired, after) == 0);
	CHECK(atomic_load_explicit(&timer_test.fired_ns, memory_order_acquire) >= before);
	CHECK(atomic_load_explicit(&timer_test.fire_count, memory_order_acquire) == 3);
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

static int test_cpu_handoff(struct kobox_posix_cpu *cpu)
{
	struct kobox_posix_task *main_task = NULL;
	struct kobox_posix_task *worker_task = NULL;
	struct handoff_case test = {.cpu = cpu};

	atomic_init(&test.entered, 0);
	CHECK(kobox_posix_task_bind_current(&main_task) == 0);
	test.main_task = main_task;
	CHECK(kobox_posix_task_start(
		&worker_task, handoff_worker, &test) == 0);
	test.worker_task = worker_task;
	CHECK(kobox_posix_cpu_enter_task(cpu, main_task) == 0);
	CHECK(kobox_posix_cpu_switch(
		cpu, main_task, worker_task, false) == 0);
	CHECK(kobox_posix_cpu_enter_task(cpu, main_task) == 0);
	CHECK(atomic_load_explicit(
		&test.entered, memory_order_acquire) == 1);
	CHECK(kobox_posix_cpu_switch(
		cpu, main_task, worker_task, false) == 0);
	CHECK(kobox_posix_cpu_enter_task(cpu, main_task) == 0);
	CHECK(atomic_load_explicit(
		&test.entered, memory_order_acquire) == 2);
	CHECK(kobox_posix_task_join_destroy(worker_task) == 0);
	CHECK(kobox_posix_cpu_leave(cpu) == 0);
	CHECK(kobox_posix_task_destroy_current(main_task) == 0);
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
	struct notification_state *notifications,
	enum kobox_posix_notification notification)
{
	struct kobox_posix_thread thread = {0};
	struct irq_case test = {.cpu = cpu, .notifications = notifications, .notification = notification};
	uint64_t deadline;

	atomic_store_explicit(
		&notifications->count[notification], 0,
		memory_order_release);
	atomic_init(&test.enable, false);
	CHECK(kobox_posix_permit_init(&test.ready, 0) == 0);
	CHECK(kobox_posix_thread_start(
		&thread, irq_disabled_worker, &test) == 0);
	CHECK(deadline_after(TEST_TIMEOUT_NS, &deadline) == 0);
	CHECK(kobox_posix_permit_wait(&test.ready, deadline) == 0);
	CHECK(kobox_posix_cpu_notify(
		cpu, notification) == 0);
	CHECK(kobox_posix_cpu_notify(
		cpu, notification) == 0);
	CHECK(kobox_posix_cpu_notify(
		cpu, notification) == 0);
	delay_ns(SHORT_DELAY_NS);
	CHECK(atomic_load_explicit(
		&notifications->count[notification],
		memory_order_acquire) == 0);
	CHECK(kobox_posix_cpu_pending(
		cpu, notification) == 3);
	atomic_store_explicit(&test.enable, true, memory_order_release);
	CHECK(join_success(&thread) == 0);
	CHECK(kobox_posix_cpu_pending(
		cpu, notification) == 0);
	CHECK(kobox_posix_permit_destroy(&test.ready) == 0);
	return 0;
}

static int test_idle_pending_before_sequence(
	struct kobox_posix_cpu *cpu,
	struct notification_state *notifications)
{
	uint64_t sequence;
	uint64_t observed;
	uint64_t delivered;

	delivered = atomic_load_explicit(
		&notifications->count[KOBOX_POSIX_NOTIFICATION_IRQ],
		memory_order_acquire);
	CHECK(kobox_posix_cpu_enter(cpu) == 0);
	CHECK(kobox_posix_cpu_irq_disable(cpu) == 0);
	CHECK(kobox_posix_cpu_notify(cpu, KOBOX_POSIX_NOTIFICATION_IRQ) == 0);
	observed = kobox_posix_cpu_notification_sequence(cpu);
	/* The notification is pending, although the sequence is already seen. */
	CHECK(kobox_posix_cpu_wait(cpu, observed, &sequence) == 0);
	CHECK(sequence == observed);
	CHECK(kobox_posix_cpu_pending(cpu, KOBOX_POSIX_NOTIFICATION_IRQ) == 1);
	CHECK(kobox_posix_cpu_irq_enable(cpu) == 0);
	CHECK(atomic_load_explicit(
		&notifications->count[KOBOX_POSIX_NOTIFICATION_IRQ],
		memory_order_acquire) == delivered + 1);
	CHECK(kobox_posix_cpu_leave(cpu) == 0);
	return 0;
}

static void nested_irq_callback(void *context, uint32_t cpu,
				enum kobox_posix_notification notification,
				uint64_t count)
{
	struct nested_irq_case *test = context;
	uint64_t deadline;
	uint64_t now;

	if (cpu != test->cpu.logical_cpu)
		atomic_fetch_add(&test->errors, 1);
	if (!kobox_posix_cpu_irq_disabled(&test->cpu))
		atomic_fetch_add(&test->errors, 1);
	if (notification == KOBOX_POSIX_NOTIFICATION_TICK) {
		if (atomic_load_explicit(&test->phase, memory_order_acquire) >= 3)
			atomic_fetch_add(&test->errors, 1);
		atomic_fetch_add_explicit(&test->nested, count, memory_order_release);
		return;
	}
	if (deadline_after(TEST_TIMEOUT_NS, &deadline))
		__builtin_trap();
	atomic_store_explicit(&test->phase, 1, memory_order_release);
	while (atomic_load_explicit(&test->phase, memory_order_acquire) != 2) {
		if (kobox_posix_monotonic_ns(&now) || now >= deadline)
			__builtin_trap();
	}
	if (atomic_load_explicit(&test->nested, memory_order_acquire))
		atomic_fetch_add(&test->errors, 1);
	if (kobox_posix_cpu_irq_enable(&test->cpu) ||
	    atomic_load_explicit(&test->nested, memory_order_acquire) != 1)
		atomic_fetch_add(&test->errors, 1);
	/* The second IRQ is produced later, after the synchronous drain ended. */
	atomic_store_explicit(&test->phase, 0, memory_order_release);
	while (atomic_load_explicit(&test->nested, memory_order_acquire) != 2) {
		if (kobox_posix_monotonic_ns(&now) || now >= deadline) {
			atomic_fetch_add(&test->errors, 1);
			break;
		}
	}
	if (kobox_posix_cpu_irq_disable(&test->cpu))
		atomic_fetch_add(&test->errors, 1);
	atomic_store_explicit(&test->phase, 3, memory_order_release);
}

static void *nested_irq_sender(void *argument)
{
	struct nested_irq_case *test = argument;
	uint64_t deadline;
	uint64_t now;

	if (deadline_after(TEST_TIMEOUT_NS, &deadline))
		return (void *)1;
	while (atomic_load_explicit(&test->phase, memory_order_acquire) != 1) {
		if (kobox_posix_monotonic_ns(&now) || now >= deadline)
			return (void *)1;
	}
	if (kobox_posix_cpu_notify(&test->cpu, KOBOX_POSIX_NOTIFICATION_TICK))
		return (void *)1;
	atomic_store_explicit(&test->phase, 2, memory_order_release);
	while (atomic_load_explicit(&test->phase, memory_order_acquire) != 0) {
		if (kobox_posix_monotonic_ns(&now) || now >= deadline)
			return (void *)1;
	}
	if (kobox_posix_cpu_notify(&test->cpu, KOBOX_POSIX_NOTIFICATION_TICK))
		return (void *)1;
	return NULL;
}

static int test_nested_irq(unsigned int cpu)
{
	struct nested_irq_case test = {0};
	struct kobox_posix_thread sender = {0};
	void *result;

	atomic_init(&test.phase, 4);
	atomic_init(&test.nested, 0);
	atomic_init(&test.errors, 0);
	CHECK(kobox_posix_cpu_init(&test.cpu, cpu, nested_irq_callback, &test) == 0);
	CHECK(kobox_posix_thread_start(&sender, nested_irq_sender, &test) == 0);
	CHECK(kobox_posix_cpu_enter(&test.cpu) == 0);
	CHECK(kobox_posix_cpu_notify(&test.cpu, KOBOX_POSIX_NOTIFICATION_IRQ) == 0);
	CHECK(!kobox_posix_cpu_irq_disabled(&test.cpu));
	CHECK(kobox_posix_cpu_leave(&test.cpu) == 0);
	CHECK(kobox_posix_thread_join(&sender, &result) == 0);
	CHECK(result == NULL);
	CHECK(kobox_posix_cpu_destroy(&test.cpu) == 0);
	CHECK(atomic_load(&test.nested) == 2);
	CHECK(atomic_load(&test.errors) == 0);
	return 0;
}

struct stop_case {
	struct kobox_posix_cpu cpu;
	atomic_uint_fast64_t iterations;
	bool disable_irq;
};

static void *stop_worker(void *argument)
{
	struct stop_case *test = argument;

	if (kobox_posix_cpu_enter(&test->cpu) ||
	    (test->disable_irq && kobox_posix_cpu_irq_disable(&test->cpu)))
		_exit(2);
	for (;;)
		atomic_fetch_add_explicit(&test->iterations, 1, memory_order_release);
	return NULL;
}

static int stop_child(unsigned int target, bool disable_irq)
{
	struct stop_case test = {.disable_irq = disable_irq};
	struct kobox_posix_cpu self = {0};
	struct notification_state notifications = {0};
	struct kobox_posix_thread worker = {0};
	struct timespec delay = {.tv_nsec = SHORT_DELAY_NS};
	uint_fast64_t before;

	/* This machine halt deliberately retains ownership until process exit. */
	alarm(5);
	atomic_init(&test.iterations, 0);
	CHECK(kobox_posix_cpu_init(&test.cpu, target, notification_callback,
				   &notifications) == 0);
	CHECK(kobox_posix_cpu_init(&self, target ^ 1, notification_callback,
				   &notifications) == 0);
	CHECK(kobox_posix_cpu_enter(&self) == 0);
	CHECK(kobox_posix_thread_start(&worker, stop_worker, &test) == 0);
	while (!atomic_load_explicit(&test.iterations, memory_order_acquire))
		;
	CHECK(kobox_posix_cpu_stop(&test.cpu) == 0);
	before = atomic_load_explicit(&test.iterations, memory_order_acquire);
	CHECK(nanosleep(&delay, NULL) == 0);
	CHECK(atomic_load_explicit(&test.iterations, memory_order_acquire) == before);
	CHECK(atomic_load(&notifications.count[KOBOX_POSIX_NOTIFICATION_IRQ]) == 0);
	CHECK(kobox_posix_cpu_irq_disabled(&test.cpu) == disable_irq);
	CHECK(kobox_posix_cpu_leave(&self) == 0);
	CHECK(kobox_posix_cpu_destroy(&self) == 0);
	return 0;
}

static int test_cpu_stop(void)
{
	unsigned int target, disabled;
	int status;
	pid_t child;

	for (target = 0; target < 2; target++) {
		for (disabled = 0; disabled < 2; disabled++) {
			child = fork();
			CHECK(child >= 0);
			if (!child)
				_exit(stop_child(target, disabled));
			CHECK(waitpid(child, &status, 0) == child);
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		}
	}
	return 0;
}

int main(void)
{
	struct kobox_posix_cpu cpus[2] = {0};
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
	CHECK(test_cpu_handoff(&cpus[0]) == 0);
	CHECK(test_cpu_bound_notification(
		&cpus[0], &notifications, KOBOX_POSIX_NOTIFICATION_TICK) == 0);
	CHECK(test_cpu_bound_notification(
		&cpus[1], &notifications, KOBOX_POSIX_NOTIFICATION_IRQ) == 0);
	CHECK(test_irq_disable_pending(&cpus[1], &notifications, KOBOX_POSIX_NOTIFICATION_IRQ) == 0);
	CHECK(test_irq_disable_pending(&cpus[0], &notifications, KOBOX_POSIX_NOTIFICATION_CONTROL_EVENT) == 0);
	CHECK(test_irq_disable_pending(&cpus[1], &notifications, KOBOX_POSIX_NOTIFICATION_CONTROL_EVENT) == 0);
	CHECK(test_irq_disable_pending(&cpus[0], &notifications, KOBOX_POSIX_NOTIFICATION_VM_EVENT) == 0);
	CHECK(test_irq_disable_pending(&cpus[1], &notifications, KOBOX_POSIX_NOTIFICATION_VM_EVENT) == 0);
	CHECK(test_idle_pending_before_sequence(&cpus[0], &notifications) == 0);
	CHECK(atomic_load_explicit(
		&notifications.cpu_mismatch, memory_order_acquire) == 0);
	CHECK(kobox_posix_cpu_destroy(&cpus[1]) == 0);
	CHECK(kobox_posix_cpu_destroy(&cpus[0]) == 0);
	CHECK(test_nested_irq(0) == 0);
	CHECK(test_nested_irq(1) == 0);
	CHECK(test_cpu_stop() == 0);
	return 0;
}
