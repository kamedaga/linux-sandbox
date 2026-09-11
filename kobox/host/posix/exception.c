// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "exception.h"
#include "host.h"

#include <errno.h>
#include <signal.h>
#include "../../arch/x86_64/linux_signal.h"
#include <unistd.h>

static const int exception_signals[] = {
	SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP,
};
static _Atomic(kobox_linux_exception_fn) exception_dispatch;
static struct sigaction previous_actions[sizeof(exception_signals) /
					 sizeof(exception_signals[0])];
static unsigned int installed_count;
static _Thread_local volatile sig_atomic_t exception_active;

static void exception_handler(int number, siginfo_t *info, void *argument)
{
	struct kobox_linux_exception_frame frame = {.size = sizeof(frame)};
	kobox_linux_exception_fn dispatch;
	int saved_errno = errno;
	unsigned int index;

	dispatch = atomic_load_explicit(&exception_dispatch, memory_order_acquire);
	if (!dispatch || exception_active || info->si_code <= 0 ||
	    kobox_posix_current_cpu(&frame.cpu))
		goto fatal;
	exception_active = 1;
	kobox_x86_linux_exception_import(&frame, argument,
					 (uintptr_t)info->si_addr);
	if (dispatch(&frame) != KOBOX_EXCEPTION_RESUME)
		goto fatal;
	kobox_x86_linux_exception_export(&frame, argument);
	exception_active = 0;
	errno = saved_errno;
	return;
fatal:
	/* Preserve a caller-installed diagnostic, but never resume a fault
	 * merely because that diagnostic returned or ignored the signal.
	 */
	for (index = 0; index < installed_count; index++) {
		struct sigaction *old = &previous_actions[index];

		if (exception_signals[index] != number ||
		    old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN)
			continue;
		if (old->sa_flags & SA_SIGINFO)
			old->sa_sigaction(number, info, argument);
		else
			old->sa_handler(number);
		break;
	}
	_exit(128 + number);
}

int kobox_posix_exceptions_remove(void)
{
	while (installed_count) {
		unsigned int index = installed_count - 1;

		if (sigaction(exception_signals[index],
			      &previous_actions[index], NULL))
			return errno;
		installed_count--;
	}
	atomic_store_explicit(&exception_dispatch, NULL, memory_order_release);
	return 0;
}

int kobox_posix_exceptions_install(kobox_linux_exception_fn dispatch)
{
	struct sigaction action = {.sa_sigaction = exception_handler,
				   .sa_flags = SA_SIGINFO};
	unsigned int index;
	int status;

	if (!dispatch)
		return EINVAL;
	if (atomic_load_explicit(&exception_dispatch, memory_order_acquire))
		return EBUSY;
	if (!atomic_is_lock_free(&exception_dispatch))
		return ENOTSUP;
	sigemptyset(&action.sa_mask);
	for (index = 0; index < KOBOX_POSIX_MAX_LOGICAL_CPUS; index++)
		sigaddset(&action.sa_mask, SIGRTMIN + (int)index);
	atomic_store_explicit(&exception_dispatch, dispatch, memory_order_release);
	for (index = 0; index < sizeof(exception_signals) /
			      sizeof(exception_signals[0]); index++) {
		if (sigaction(exception_signals[index], &action,
			      &previous_actions[index])) {
			status = errno;
			(void)kobox_posix_exceptions_remove();
			return status;
		}
		installed_count++;
	}
	return 0;
}
