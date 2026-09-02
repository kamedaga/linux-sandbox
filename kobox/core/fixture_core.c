// SPDX-License-Identifier: GPL-2.0-only

#include "../fixture/fixture.h"
#include "../host/host.h"

#include <stdatomic.h>
#include <stdint.h>

struct kobox_fixture_percpu {
	uint32_t cpu_count;
	atomic_uint_least64_t values[];
};

struct kobox_fixture_rcu {
	struct kobox_host_mutex *lock;
	struct kobox_host_event *quiescent;
	uint64_t readers;
};

struct kobox_fixture_spin {
	atomic_flag locked;
};

static int fixture_mutex_create(void **mutex_out)
{
	return kobox_host_mutex_create((struct kobox_host_mutex **)mutex_out);
}

static void fixture_mutex_destroy(void *mutex)
{
	kobox_host_mutex_destroy(mutex);
}

static int fixture_mutex_lock(void *mutex)
{
	return kobox_host_mutex_lock(mutex);
}

static int fixture_mutex_unlock(void *mutex)
{
	return kobox_host_mutex_unlock(mutex);
}

static int fixture_spin_create(void **spin_out)
{
	struct kobox_fixture_spin *spin;

	if (!spin_out)
		return -1;
	*spin_out = NULL;
	spin = kobox_host_allocate(sizeof(*spin));
	if (!spin)
		return -1;
	atomic_flag_clear_explicit(&spin->locked, memory_order_relaxed);
	*spin_out = spin;
	return 0;
}

static void fixture_spin_destroy(void *spin)
{
	if (spin)
		kobox_host_deallocate(spin, sizeof(struct kobox_fixture_spin));
}

static int fixture_spin_lock(void *opaque_spin)
{
	struct kobox_fixture_spin *spin = opaque_spin;

	if (!spin)
		return -1;
	while (atomic_flag_test_and_set_explicit(&spin->locked,
						 memory_order_acquire))
		;
	return 0;
}

static int fixture_spin_unlock(void *opaque_spin)
{
	struct kobox_fixture_spin *spin = opaque_spin;

	if (!spin)
		return -1;
	atomic_flag_clear_explicit(&spin->locked, memory_order_release);
	return 0;
}

static int fixture_event_create(void **event_out, int signaled)
{
	return kobox_host_event_create((struct kobox_host_event **)event_out,
				       signaled);
}

static void fixture_event_destroy(void *event)
{
	kobox_host_event_destroy(event);
}

static int fixture_event_wait(void *event)
{
	return kobox_host_event_wait(event);
}

static int fixture_event_signal(void *event)
{
	return kobox_host_event_signal(event);
}

static int fixture_event_reset(void *event)
{
	return kobox_host_event_reset(event);
}

static int fixture_thread_create(void **thread_out,
				 kobox_fixture_thread_fn function, void *argument,
				 uint32_t logical_cpu)
{
	return kobox_host_thread_create((struct kobox_host_thread **)thread_out,
					function, argument, logical_cpu);
}

static int fixture_thread_join(void *thread, int *result_out)
{
	return kobox_host_thread_join(thread, result_out);
}

static int fixture_percpu_create(void **percpu_out, uint32_t cpu_count)
{
	struct kobox_fixture_percpu *percpu;
	size_t size;

	if (!percpu_out || !cpu_count)
		return -1;
	*percpu_out = NULL;
	size = sizeof(*percpu) + cpu_count * sizeof(percpu->values[0]);
	if (size < sizeof(*percpu) ||
	    (size - sizeof(*percpu)) / sizeof(percpu->values[0]) != cpu_count)
		return -1;
	percpu = kobox_host_allocate(size);
	if (!percpu)
		return -1;
	percpu->cpu_count = cpu_count;
	*percpu_out = percpu;
	return 0;
}

static void fixture_percpu_destroy(void *opaque_percpu)
{
	struct kobox_fixture_percpu *percpu = opaque_percpu;
	size_t size;

	if (!percpu)
		return;
	size = sizeof(*percpu) +
	       (size_t)percpu->cpu_count * sizeof(percpu->values[0]);
	kobox_host_deallocate(percpu, size);
}

static int fixture_percpu_add(void *opaque_percpu, uint32_t cpu,
			      uint64_t value)
{
	struct kobox_fixture_percpu *percpu = opaque_percpu;

	if (!percpu || cpu >= percpu->cpu_count)
		return -1;
	atomic_fetch_add_explicit(&percpu->values[cpu], value,
				  memory_order_relaxed);
	return 0;
}

