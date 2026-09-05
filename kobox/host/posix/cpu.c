/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>
#include <limits.h>
#include <ucontext.h>

static _Atomic(struct kobox_posix_cpu *)
	cpu_signal_slots[KOBOX_POSIX_MAX_LOGICAL_CPUS];
static _Thread_local _Atomic(struct kobox_posix_cpu *) active_cpu;
static _Thread_local sigset_t saved_signal_mask;
static _Thread_local bool saved_signal_mask_valid;
static _Thread_local bool dispatching;

int kobox_posix_notifications_save(uint64_t *mask_out)
{
	sigset_t notifications;
	sigset_t previous;
	unsigned int cpu;
	int status;

	if (!mask_out)
		return EINVAL;
	sigemptyset(&notifications);
	for (cpu = 0; cpu < KOBOX_POSIX_MAX_LOGICAL_CPUS; cpu++)
		sigaddset(&notifications, SIGRTMIN + (int)cpu);
	status = pthread_sigmask(SIG_BLOCK, &notifications, &previous);
	if (status)
		return status;
	*mask_out = 0;
	for (cpu = 0; cpu < KOBOX_POSIX_MAX_LOGICAL_CPUS; cpu++) {
		if (sigismember(&previous, SIGRTMIN + (int)cpu))
			*mask_out |= UINT64_C(1) << cpu;
	}
	return 0;
}

int kobox_posix_notifications_restore(uint64_t mask)
{
	sigset_t unblocked;
	unsigned int cpu;

	if (mask >> KOBOX_POSIX_MAX_LOGICAL_CPUS)
		return EINVAL;
	sigemptyset(&unblocked);
	for (cpu = 0; cpu < KOBOX_POSIX_MAX_LOGICAL_CPUS; cpu++) {
		if (!(mask & (UINT64_C(1) << cpu)))
			sigaddset(&unblocked, SIGRTMIN + (int)cpu);
	}
	return pthread_sigmask(SIG_UNBLOCK, &unblocked, NULL);
}

static void dispatch_pending(struct kobox_posix_cpu *cpu)
{
	unsigned int index;
	uint64_t mask;
	bool delivered;

	if (dispatching || atomic_load_explicit(
		    &cpu->irq_disable_depth, memory_order_acquire) != 0)
		return;
	if (kobox_posix_notifications_save(&mask))
		__builtin_trap();
	dispatching = true;
	do {
		delivered = false;
		cpu = atomic_load_explicit(&active_cpu, memory_order_acquire);
		if (!cpu || atomic_load_explicit(
			    &cpu->irq_disable_depth, memory_order_acquire))
			break;
		for (index = 0; index < KOBOX_POSIX_NOTIFICATION_COUNT; index++) {
			uint64_t count = atomic_exchange_explicit(
				&cpu->pending[index], 0, memory_order_acq_rel);

			if (count) {
				cpu->notification(cpu->notification_context,
					cpu->logical_cpu,
					(enum kobox_posix_notification)index, count);
				delivered = true;
				/* Recheck ownership and IRQ state after a possible switch. */
				break;
			}
		}
	} while (delivered);
	dispatching = false;
	if (kobox_posix_notifications_restore(mask))
		__builtin_trap();
}

