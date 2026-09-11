/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_TASK_HOST_H
#define KOBOX_LINUX_TASK_HOST_H

#include "../memory/host.h"

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define KOBOX_LINUX_TASK_HOST_IDENTITY 0x4b4f424f58544153ULL

enum kobox_linux_task_notification {
	KOBOX_LINUX_TASK_RESCHEDULE = 0,
	KOBOX_LINUX_TASK_CALL_FUNCTION,
	KOBOX_LINUX_TASK_DEVICE_IRQ,
	KOBOX_LINUX_TASK_CONTROL_EVENT,
	KOBOX_LINUX_TASK_VM_EVENT,
	KOBOX_LINUX_TASK_CLOCKEVENT,
};

struct kobox_linux_task_host_operations {
	size_t size;
	uint64_t identity;
	int (*task_bind_current)(void **task_out);
	int (*task_create)(void **task_out, void *(*entry)(void *),
			   void *argument);
	int (*task_wake)(void *task);
	int (*task_park)(void *task);
	int (*task_join_destroy)(void *task);
	int (*task_destroy_current)(void *task);
	void (*task_exit)(void);
	int (*cpu_enter)(uint32_t cpu, void *task);
	int (*cpu_leave)(uint32_t cpu);
	int (*cpu_switch)(uint32_t cpu, void *previous_task,
			  void *next_task, uint8_t exiting);
	int (*cpu_wait)(uint32_t cpu, uint64_t observed_sequence,
			uint64_t *sequence_out);
	/* Irreversible remote halt, acknowledged even with guest IRQs disabled. */
	int (*cpu_stop)(uint32_t cpu);
	int (*cpu_notify)(uint32_t cpu,
			  enum kobox_linux_task_notification notification);
	int (*cpu_irq_disable)(uint32_t cpu);
	int (*cpu_irq_enable)(uint32_t cpu);
	int (*notifications_save)(uint64_t *mask_out);
	int (*notifications_restore)(uint64_t mask);
	uint8_t (*cpu_irq_disabled)(uint32_t cpu);
	uint64_t (*cpu_notification_sequence)(uint32_t cpu);
	int (*monotonic_ns)(uint64_t *time_out);
	int (*realtime_ns)(uint64_t *time_out);
	int (*clockevent_arm)(uint32_t cpu, uint64_t monotonic_deadline_ns);
	int (*clockevent_cancel)(uint32_t cpu);
	int (*clockevent_stop)(uint32_t cpu);
};

struct kobox_linux_task_layout {
	size_t size;
	uint64_t identity;
	struct kobox_linux_memory_layout memory;
	const struct kobox_linux_task_host_operations *operations;
	void *boot_task;
};

struct kobox_linux_task_report {
	size_t size;
	uint64_t identity;
	uint64_t context_switches;
	uint64_t remote_reschedule_ipis;
	uint64_t hardirq_entries[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint64_t idle_irq_entries[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint64_t idle_entries[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint64_t idle_exits[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint64_t tick_progress[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint8_t high_resolution_ready[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
	uint32_t logical_cpu_count;
	uint8_t upstream_schedule_ready;
	uint8_t upstream_try_to_wake_up_ready;
	uint8_t current_percpu_ready;
	uint8_t local_switch_ready;
	uint8_t remote_switch_ready;
	uint8_t migration_ready;
	uint8_t affinity_ready;
	uint8_t preempt_disable_ready;
	uint8_t irq_disable_ready;
	uint8_t exit_join_ready;
};

typedef int (*kobox_linux_task_boot_fn)(
	const struct kobox_linux_task_layout *layout,
	struct kobox_linux_task_report *report);

/* Machine IRQ entry/return: host masks IRQs on entry and restores on return. */
typedef void (*kobox_linux_task_notification_fn)(
	uint32_t cpu,
	enum kobox_linux_task_notification notification,
	uint64_t count);

#endif /* KOBOX_LINUX_TASK_HOST_H */
