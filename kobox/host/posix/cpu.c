/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>
#include <limits.h>

static _Atomic(struct kobox_posix_cpu *)
	cpu_signal_slots[KOBOX_POSIX_MAX_LOGICAL_CPUS];
static _Thread_local _Atomic(struct kobox_posix_cpu *) active_cpu;
static _Thread_local sigset_t saved_signal_mask;
static _Thread_local bool saved_signal_mask_valid;

static void dispatch_pending(struct kobox_posix_cpu *cpu)
{
	unsigned int index;

	if (atomic_load_explicit(
		    &cpu->irq_disable_depth, memory_order_acquire) != 0)
		return;
	for (index = 0; index < KOBOX_POSIX_NOTIFICATION_COUNT; index++) {
		uint64_t count = atomic_exchange_explicit(
			&cpu->pending[index], 0, memory_order_acq_rel);

		if (count)
			cpu->notification(
				cpu->notification_context, cpu->logical_cpu,
				(enum kobox_posix_notification)index, count);
	}
}

static void notification_handler(int signal_number)
{
	unsigned int index;
	struct kobox_posix_cpu *cpu;

	for (index = 0; index < KOBOX_POSIX_MAX_LOGICAL_CPUS; index++) {
		cpu = atomic_load_explicit(
			&cpu_signal_slots[index], memory_order_acquire);
		if (cpu && cpu->signal_number == signal_number) {
			if (atomic_load_explicit(
				    &active_cpu, memory_order_acquire) == cpu)
				dispatch_pending(cpu);
			return;
		}
	}
}

static int increment_pending(atomic_uint_fast64_t *pending)
{
	uint_fast64_t current = atomic_load_explicit(
		pending, memory_order_relaxed);

	for (;;) {
		if (current == UINT64_MAX)
			return EOVERFLOW;
		if (atomic_compare_exchange_weak_explicit(
			    pending, &current, current + 1,
			    memory_order_release, memory_order_relaxed))
			return 0;
	}
}

int kobox_posix_cpu_init(
	struct kobox_posix_cpu *cpu,
	uint32_t logical_cpu,
	kobox_posix_notification_fn notification,
	void *context)
{
	struct kobox_posix_cpu *expected = NULL;
	struct sigaction action = {0};
	unsigned int index;
	int status;

	if (!cpu || !notification || cpu->initialized ||
	    logical_cpu >= KOBOX_POSIX_MAX_LOGICAL_CPUS)
		return EINVAL;
	if (SIGRTMIN + (int)logical_cpu > SIGRTMAX)
		return ENOSPC;
	status = pthread_mutex_init(&cpu->execution_lock, NULL);
	if (status)
		return status;
	status = pthread_mutex_init(&cpu->owner_lock, NULL);
	if (status) {
		pthread_mutex_destroy(&cpu->execution_lock);
		return status;
	}
	cpu->logical_cpu = logical_cpu;
	cpu->signal_number = SIGRTMIN + (int)logical_cpu;
	cpu->notification = notification;
	cpu->notification_context = context;
	cpu->owner_valid = false;
	cpu->accepting_notifications = true;
	atomic_init(&cpu->irq_disable_depth, 0);
	if (!atomic_is_lock_free(&cpu->irq_disable_depth) ||
	    !atomic_is_lock_free(&active_cpu) ||
	    !atomic_is_lock_free(&cpu_signal_slots[logical_cpu])) {
		pthread_mutex_destroy(&cpu->owner_lock);
		pthread_mutex_destroy(&cpu->execution_lock);
		return ENOTSUP;
	}
	for (index = 0; index < KOBOX_POSIX_NOTIFICATION_COUNT; index++) {
		atomic_init(&cpu->pending[index], 0);
		if (!atomic_is_lock_free(&cpu->pending[index])) {
			pthread_mutex_destroy(&cpu->owner_lock);
			pthread_mutex_destroy(&cpu->execution_lock);
			return ENOTSUP;
		}
	}
	index = logical_cpu;
	if (!atomic_compare_exchange_strong_explicit(
		    &cpu_signal_slots[index], &expected, cpu,
		    memory_order_acq_rel, memory_order_acquire)) {
		pthread_mutex_destroy(&cpu->owner_lock);
		pthread_mutex_destroy(&cpu->execution_lock);
		return EBUSY;
	}
	action.sa_handler = notification_handler;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, cpu->signal_number);
	if (sigaction(cpu->signal_number, &action, &cpu->previous_action) != 0) {
		status = errno;
		atomic_store_explicit(
			&cpu_signal_slots[index], NULL, memory_order_release);
		pthread_mutex_destroy(&cpu->owner_lock);
		pthread_mutex_destroy(&cpu->execution_lock);
		return status;
	}
	cpu->initialized = true;
	return 0;
}