static void notification_handler(int signal_number, siginfo_t *info, void *arg)
{
	unsigned int index;
	struct kobox_posix_cpu *cpu;
	ucontext_t *context = arg;
	int interrupted_errno = errno;

	(void)info;

	for (index = 0; index < KOBOX_POSIX_MAX_LOGICAL_CPUS; index++) {
		cpu = atomic_load_explicit(
			&cpu_signal_slots[index], memory_order_acquire);
		if (cpu && cpu->signal_number == signal_number) {
			if (atomic_load_explicit(
				    &active_cpu, memory_order_acquire) == cpu)
				dispatch_pending(cpu);
			/* A task may return from the IPI on a different logical CPU. */
			if (atomic_load_explicit(&active_cpu, memory_order_acquire)) {
				for (index = 0; index < KOBOX_POSIX_MAX_LOGICAL_CPUS;
				     index++)
					sigdelset(&context->uc_sigmask,
						  SIGRTMIN + (int)index);
			}
			errno = interrupted_errno;
			return;
		}
	}
	errno = interrupted_errno;
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
	status = pthread_mutex_init(&cpu->owner_lock, NULL);
	if (status)
		return status;
	status = pthread_cond_init(&cpu->execution_condition, NULL);
	if (status) {
		pthread_mutex_destroy(&cpu->owner_lock);
		return status;
	}
	cpu->logical_cpu = logical_cpu;
	cpu->signal_number = SIGRTMIN + (int)logical_cpu;
	cpu->notification = notification;
	cpu->notification_context = context;
	cpu->handoff_released = true;
	cpu->owner_valid = false;
	cpu->accepting_notifications = true;
	atomic_init(&cpu->irq_disable_depth, 0);
	atomic_init(&cpu->notification_sequence, 0);
	if (!atomic_is_lock_free(&cpu->irq_disable_depth) ||
	    !atomic_is_lock_free(&cpu->notification_sequence) ||
	    !atomic_is_lock_free(&active_cpu) ||
	    !atomic_is_lock_free(&cpu_signal_slots[logical_cpu])) {
		pthread_cond_destroy(&cpu->execution_condition);
		pthread_mutex_destroy(&cpu->owner_lock);
		return ENOTSUP;
	}
	for (index = 0; index < KOBOX_POSIX_NOTIFICATION_COUNT; index++) {
		atomic_init(&cpu->pending[index], 0);
		if (!atomic_is_lock_free(&cpu->pending[index])) {
			pthread_cond_destroy(&cpu->execution_condition);
			pthread_mutex_destroy(&cpu->owner_lock);
			return ENOTSUP;
		}
	}
	index = logical_cpu;
	if (!atomic_compare_exchange_strong_explicit(
		    &cpu_signal_slots[index], &expected, cpu,
		    memory_order_acq_rel, memory_order_acquire)) {
		pthread_cond_destroy(&cpu->execution_condition);
		pthread_mutex_destroy(&cpu->owner_lock);
		return EBUSY;
	}
	action.sa_sigaction = notification_handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, cpu->signal_number);
	if (sigaction(cpu->signal_number, &action, &cpu->previous_action) != 0) {
		status = errno;
		atomic_store_explicit(
			&cpu_signal_slots[index], NULL, memory_order_release);
		pthread_cond_destroy(&cpu->execution_condition);
		pthread_mutex_destroy(&cpu->owner_lock);
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
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status)
		return status;
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
		return status;
	}
	status = pthread_cond_destroy(&cpu->execution_condition);
	if (status)
		return status;
	status = pthread_mutex_destroy(&cpu->owner_lock);
	if (status)
		return status;
	cpu->initialized = false;
	return 0;
}

static int cpu_enter(
	struct kobox_posix_cpu *cpu,
	struct kobox_posix_task *task)
{
	sigset_t signal_set;
	bool acquired_owner = false;
	unsigned int cpu_index;
	int status;

