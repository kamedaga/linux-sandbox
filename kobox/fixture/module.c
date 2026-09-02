// SPDX-License-Identifier: GPL-2.0-only

#include "fixture.h"

#include <linux/compiler_attributes.h>

static const char fixture_module_license[]
	__section(".modinfo") __used __aligned(1) = "license=GPL";
static const char fixture_module_name[]
	__section(".modinfo") __used __aligned(1) =
		"name=kobox_fixture";
static const char fixture_module_dependency[]
	__section(".modinfo") __used __aligned(1) =
		"depends=fixture_core";
static const char fixture_module_interface[]
	__section(".modinfo") __used __aligned(1) =
		"kobox_interface=dev";

struct fixture_state {
	const struct kobox_fixture_core_ops *ops;
	void *lock;
	void *spin;
	void *ready_event;
	void *release_event;
	void *percpu;
	void *rcu;
	uint32_t ready;
	uint32_t completed;
	uint32_t spin_count;
};

static uint32_t fixture_module_active;

static int fixture_identity_valid(const struct kobox_fixture_core_ops *ops)
{
	return ops && ops->size == sizeof(*ops) &&
	       ops->identity[0] == KOBOX_FIXTURE_CORE_IDENTITY_0 &&
	       ops->identity[1] == KOBOX_FIXTURE_CORE_IDENTITY_1 &&
	       ops->identity[2] == KOBOX_FIXTURE_CORE_IDENTITY_2 &&
	       ops->identity[3] == KOBOX_FIXTURE_CORE_IDENTITY_3;
}

static int fixture_ops_valid(const struct kobox_fixture_core_ops *ops)
{
	return fixture_identity_valid(ops) && ops->allocate && ops->deallocate &&
	       ops->mutex_create && ops->mutex_destroy && ops->mutex_lock &&
	       ops->mutex_unlock && ops->spin_create && ops->spin_destroy &&
	       ops->spin_lock && ops->spin_unlock && ops->event_create &&
	       ops->event_destroy &&
	       ops->event_wait && ops->event_signal && ops->event_reset &&
	       ops->thread_create && ops->thread_join && ops->current_cpu &&
	       ops->percpu_create && ops->percpu_destroy && ops->percpu_add &&
	       ops->percpu_read && ops->rcu_create && ops->rcu_destroy &&
	       ops->rcu_read_lock && ops->rcu_read_unlock &&
	       ops->rcu_synchronize && ops->monotonic_time_ns;
}

static int fixture_worker(void *argument)
{
	struct fixture_state *state = argument;
	uint32_t cpu;

	if (state->ops->rcu_read_lock(state->rcu))
		return -1;
	if (state->ops->mutex_lock(state->lock))
		goto unlock_rcu;
	state->ready++;
	if (state->ready == KOBOX_FIXTURE_CPU_COUNT &&
	    state->ops->event_signal(state->ready_event)) {
		state->ops->mutex_unlock(state->lock);
		goto unlock_rcu;
	}
	if (state->ops->mutex_unlock(state->lock))
		goto unlock_rcu;
	if (state->ops->event_wait(state->release_event))
		goto unlock_rcu;
	cpu = state->ops->current_cpu();
	if (state->ops->percpu_add(state->percpu, cpu, 1))
		goto unlock_rcu;
	if (state->ops->spin_lock(state->spin))
		goto unlock_rcu;
	state->spin_count++;
	if (state->ops->spin_unlock(state->spin))
		goto unlock_rcu;
	if (state->ops->rcu_read_unlock(state->rcu))
		return -1;
	if (state->ops->mutex_lock(state->lock))
		return -1;
	state->completed++;
	return state->ops->mutex_unlock(state->lock);

unlock_rcu:
	state->ops->rcu_read_unlock(state->rcu);
	return -1;
}

