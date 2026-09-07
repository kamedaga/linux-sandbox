// SPDX-License-Identifier: GPL-2.0-only

#include "posix_machine.h"
#include "../host/posix/host.h"

#include <errno.h>

static struct kobox_posix_cpu cpus[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
static struct kobox_posix_oneshot_timer timers[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
static kobox_linux_task_notification_fn task_dispatch;

static int host_protection(unsigned int protection, unsigned int *native)
{
	if (protection & ~(KOBOX_LINUX_MEMORY_READ | KOBOX_LINUX_MEMORY_WRITE |
			   KOBOX_LINUX_MEMORY_EXECUTE))
		return EINVAL;
	*native = 0;
	if (protection & KOBOX_LINUX_MEMORY_READ)
		*native |= KOBOX_POSIX_MEMORY_READ;
	if (protection & KOBOX_LINUX_MEMORY_WRITE)
		*native |= KOBOX_POSIX_MEMORY_WRITE;
	if (protection & KOBOX_LINUX_MEMORY_EXECUTE)
		*native |= KOBOX_POSIX_MEMORY_EXECUTE;
	return 0;
}

static int host_map(void *window, size_t window_offset, void *backing,
		    size_t backing_offset, size_t size,
		    unsigned int protection, void **address_out)
{
	unsigned int native = 0;
	uint64_t mask;
	int status;
	int restore_status;

	status = host_protection(protection, &native);
	if (status)
		return status;
	status = kobox_posix_notifications_save(&mask);
	if (status)
		return status;
	status = kobox_posix_memory_window_map(window, window_offset, backing,
		backing_offset, size, native, address_out);
	restore_status = kobox_posix_notifications_restore(mask);
	return status ? status : restore_status;
}

static int host_protect(void *opaque, size_t offset, size_t size,
			unsigned int protection)
{
	struct kobox_posix_memory_window *window = opaque;
	unsigned int native;
	uint64_t mask;
	int restore_status, status;

	if (!window || !window->initialized || offset > window->size ||
	    size > window->size - offset || !size ||
	    offset % KOBOX_LINUX_MEMORY_PAGE_SIZE || size % KOBOX_LINUX_MEMORY_PAGE_SIZE)
		return EINVAL;
	status = host_protection(protection, &native);
	if (status)
		return status;
	status = kobox_posix_notifications_save(&mask);
	if (status)
		return status;
	status = kobox_posix_memory_protect((char *)window->address + offset,
					  size, native);
	restore_status = kobox_posix_notifications_restore(mask);
	return status ? status : restore_status;
}

static int host_reset(void *window, size_t window_offset, size_t size)
{
	uint64_t mask;
	int status = kobox_posix_notifications_save(&mask);
	int restore_status;

	if (status)
		return status;
	status = kobox_posix_memory_window_reset(window, window_offset, size);
	restore_status = kobox_posix_notifications_restore(mask);
	return status ? status : restore_status;
}

static int host_task_bind_current(void **task_out)
{
	return kobox_posix_task_bind_current(
		(struct kobox_posix_task **)task_out);
}

static int host_task_create(
	void **task_out,
	void *(*entry)(void *),
	void *argument)
{
	return kobox_posix_task_start(
		(struct kobox_posix_task **)task_out, entry, argument);
}

static int host_task_wake(void *task)
{
	return kobox_posix_task_wake(task);
}

static int host_task_park(void *task)
{
	return kobox_posix_task_park(task);
}

static int host_task_join_destroy(void *task)
{
	return kobox_posix_task_join_destroy(task);
}

static int host_task_destroy_current(void *task)
{
	return kobox_posix_task_destroy_current(task);
}

static int host_cpu_enter(uint32_t cpu, void *task)
{
	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	return kobox_posix_cpu_enter_task(&cpus[cpu], task);
}

static int host_cpu_leave(uint32_t cpu)
{
	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	return kobox_posix_cpu_leave(&cpus[cpu]);
}

static int host_cpu_switch(
	uint32_t cpu,
	void *previous_task,
	void *next_task,
	uint8_t exiting)
{
	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	return kobox_posix_cpu_switch(
		&cpus[cpu], previous_task, next_task, exiting != 0);
}

static int host_cpu_wait(
	uint32_t cpu,
	uint64_t observed_sequence,
	uint64_t *sequence_out)
{
	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	return kobox_posix_cpu_wait(
		&cpus[cpu], observed_sequence, sequence_out);
}

static int host_cpu_notify(
	uint32_t cpu,
	enum kobox_linux_task_notification notification)
{
	enum kobox_posix_notification native;

	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	switch (notification) {
	case KOBOX_LINUX_TASK_RESCHEDULE:
		native = KOBOX_POSIX_NOTIFICATION_IRQ;
		break;
	case KOBOX_LINUX_TASK_CALL_FUNCTION:
		native = KOBOX_POSIX_NOTIFICATION_CALL_FUNCTION;
		break;
	case KOBOX_LINUX_TASK_CLOCKEVENT:
		native = KOBOX_POSIX_NOTIFICATION_TICK;
		break;
	case KOBOX_LINUX_TASK_VM_EVENT:
		native = KOBOX_POSIX_NOTIFICATION_VM_EVENT;
		break;
	default:
		return EINVAL;
	}
	return kobox_posix_cpu_notify(&cpus[cpu], native);
}

static void host_clockevent_fire(void *context)
{
	if (kobox_posix_cpu_notify(context, KOBOX_POSIX_NOTIFICATION_TICK))
		__builtin_trap();
}

static int host_clockevent_arm(uint32_t cpu, uint64_t deadline)
{
	uint64_t mask;
	int status;
	int restored;

	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return EINVAL;
	status = kobox_posix_notifications_save(&mask);
	if (status)
		return status;
	status = kobox_posix_oneshot_timer_arm(&timers[cpu], deadline);
	restored = kobox_posix_notifications_restore(mask);
	return status ? status : restored;
}

static int host_clockevent_control(uint32_t cpu, bool stop)
{
	uint64_t mask;
	int status;
	int restored;

	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return EINVAL;
	status = kobox_posix_notifications_save(&mask);
	if (status)
		return status;
	status = stop ? kobox_posix_oneshot_timer_destroy(&timers[cpu]) :
		kobox_posix_oneshot_timer_cancel(&timers[cpu]);
	restored = kobox_posix_notifications_restore(mask);
	return status ? status : restored;
}

static int host_clockevent_cancel(uint32_t cpu)
{
	return host_clockevent_control(cpu, false);
}

static int host_clockevent_stop(uint32_t cpu)
{
	return host_clockevent_control(cpu, true);
}

static int host_cpu_irq_disable(uint32_t cpu)
{
	return cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS ?
		kobox_posix_cpu_irq_disable(&cpus[cpu]) : -1;
}

static int host_cpu_irq_enable(uint32_t cpu)
{
	return cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS ?
		kobox_posix_cpu_irq_enable(&cpus[cpu]) : -1;
}

static uint8_t host_cpu_irq_disabled(uint32_t cpu)
{
	return cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS &&
		kobox_posix_cpu_irq_disabled(&cpus[cpu]);
}

static uint64_t host_cpu_notification_sequence(uint32_t cpu)
{
	return cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS ?
		kobox_posix_cpu_notification_sequence(&cpus[cpu]) : 0;
}

static void host_notification(
	void *context,
	uint32_t cpu,
	enum kobox_posix_notification notification,
	uint64_t count)
{
	enum kobox_linux_task_notification translated;

	(void)context;
	switch (notification) {
	case KOBOX_POSIX_NOTIFICATION_IRQ:
		translated = KOBOX_LINUX_TASK_RESCHEDULE;
		break;
	case KOBOX_POSIX_NOTIFICATION_CALL_FUNCTION:
		translated = KOBOX_LINUX_TASK_CALL_FUNCTION;
		break;
	case KOBOX_POSIX_NOTIFICATION_TICK:
		translated = KOBOX_LINUX_TASK_CLOCKEVENT;
		break;
	case KOBOX_POSIX_NOTIFICATION_VM_EVENT:
		translated = KOBOX_LINUX_TASK_VM_EVENT;
		break;
	default:
		__builtin_trap();
	}
	task_dispatch(cpu, translated, count);
}

const struct kobox_linux_memory_host_operations kobox_task_posix_memory_operations = {
	.size = sizeof(kobox_task_posix_memory_operations),
	.identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
	.map = host_map,
	.reset = host_reset,
	.protect = host_protect,
};

const struct kobox_linux_task_host_operations kobox_task_posix_operations = {
	.size = sizeof(kobox_task_posix_operations),
	.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
	.task_bind_current = host_task_bind_current,
	.task_create = host_task_create,
	.task_wake = host_task_wake,
	.task_park = host_task_park,
	.task_join_destroy = host_task_join_destroy,
	.task_destroy_current = host_task_destroy_current,
	.task_exit = kobox_posix_task_exit,
	.cpu_enter = host_cpu_enter,
	.cpu_leave = host_cpu_leave,
	.cpu_switch = host_cpu_switch,
	.cpu_wait = host_cpu_wait,
	.cpu_notify = host_cpu_notify,
	.cpu_irq_disable = host_cpu_irq_disable,
	.cpu_irq_enable = host_cpu_irq_enable,
	.notifications_save = kobox_posix_notifications_save,
	.notifications_restore = kobox_posix_notifications_restore,
	.cpu_irq_disabled = host_cpu_irq_disabled,
	.cpu_notification_sequence = host_cpu_notification_sequence,
	.monotonic_ns = kobox_posix_monotonic_ns,
	.realtime_ns = kobox_posix_realtime_ns,
	.clockevent_arm = host_clockevent_arm,
	.clockevent_cancel = host_clockevent_cancel,
	.clockevent_stop = host_clockevent_stop,
};

int kobox_task_posix_destroy(void)
{
	unsigned int cpu = KOBOX_LINUX_MEMORY_LOGICAL_CPUS;
	int status;

	while (cpu--) {
		if (timers[cpu].initialized) {
			status = kobox_posix_oneshot_timer_destroy(&timers[cpu]);
			if (status)
				return status;
		}
		if (cpus[cpu].initialized) {
			status = kobox_posix_cpu_destroy(&cpus[cpu]);
			if (status)
				return status;
		}
	}
	task_dispatch = NULL;
	return 0;
}

int kobox_task_posix_init(kobox_linux_task_notification_fn dispatch)
{
	unsigned int cpu;
	int status;

	if (!dispatch)
		return EINVAL;
	if (task_dispatch)
		return EBUSY;
	task_dispatch = dispatch;
	for (cpu = 0; cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS; cpu++) {
		status = kobox_posix_cpu_init(&cpus[cpu], cpu,
					     host_notification, NULL);
		if (status)
			goto fail;
		status = kobox_posix_oneshot_timer_init(&timers[cpu],
						      host_clockevent_fire,
						      &cpus[cpu]);
		if (status)
			goto fail;
	}
	return 0;
fail:
	/* On cleanup failure the still-owned domains keep init disabled. */
	(void)kobox_task_posix_destroy();
	return status;
}
