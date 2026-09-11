/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>
#include <ucontext.h>

static _Atomic(struct kobox_posix_cpu *)
	cpu_signal_slots[KOBOX_POSIX_MAX_LOGICAL_CPUS];
static _Thread_local _Atomic(struct kobox_posix_cpu *) active_cpu;
static _Thread_local unsigned char thread_identity;
static _Thread_local sigset_t saved_signal_mask;
static _Thread_local bool saved_signal_mask_valid;

static int machine_status(enum kobox_machine_result result)
{
	switch (result) {
	case KOBOX_MACHINE_OK:
		return 0;
	case KOBOX_MACHINE_INVALID:
		return EINVAL;
	case KOBOX_MACHINE_BUSY:
		return EBUSY;
	case KOBOX_MACHINE_OVERFLOW:
		return EOVERFLOW;
	case KOBOX_MACHINE_CLOSED:
		return ECANCELED;
	case KOBOX_MACHINE_NOT_LOCK_FREE:
		return ENOTSUP;
	}
	__builtin_trap();
}

int kobox_posix_current_cpu(uint32_t *cpu_out)
{
	struct kobox_posix_cpu *cpu;

	if (!cpu_out)
		return EINVAL;
	cpu = atomic_load_explicit(&active_cpu, memory_order_acquire);
	if (!cpu)
		return ENXIO;
	*cpu_out = cpu->logical_cpu;
	return 0;
}

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

static void dispatch_pending(void)
{
	struct kobox_posix_cpu *cpu;
	enum kobox_machine_notification notification;
	uint64_t mask, callback_mask, count;

	if (kobox_posix_notifications_save(&mask))
		__builtin_trap();
	for (;;) {
		cpu = atomic_load_explicit(&active_cpu, memory_order_acquire);
		if (cpu && atomic_load_explicit(&cpu->domain.stop_requested,
					       memory_order_acquire)) {
			sigset_t blocked;

			/* Terminal stop is independent of logical IRQ masking.
			 * Do not enter Linux or transfer execution ownership.
			 */
			sigfillset(&blocked);
			if (pthread_sigmask(SIG_BLOCK, &blocked, NULL))
				__builtin_trap();
			atomic_store_explicit(&cpu->domain.stopped, true,
					      memory_order_release);
			for (;;)
				sigsuspend(&blocked);
		}
		if (!cpu || !kobox_machine_domain_irq_take(
				&cpu->domain, &notification, &count))
			break;
		/* Linux may enable logical IRQs and accept nested upcalls. */
		if (kobox_posix_notifications_restore(0))
			__builtin_trap();
		cpu->notification(cpu->notification_context, cpu->logical_cpu,
			(enum kobox_posix_notification)notification, count);
		if (kobox_posix_notifications_save(&callback_mask))
			__builtin_trap();
		/* Interrupt return can follow a task migration. */
		cpu = atomic_load_explicit(&active_cpu, memory_order_acquire);
		if (!cpu || !kobox_machine_domain_irq_return(&cpu->domain))
			__builtin_trap();
	}
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
				dispatch_pending();
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
	status = machine_status(kobox_machine_domain_init(&cpu->domain));
	if (status || !atomic_is_lock_free(&active_cpu) ||
	    !atomic_is_lock_free(&cpu_signal_slots[logical_cpu])) {
		pthread_cond_destroy(&cpu->execution_condition);
		pthread_mutex_destroy(&cpu->owner_lock);
		return status ? status : ENOTSUP;
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
	int status;

	if (!cpu || !cpu->initialized)
		return EINVAL;
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status)
		return status;
	status = machine_status(kobox_machine_domain_close(&cpu->domain));
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
		kobox_machine_domain_reopen(&cpu->domain);
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
	void *identity = task ? (void *)task : &thread_identity;
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
	while (!kobox_machine_domain_can_enter(&cpu->domain, identity)) {
		status = pthread_cond_wait(
			&cpu->execution_condition, &cpu->owner_lock);
		if (status) {
			pthread_mutex_unlock(&cpu->owner_lock);
			goto restore_mask;
		}
	}
	status = machine_status(kobox_machine_domain_enter(&cpu->domain,
							  identity));
	if (status) {
		pthread_mutex_unlock(&cpu->owner_lock);
		goto restore_mask;
	}
	cpu->owner = pthread_self();
	atomic_store_explicit(&active_cpu, cpu, memory_order_release);
	pthread_mutex_unlock(&cpu->owner_lock);
	status = pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL);
	if (status) {
		pthread_mutex_lock(&cpu->owner_lock);
		kobox_machine_domain_release(&cpu->domain);
		atomic_store_explicit(&active_cpu, NULL, memory_order_release);
		pthread_cond_broadcast(&cpu->execution_condition);
		pthread_mutex_unlock(&cpu->owner_lock);
		goto restore_mask;
	}
	if (kobox_machine_domain_pending(&cpu->domain)) {
		status = pthread_kill(pthread_self(), cpu->signal_number);
		if (status) {
			pthread_sigmask(SIG_BLOCK, &signal_set, NULL);
			pthread_mutex_lock(&cpu->owner_lock);
			kobox_machine_domain_release(&cpu->domain);
			atomic_store_explicit(&active_cpu, NULL, memory_order_release);
			pthread_cond_broadcast(&cpu->execution_condition);
			pthread_mutex_unlock(&cpu->owner_lock);
			goto restore_mask;
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
		    &cpu->domain.irq_depth, memory_order_acquire) != 0)
		return EBUSY;
	sigemptyset(&signal_set);
	sigaddset(&signal_set, cpu->signal_number);
	status = pthread_sigmask(SIG_BLOCK, &signal_set, NULL);
	if (status)
		return status;
	status = pthread_mutex_lock(&cpu->owner_lock);
	if (status)
		return status;
	kobox_machine_domain_release(&cpu->domain);
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
	status = machine_status(kobox_machine_domain_handoff(
		&cpu->domain, previous, next));
	if (status) {
		status = EPERM;
		goto unlock_owner;
	}
	cpu->owner = next->thread.native;
	status = kobox_posix_task_wake(next);
	if (status) {
		cpu->owner = previous->thread.native;
		kobox_machine_domain_handoff_abort(&cpu->domain, previous);
		goto unlock_owner;
	}
	atomic_store_explicit(&active_cpu, NULL, memory_order_release);
	kobox_machine_domain_handoff_finish(&cpu->domain);
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
	while (kobox_machine_domain_should_wait(&cpu->domain,
					      observed_sequence)) {
		status = pthread_cond_wait(
			&cpu->execution_condition, &cpu->owner_lock);
		if (status)
			break;
	}
	if (!status)
		*sequence_out = atomic_load_explicit(
			&cpu->domain.sequence, memory_order_acquire);
	pthread_mutex_unlock(&cpu->owner_lock);
	restore_status = kobox_posix_notifications_restore(mask);
	return status ? status : restore_status;
}