int kobox_posix_cpu_destroy(struct kobox_posix_cpu *cpu)
{
	unsigned int index;
	int status;

	if (!cpu || !cpu->initialized)
		return EINVAL;
	status = pthread_mutex_trylock(&cpu->execution_lock);
	if (status)
		return status;
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status) {
		pthread_mutex_unlock(&cpu->execution_lock);
		return status;
	}
	if (cpu->owner_valid ||
	    atomic_load_explicit(&cpu->irq_disable_depth, memory_order_acquire))
		status = EBUSY;
	for (index = 0; index < KOBOX_POSIX_NOTIFICATION_COUNT; index++) {
		if (atomic_load_explicit(
			    &cpu->pending[index], memory_order_acquire))
			status = EBUSY;
	}
	if (!status)
		cpu->accepting_notifications = false;
	pthread_mutex_unlock(&cpu->owner_lock);
	if (status) {
		pthread_mutex_unlock(&cpu->execution_lock);
		return status;
	}
	atomic_store_explicit(
		&cpu_signal_slots[cpu->logical_cpu], NULL, memory_order_release);
	if (sigaction(cpu->signal_number, &cpu->previous_action, NULL) != 0) {
		status = errno;
		atomic_store_explicit(
			&cpu_signal_slots[cpu->logical_cpu], cpu,
			memory_order_release);
		pthread_mutex_lock(&cpu->owner_lock);
		cpu->accepting_notifications = true;
		pthread_mutex_unlock(&cpu->owner_lock);
		pthread_mutex_unlock(&cpu->execution_lock);
		return status;
	}
	pthread_mutex_unlock(&cpu->execution_lock);
	status = pthread_mutex_destroy(&cpu->owner_lock);
	if (status)
		return status;
	status = pthread_mutex_destroy(&cpu->execution_lock);
	if (status)
		return status;
	cpu->initialized = false;
	return 0;
}

int kobox_posix_cpu_enter(struct kobox_posix_cpu *cpu)
{
	sigset_t signal_set;
	int status;

	if (!cpu || !cpu->initialized || atomic_load_explicit(
		    &active_cpu, memory_order_acquire))
		return EINVAL;
	sigemptyset(&signal_set);
	sigaddset(&signal_set, cpu->signal_number);
	status = pthread_sigmask(SIG_BLOCK, &signal_set, &saved_signal_mask);
	if (status)
		return status;
	saved_signal_mask_valid = true;
	status = pthread_mutex_lock(&cpu->execution_lock);
	if (status)
		goto restore_mask;
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status) {
		pthread_mutex_unlock(&cpu->execution_lock);
		goto restore_mask;
	}
	cpu->owner = pthread_self();
	cpu->owner_valid = true;
	atomic_store_explicit(&active_cpu, cpu, memory_order_release);
	pthread_mutex_unlock(&cpu->owner_lock);
	status = pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL);
	if (status) {
		pthread_mutex_lock(&cpu->owner_lock);
		cpu->owner_valid = false;
		atomic_store_explicit(&active_cpu, NULL, memory_order_release);
		pthread_mutex_unlock(&cpu->owner_lock);
		pthread_mutex_unlock(&cpu->execution_lock);
		goto restore_mask;
	}
	for (unsigned int index = 0;
	     index < KOBOX_POSIX_NOTIFICATION_COUNT; index++) {
		if (atomic_load_explicit(
			    &cpu->pending[index], memory_order_acquire)) {
			status = pthread_kill(pthread_self(), cpu->signal_number);
			if (status) {
				pthread_sigmask(SIG_BLOCK, &signal_set, NULL);
				pthread_mutex_lock(&cpu->owner_lock);
				cpu->owner_valid = false;
				atomic_store_explicit(
					&active_cpu, NULL, memory_order_release);
				pthread_mutex_unlock(&cpu->owner_lock);
				pthread_mutex_unlock(&cpu->execution_lock);
				goto restore_mask;
			}
			break;
		}
	}
	return 0;

