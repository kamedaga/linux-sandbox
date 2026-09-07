// SPDX-License-Identifier: GPL-2.0-only

#include "time_port.h"

#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/timekeeping.h>

#include "../../kernel/time/tick-internal.h"

struct kobox_clockevent {
	struct clock_event_device device;
	u64 host_deadline;
	bool armed;
};

static DEFINE_PER_CPU(struct kobox_clockevent, host_events);

static int host_next_event(unsigned long delta, struct clock_event_device *dev)
{
	struct kobox_clockevent *event = container_of(dev, typeof(*event), device);
	const struct kobox_linux_task_host_operations *host = kobox_task_host();
	u64 now;
	int status;

	if (!irqs_disabled() || host->monotonic_ns(&now) ||
	    U64_MAX - now < delta)
		__builtin_trap();
	event->host_deadline = now + delta;
	event->armed = true;
	status = host->clockevent_arm(smp_processor_id(), event->host_deadline);
	if (status)
		__builtin_trap();
	return 0;
}

static int host_event_shutdown(struct clock_event_device *dev)
{
	struct kobox_clockevent *event = container_of(dev, typeof(*event), device);

	event->armed = false;
	if (kobox_task_host()->clockevent_cancel(smp_processor_id()))
		__builtin_trap();
	return 0;
}

void kobox_task_clock_init(void)
{
	struct kobox_clockevent *event = this_cpu_ptr(&host_events);
	struct clock_event_device *dev = &event->device;

	if (!irqs_disabled())
		__builtin_trap();
	dev->name = "kobox-oneshot";
	dev->features = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERCPU;
	dev->rating = 400;
	dev->cpumask = cpumask_of(smp_processor_id());
	dev->set_next_event = host_next_event;
	dev->set_state_shutdown = host_event_shutdown;
	dev->set_state_oneshot = host_event_shutdown;
	/* Nanosecond device counter; Linux owns all deadline selection. */
	clockevents_config_and_register(dev, NSEC_PER_SEC, 1000, S64_MAX);
}

void kobox_task_clock_interrupt(void)
{
	struct kobox_clockevent *event = this_cpu_ptr(&host_events);
	u64 now;

	if (!in_hardirq() || !irqs_disabled() ||
	    kobox_task_host()->monotonic_ns(&now))
		__builtin_trap();
	/*
	 * Like a hardware interrupt already in flight, an old host notification
	 * may survive cancel/reprogram. It cannot expire the new device deadline.
	 */
	if (!event->armed || now < event->host_deadline)
		return;
	event->armed = false;
	event->device.event_handler(&event->device);
}

void kobox_task_clock_stop(void)
{
	struct kobox_clockevent *event = this_cpu_ptr(&host_events);

	local_irq_disable();
	clockevents_shutdown(&event->device);
	/* Stop the host producer before retiring this fixture CPU's domain. */
	if (kobox_task_host()->clockevent_stop(smp_processor_id()))
		__builtin_trap();
	local_irq_enable();
}