static int fixture_percpu_read(void *opaque_percpu, uint32_t cpu,
			       uint64_t *value_out)
{
	struct kobox_fixture_percpu *percpu = opaque_percpu;

	if (!percpu || cpu >= percpu->cpu_count || !value_out)
		return -1;
	*value_out = atomic_load_explicit(&percpu->values[cpu],
					  memory_order_acquire);
	return 0;
}

static int fixture_rcu_create(void **rcu_out)
{
	struct kobox_fixture_rcu *rcu;

	if (!rcu_out)
		return -1;
	*rcu_out = NULL;
	rcu = kobox_host_allocate(sizeof(*rcu));
	if (!rcu)
		return -1;
	if (kobox_host_mutex_create(&rcu->lock) ||
	    kobox_host_event_create(&rcu->quiescent, 1)) {
		kobox_host_event_destroy(rcu->quiescent);
		kobox_host_mutex_destroy(rcu->lock);
		kobox_host_deallocate(rcu, sizeof(*rcu));
		return -1;
	}
	*rcu_out = rcu;
	return 0;
}

static void fixture_rcu_destroy(void *opaque_rcu)
{
	struct kobox_fixture_rcu *rcu = opaque_rcu;

	if (!rcu)
		return;
	kobox_host_event_destroy(rcu->quiescent);
	kobox_host_mutex_destroy(rcu->lock);
	kobox_host_deallocate(rcu, sizeof(*rcu));
}

static int fixture_rcu_read_lock(void *opaque_rcu)
{
	struct kobox_fixture_rcu *rcu = opaque_rcu;

	if (!rcu || kobox_host_mutex_lock(rcu->lock))
		return -1;
	if (!rcu->readers && kobox_host_event_reset(rcu->quiescent)) {
		kobox_host_mutex_unlock(rcu->lock);
		return -1;
	}
	if (rcu->readers == UINT64_MAX) {
		kobox_host_mutex_unlock(rcu->lock);
		return -1;
	}
	rcu->readers++;
	return kobox_host_mutex_unlock(rcu->lock);
}

static int fixture_rcu_read_unlock(void *opaque_rcu)
{
	struct kobox_fixture_rcu *rcu = opaque_rcu;
	int status = 0;

	if (!rcu || kobox_host_mutex_lock(rcu->lock))
		return -1;
	if (!rcu->readers)
		status = -1;
	else if (!--rcu->readers)
		status = kobox_host_event_signal(rcu->quiescent);
	if (kobox_host_mutex_unlock(rcu->lock))
		return -1;
	return status;
}

static int fixture_rcu_synchronize(void *opaque_rcu)
{
	struct kobox_fixture_rcu *rcu = opaque_rcu;

	return rcu ? kobox_host_event_wait(rcu->quiescent) : -1;
}

static const struct kobox_fixture_core_ops fixture_core_ops = {
	.size = sizeof(fixture_core_ops),
	.identity = {
		KOBOX_FIXTURE_CORE_IDENTITY_0,
		KOBOX_FIXTURE_CORE_IDENTITY_1,
		KOBOX_FIXTURE_CORE_IDENTITY_2,
		KOBOX_FIXTURE_CORE_IDENTITY_3,
	},
	.allocate = kobox_host_allocate,
	.deallocate = kobox_host_deallocate,
	.mutex_create = fixture_mutex_create,
	.mutex_destroy = fixture_mutex_destroy,
	.mutex_lock = fixture_mutex_lock,
	.mutex_unlock = fixture_mutex_unlock,
	.spin_create = fixture_spin_create,
	.spin_destroy = fixture_spin_destroy,
	.spin_lock = fixture_spin_lock,
	.spin_unlock = fixture_spin_unlock,
	.event_create = fixture_event_create,
	.event_destroy = fixture_event_destroy,
	.event_wait = fixture_event_wait,
	.event_signal = fixture_event_signal,
	.event_reset = fixture_event_reset,
	.thread_create = fixture_thread_create,
	.thread_join = fixture_thread_join,
	.current_cpu = kobox_host_current_cpu,
	.percpu_create = fixture_percpu_create,
	.percpu_destroy = fixture_percpu_destroy,
	.percpu_add = fixture_percpu_add,
	.percpu_read = fixture_percpu_read,
	.rcu_create = fixture_rcu_create,
	.rcu_destroy = fixture_rcu_destroy,
	.rcu_read_lock = fixture_rcu_read_lock,
	.rcu_read_unlock = fixture_rcu_read_unlock,
	.rcu_synchronize = fixture_rcu_synchronize,
	.monotonic_time_ns = kobox_host_monotonic_time_ns,
};

__attribute__((visibility("default")))
const struct kobox_fixture_core_ops *kobox_fixture_core_get_ops(void)
{
	return &fixture_core_ops;
}