int kobox_posix_cpu_irq_disable(struct kobox_posix_cpu *cpu)
{
	if (!cpu || atomic_load_explicit(
		    &active_cpu, memory_order_acquire) != cpu)
		return EINVAL;
	return machine_status(kobox_machine_domain_irq_disable(&cpu->domain));
}

int kobox_posix_cpu_irq_enable(struct kobox_posix_cpu *cpu)
{
	bool dispatch;
	int status;

	if (!cpu || atomic_load_explicit(
		    &active_cpu, memory_order_acquire) != cpu)
		return EINVAL;
	status = machine_status(kobox_machine_domain_irq_enable(&cpu->domain,
							      &dispatch));
	if (!status && dispatch)
		dispatch_pending();
	return status;
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
	status = machine_status(kobox_machine_domain_notify(&cpu->domain,
			(enum kobox_machine_notification)notification));
	if (status) {
		pthread_mutex_unlock(&cpu->owner_lock);
		goto restore_mask;
	}
	if (cpu->domain.owner) {
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

int kobox_posix_cpu_stop(struct kobox_posix_cpu *cpu)
{
	uint64_t start, now;
	int status;

	if (!cpu || !cpu->initialized ||
	    atomic_load_explicit(&active_cpu, memory_order_acquire) == cpu)
		return EINVAL;
	status = kobox_posix_monotonic_ns(&start);
	if (status)
		return status;
	atomic_store_explicit(&cpu->domain.stop_requested, true, memory_order_release);
	/* Reuse the ownership-safe doorbell, not its guest IRQ dispatch. */
	status = kobox_posix_cpu_notify(cpu, KOBOX_POSIX_NOTIFICATION_IRQ);
	if (status)
		return status;
	while (!atomic_load_explicit(&cpu->domain.stopped, memory_order_acquire)) {
		status = kobox_posix_monotonic_ns(&now);
		if (status)
			return status;
		if (now - start > UINT64_C(2000000000))
			return ETIMEDOUT;
	}
	return 0;
}

bool kobox_posix_cpu_irq_disabled(const struct kobox_posix_cpu *cpu)
{
	return cpu && cpu->initialized && atomic_load_explicit(
		&cpu->domain.irq_depth, memory_order_acquire) != 0;
}

uint64_t kobox_posix_cpu_notification_sequence(
	const struct kobox_posix_cpu *cpu)
{
	if (!cpu || !cpu->initialized)
		return 0;
	return atomic_load_explicit(
		&cpu->domain.sequence, memory_order_acquire);
}

uint64_t kobox_posix_cpu_pending(
	const struct kobox_posix_cpu *cpu,
	enum kobox_posix_notification notification)
{
	if (!cpu || !cpu->initialized || notification < 0 ||
	    notification >= KOBOX_POSIX_NOTIFICATION_COUNT)
		return 0;
	return atomic_load_explicit(
		&cpu->domain.pending[notification], memory_order_acquire);
}