__attribute__((visibility("default")))
int kobox_fixture_module_init(void)
{
	const struct kobox_fixture_core_ops *ops = kobox_fixture_core_get_ops();

	if (fixture_module_active || !fixture_ops_valid(ops))
		return -1;
#ifdef KOBOX_FIXTURE_FAIL_INIT
	return -1;
#endif
	fixture_module_active = 1;
	return 0;
}

__attribute__((visibility("default")))
int kobox_fixture_module_run(uint64_t *result_out)
{
	const struct kobox_fixture_core_ops *ops = kobox_fixture_core_get_ops();
	struct fixture_state *state = NULL;
	void *threads[KOBOX_FIXTURE_CPU_COUNT] = { NULL, NULL };
	uint8_t *allocation = NULL;
	uint64_t before;
	uint64_t after;
	uint64_t cpu_value;
	uint32_t created = 0;
	uint32_t joined = 0;
	uint32_t index;
	int thread_result;
	int status = -1;

	if (!result_out || !fixture_module_active || !fixture_ops_valid(ops))
		return -1;
	*result_out = 0;
	state = ops->allocate(sizeof(*state));
	allocation = ops->allocate(256);
	if (!state || !allocation)
		goto out;
	state->ops = ops;
	for (index = 0; index < 256; index++)
		allocation[index] = (uint8_t)(index ^ 0xa5u);
	for (index = 0; index < 256; index++) {
		if (allocation[index] != (uint8_t)(index ^ 0xa5u))
			goto out;
	}
	if (ops->mutex_create(&state->lock) ||
	    ops->spin_create(&state->spin) ||
	    ops->event_create(&state->ready_event, 0) ||
	    ops->event_create(&state->release_event, 0) ||
	    ops->percpu_create(&state->percpu, KOBOX_FIXTURE_CPU_COUNT) ||
	    ops->rcu_create(&state->rcu))
		goto out;
	for (index = 0; index < KOBOX_FIXTURE_CPU_COUNT; index++) {
		if (ops->thread_create(&threads[index], fixture_worker, state, index))
			goto out;
		created++;
	}
	if (ops->event_wait(state->ready_event) ||
	    ops->monotonic_time_ns(&before) ||
	    ops->event_signal(state->release_event) ||
	    ops->rcu_synchronize(state->rcu) ||
	    ops->monotonic_time_ns(&after) || after < before)
		goto out;
	for (index = 0; index < created; index++) {
		if (ops->thread_join(threads[index], &thread_result) || thread_result)
			goto out;
		threads[index] = NULL;
		joined++;
	}
	if (state->ready != KOBOX_FIXTURE_CPU_COUNT ||
	    state->completed != KOBOX_FIXTURE_CPU_COUNT ||
	    state->spin_count != KOBOX_FIXTURE_CPU_COUNT)
		goto out;
	for (index = 0; index < KOBOX_FIXTURE_CPU_COUNT; index++) {
		if (ops->percpu_read(state->percpu, index, &cpu_value) ||
		    cpu_value != 1)
			goto out;
	}
	*result_out = KOBOX_FIXTURE_RESULT;
	status = 0;

out:
	if (state && state->release_event)
		ops->event_signal(state->release_event);
	for (index = joined; index < created; index++) {
		if (threads[index])
			ops->thread_join(threads[index], &thread_result);
	}
	if (state) {
		ops->rcu_destroy(state->rcu);
		ops->percpu_destroy(state->percpu);
		ops->event_destroy(state->release_event);
		ops->event_destroy(state->ready_event);
		ops->spin_destroy(state->spin);
		ops->mutex_destroy(state->lock);
		ops->deallocate(state, sizeof(*state));
	}
	if (allocation)
		ops->deallocate(allocation, 256);
	return status;
}

__attribute__((visibility("default")))
int kobox_fixture_module_quiesce(void)
{
	return fixture_module_active ? 0 : -1;
}

__attribute__((visibility("default")))
int kobox_fixture_module_cleanup(void)
{
	if (!fixture_module_active)
		return -1;
	fixture_module_active = 0;
	return 0;
}
