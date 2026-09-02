/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_FIXTURE_H
#define KOBOX_FIXTURE_H

#include "../runtime/module_context.h"

#include <stddef.h>
#include <stdint.h>

#define KOBOX_FIXTURE_CORE_IDENTITY_SIZE 4u
#define KOBOX_FIXTURE_CORE_IDENTITY_0 'd'
#define KOBOX_FIXTURE_CORE_IDENTITY_1 'e'
#define KOBOX_FIXTURE_CORE_IDENTITY_2 'v'
#define KOBOX_FIXTURE_CORE_IDENTITY_3 '\0'
#define KOBOX_FIXTURE_CPU_COUNT 2u
#define KOBOX_FIXTURE_RESULT UINT64_C(0x0002000200020001)

#define KOBOX_FIXTURE_CORE_NODE_ID 1u
#define KOBOX_FIXTURE_PROVIDER_NODE_ID 2u
#define KOBOX_FIXTURE_CONSUMER_NODE_ID 3u
#define KOBOX_FIXTURE_RESOURCE_SLOT_ID 1u

#define KOBOX_FIXTURE_LIFECYCLE_INIT 1u
#define KOBOX_FIXTURE_LIFECYCLE_QUIESCE 2u
#define KOBOX_FIXTURE_LIFECYCLE_CLEANUP 3u

typedef int (*kobox_fixture_thread_fn)(void *argument);

struct kobox_fixture_core_ops {
	uint32_t size;
	uint8_t identity[KOBOX_FIXTURE_CORE_IDENTITY_SIZE];

	void *(*allocate)(size_t size);
	void (*deallocate)(void *pointer, size_t size);

	int (*mutex_create)(void **mutex_out);
	void (*mutex_destroy)(void *mutex);
	int (*mutex_lock)(void *mutex);
	int (*mutex_unlock)(void *mutex);
	int (*spin_create)(void **spin_out);
	void (*spin_destroy)(void *spin);
	int (*spin_lock)(void *spin);
	int (*spin_unlock)(void *spin);

	int (*event_create)(void **event_out, int signaled);
	void (*event_destroy)(void *event);
	int (*event_wait)(void *event);
	int (*event_signal)(void *event);
	int (*event_reset)(void *event);

	int (*thread_create)(void **thread_out, kobox_fixture_thread_fn function,
			     void *argument, uint32_t logical_cpu);
	int (*thread_join)(void *thread, int *result_out);
	uint32_t (*current_cpu)(void);

	int (*percpu_create)(void **percpu_out, uint32_t cpu_count);
	void (*percpu_destroy)(void *percpu);
	int (*percpu_add)(void *percpu, uint32_t cpu, uint64_t value);
	int (*percpu_read)(void *percpu, uint32_t cpu, uint64_t *value_out);

	int (*rcu_create)(void **rcu_out);
	void (*rcu_destroy)(void *rcu);
	int (*rcu_read_lock)(void *rcu);
	int (*rcu_read_unlock)(void *rcu);
	int (*rcu_synchronize)(void *rcu);

	int (*monotonic_time_ns)(uint64_t *time_out);

	int (*lifecycle_record)(uint32_t node_id, uint32_t phase);
};

typedef int (*kobox_fixture_module_run_fn)(uint64_t *result_out);
typedef int (*kobox_fixture_lifecycle_snapshot_fn)(uint32_t *records,
						  size_t capacity,
						  size_t *count_out);

extern const struct kobox_fixture_core_ops kobox_fixture_core_operations;
int kobox_fixture_core_init(const struct kobox_module_context *context);
int kobox_fixture_core_quiesce(const struct kobox_module_context *context);
int kobox_fixture_core_cleanup(const struct kobox_module_context *context);
int kobox_fixture_lifecycle_snapshot(uint32_t *records, size_t capacity,
				     size_t *count_out);

int kobox_fixture_provider_add(uint64_t value, uint64_t *total_out);
int kobox_fixture_provider_init(const struct kobox_module_context *context);
int kobox_fixture_provider_quiesce(const struct kobox_module_context *context);
int kobox_fixture_provider_cleanup(const struct kobox_module_context *context);

int kobox_fixture_consumer_init(const struct kobox_module_context *context);
int kobox_fixture_consumer_run(uint64_t *result_out);
int kobox_fixture_consumer_quiesce(const struct kobox_module_context *context);
int kobox_fixture_consumer_cleanup(const struct kobox_module_context *context);

#endif