	if (!cpu || !cpu->initialized || atomic_load_explicit(
		    &active_cpu, memory_order_acquire))
		return EINVAL;
	sigemptyset(&signal_set);
	for (cpu_index = 0; cpu_index < KOBOX_POSIX_MAX_LOGICAL_CPUS; cpu_index++)
		sigaddset(&signal_set, SIGRTMIN + (int)cpu_index);
	status = pthread_sigmask(SIG_BLOCK, &signal_set, &saved_signal_mask);
	if (status)
		return status;
	saved_signal_mask_valid = true;
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status)
		goto restore_mask;
	while (cpu->owner_valid &&
	       (!pthread_equal(cpu->owner, pthread_self()) ||
		!cpu->handoff_released)) {
		status = pthread_cond_wait(
			&cpu->execution_condition, &cpu->owner_lock);
		if (status) {
			pthread_mutex_unlock(&cpu->owner_lock);
			goto restore_mask;
		}
	}
	if (!cpu->owner_valid) {
		cpu->owner = pthread_self();
		cpu->owner_valid = true;
		cpu->owner_task = task;
		acquired_owner = true;
	}
	if (!acquired_owner && task && cpu->owner_task != task) {
		pthread_mutex_unlock(&cpu->owner_lock);
		status = EPERM;
		goto restore_mask;
	}
	atomic_store_explicit(&active_cpu, cpu, memory_order_release);
	pthread_mutex_unlock(&cpu->owner_lock);
	status = pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL);
	if (status) {
		pthread_mutex_lock(&cpu->owner_lock);
		if (cpu->owner_valid && pthread_equal(cpu->owner, pthread_self()))
			cpu->owner_valid = false;
		cpu->owner_task = NULL;
		atomic_store_explicit(&active_cpu, NULL, memory_order_release);
		pthread_cond_broadcast(&cpu->execution_condition);
		pthread_mutex_unlock(&cpu->owner_lock);
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
				cpu->owner_task = NULL;
				atomic_store_explicit(
					&active_cpu, NULL, memory_order_release);
				pthread_cond_broadcast(
					&cpu->execution_condition);
				pthread_mutex_unlock(&cpu->owner_lock);
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

int kobox_posix_cpu_enter(struct kobox_posix_cpu *cpu)
{
	return cpu_enter(cpu, NULL);
}

int kobox_posix_cpu_enter_task(
	struct kobox_posix_cpu *cpu,
	struct kobox_posix_task *task)
{
	if (!task || !task->initialized ||
	    !pthread_equal(task->thread.native, pthread_self()))
		return EINVAL;
	return cpu_enter(cpu, task);
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
	cpu->owner_task = NULL;
	atomic_store_explicit(&active_cpu, NULL, memory_order_release);
	pthread_cond_broadcast(&cpu->execution_condition);
	pthread_mutex_unlock(&cpu->owner_lock);
	status = pthread_sigmask(SIG_SETMASK, &saved_signal_mask, NULL);
	saved_signal_mask_valid = false;
	return status;
}

int kobox_posix_cpu_switch(
	struct kobox_posix_cpu *cpu,
	struct kobox_posix_task *previous,
	struct kobox_posix_task *next,
	bool exiting)
{
	sigset_t signal_set;
	int status;

	if (!cpu || !previous || !next || previous == next ||
	    !previous->initialized || !previous->thread.started ||
	    !next->initialized || !next->thread.started ||
	    !pthread_equal(previous->thread.native, pthread_self()) ||
	    atomic_load_explicit(&active_cpu, memory_order_acquire) != cpu ||
	    !saved_signal_mask_valid)
		return EINVAL;
	sigemptyset(&signal_set);
	sigaddset(&signal_set, cpu->signal_number);
	status = pthread_sigmask(SIG_BLOCK, &signal_set, NULL);
	if (status)
		return status;
	status = pthread_mutex_lock(&previous->dispatch.lock);
	if (status)
		goto restore_active_mask;
	if (previous->dispatch.count) {
		status = EBUSY;
		goto unlock_dispatch;
	}
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status)
		goto unlock_dispatch;
	if (!cpu->owner_valid || !pthread_equal(cpu->owner, pthread_self()) ||
	    cpu->owner_task != previous) {
		status = EPERM;
		goto unlock_owner;
	}
	cpu->handoff_released = false;
	cpu->owner = next->thread.native;
	cpu->owner_task = next;
	status = kobox_posix_task_wake(next);
	if (status) {
		cpu->owner = previous->thread.native;
		cpu->owner_task = previous;
		cpu->handoff_released = true;
		goto unlock_owner;
	}
	atomic_store_explicit(&active_cpu, NULL, memory_order_release);
	cpu->handoff_released = true;
	pthread_cond_broadcast(&cpu->execution_condition);
	pthread_mutex_unlock(&cpu->owner_lock);
	status = pthread_sigmask(SIG_SETMASK, &saved_signal_mask, NULL);
	saved_signal_mask_valid = false;
	if (status) {
		pthread_mutex_unlock(&previous->dispatch.lock);
		return status;
	}
	if (exiting) {
		pthread_mutex_unlock(&previous->dispatch.lock);
		kobox_posix_task_exit();
	}
	while (!previous->dispatch.count) {
		status = pthread_cond_wait(
			&previous->dispatch.condition,
			&previous->dispatch.lock);
		if (status) {
			pthread_mutex_unlock(&previous->dispatch.lock);
			return status;
		}
	}
	previous->dispatch.count--;
	return pthread_mutex_unlock(&previous->dispatch.lock);

unlock_owner:
	pthread_mutex_unlock(&cpu->owner_lock);
unlock_dispatch:
	pthread_mutex_unlock(&previous->dispatch.lock);
restore_active_mask:
	pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL);
	return status;
}