restore_mask:
	pthread_sigmask(SIG_SETMASK, &saved_signal_mask, NULL);
	saved_signal_mask_valid = false;
	return status;
}

int kobox_posix_cpu_leave(struct kobox_posix_cpu *cpu)
{
	sigset_t signal_set;
	int status;

	if (!cpu || atomic_load_explicit(
		    &active_cpu, memory_order_acquire) != cpu ||
	    !saved_signal_mask_valid)
		return EINVAL;
	if (atomic_load_explicit(
		    &cpu->irq_disable_depth, memory_order_acquire) != 0)
		return EBUSY;
	sigemptyset(&signal_set);
	sigaddset(&signal_set, cpu->signal_number);
	status = pthread_sigmask(SIG_BLOCK, &signal_set, NULL);
	if (status)
		return status;
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status)
		return status;
	cpu->owner_valid = false;
	atomic_store_explicit(&active_cpu, NULL, memory_order_release);
	pthread_mutex_unlock(&cpu->owner_lock);
	pthread_mutex_unlock(&cpu->execution_lock);
	status = pthread_sigmask(SIG_SETMASK, &saved_signal_mask, NULL);
	saved_signal_mask_valid = false;
	return status;
}

int kobox_posix_cpu_irq_disable(struct kobox_posix_cpu *cpu)
{
	unsigned int depth;

	if (!cpu || atomic_load_explicit(
		    &active_cpu, memory_order_acquire) != cpu)
		return EINVAL;
	depth = atomic_load_explicit(
		&cpu->irq_disable_depth, memory_order_acquire);
	for (;;) {
		if (depth == UINT_MAX)
			return EOVERFLOW;
		if (atomic_compare_exchange_weak_explicit(
			    &cpu->irq_disable_depth, &depth, depth + 1,
			    memory_order_acq_rel, memory_order_acquire))
			return 0;
	}
}

int kobox_posix_cpu_irq_enable(struct kobox_posix_cpu *cpu)
{
	unsigned int depth;

	if (!cpu || atomic_load_explicit(
		    &active_cpu, memory_order_acquire) != cpu)
		return EINVAL;
	depth = atomic_load_explicit(
		&cpu->irq_disable_depth, memory_order_acquire);
	if (!depth)
		return EINVAL;
	if (atomic_fetch_sub_explicit(
		    &cpu->irq_disable_depth, 1, memory_order_acq_rel) == 1)
		dispatch_pending(cpu);
	return 0;
}

int kobox_posix_cpu_notify(
	struct kobox_posix_cpu *cpu,
	enum kobox_posix_notification notification)
{
	pthread_t owner;
	bool owner_valid;
	int status;

	if (!cpu || !cpu->initialized || notification < 0 ||
	    notification >= KOBOX_POSIX_NOTIFICATION_COUNT)
		return EINVAL;
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status)
		return status;
	if (!cpu->accepting_notifications) {
		pthread_mutex_unlock(&cpu->owner_lock);
		return ECANCELED;
	}
	status = increment_pending(&cpu->pending[notification]);
	if (status) {
		pthread_mutex_unlock(&cpu->owner_lock);
		return status;
	}
	owner = cpu->owner;
	owner_valid = cpu->owner_valid;
	status = pthread_mutex_unlock(&cpu->owner_lock);
	if (status)
		return status;
	if (!owner_valid)
		return 0;
	status = pthread_kill(owner, cpu->signal_number);
	return status == ESRCH ? 0 : status;
}

uint64_t kobox_posix_cpu_pending(
	const struct kobox_posix_cpu *cpu,
	enum kobox_posix_notification notification)
{
	if (!cpu || !cpu->initialized || notification < 0 ||
	    notification >= KOBOX_POSIX_NOTIFICATION_COUNT)
		return 0;
	return atomic_load_explicit(
		&cpu->pending[notification], memory_order_acquire);
}