int kobox_posix_cpu_wait(
	struct kobox_posix_cpu *cpu,
	uint64_t observed_sequence,
	uint64_t *sequence_out)
{
	int status;
	int restore_status;
	uint64_t mask;
	unsigned int index;

	if (!cpu || !sequence_out || atomic_load_explicit(
		    &active_cpu, memory_order_acquire) != cpu)
		return EINVAL;
	status = kobox_posix_notifications_save(&mask);
	if (status)
		return status;
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status) {
		(void)kobox_posix_notifications_restore(mask);
		return status;
	}
	while (atomic_load_explicit(
		       &cpu->notification_sequence, memory_order_acquire) ==
	       observed_sequence) {
		/*
		 * The caller can observe the new sequence before its signal is
		 * delivered. Never sleep over an interrupt already pending when
		 * we mask signals to enter pthread_cond_wait().
		 */
		for (index = 0; index < KOBOX_POSIX_NOTIFICATION_COUNT; index++) {
			if (atomic_load_explicit(&cpu->pending[index],
						 memory_order_acquire))
				break;
		}
		if (index != KOBOX_POSIX_NOTIFICATION_COUNT)
			break;
		status = pthread_cond_wait(
			&cpu->execution_condition, &cpu->owner_lock);
		if (status)
			break;
	}
	if (!status)
		*sequence_out = atomic_load_explicit(
			&cpu->notification_sequence, memory_order_acquire);
	pthread_mutex_unlock(&cpu->owner_lock);
	restore_status = kobox_posix_notifications_restore(mask);
	return status ? status : restore_status;
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
	int status;
	int restore_status;
	int unlock_status;
	uint64_t mask;

	if (!cpu || !cpu->initialized || notification < 0 ||
	    notification >= KOBOX_POSIX_NOTIFICATION_COUNT)
		return EINVAL;
	status = kobox_posix_notifications_save(&mask);
	if (status)
		return status;
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status)
		goto restore_mask;
	if (!cpu->accepting_notifications) {
		pthread_mutex_unlock(&cpu->owner_lock);
		status = ECANCELED;
		goto restore_mask;
	}
	status = increment_pending(&cpu->pending[notification]);
	if (status) {
		pthread_mutex_unlock(&cpu->owner_lock);
		goto restore_mask;
	}
	status = increment_pending(&cpu->notification_sequence);
	if (status) {
		(void)atomic_fetch_sub_explicit(
			&cpu->pending[notification], 1, memory_order_release);
		pthread_mutex_unlock(&cpu->owner_lock);
		goto restore_mask;
	}
	if (cpu->owner_valid) {
		pthread_cond_broadcast(&cpu->execution_condition);
		/* Ownership must keep pthread_t alive until delivery is issued. */
		status = pthread_kill(cpu->owner, cpu->signal_number);
	}
	unlock_status = pthread_mutex_unlock(&cpu->owner_lock);
	if (!status)
		status = unlock_status;
restore_mask:
	restore_status = kobox_posix_notifications_restore(mask);
	return status ? status : restore_status;
}

bool kobox_posix_cpu_irq_disabled(const struct kobox_posix_cpu *cpu)
{
	return cpu && cpu->initialized && atomic_load_explicit(
		&cpu->irq_disable_depth, memory_order_acquire) != 0;
}

uint64_t kobox_posix_cpu_notification_sequence(
	const struct kobox_posix_cpu *cpu)
{
	if (!cpu || !cpu->initialized)
		return 0;
	return atomic_load_explicit(
		&cpu->notification_sequence, memory_order_acquire);
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
