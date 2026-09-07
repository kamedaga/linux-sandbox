// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "exception.h"
#include "../host/posix/host.h"

#include <errno.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

static const int exception_signals[] = {
	SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP,
};
static const int register_index[KOBOX_EXCEPTION_REGISTERS] = {
	[KOBOX_EXCEPTION_AX] = REG_RAX,
	[KOBOX_EXCEPTION_BX] = REG_RBX,
	[KOBOX_EXCEPTION_CX] = REG_RCX,
	[KOBOX_EXCEPTION_DX] = REG_RDX,
	[KOBOX_EXCEPTION_SI] = REG_RSI,
	[KOBOX_EXCEPTION_DI] = REG_RDI,
	[KOBOX_EXCEPTION_BP] = REG_RBP,
	[KOBOX_EXCEPTION_R8] = REG_R8,
	[KOBOX_EXCEPTION_R9] = REG_R9,
	[KOBOX_EXCEPTION_R10] = REG_R10,
	[KOBOX_EXCEPTION_R11] = REG_R11,
	[KOBOX_EXCEPTION_R12] = REG_R12,
	[KOBOX_EXCEPTION_R13] = REG_R13,
	[KOBOX_EXCEPTION_R14] = REG_R14,
	[KOBOX_EXCEPTION_R15] = REG_R15,
};
static _Atomic(kobox_linux_exception_fn) exception_dispatch;
static struct sigaction previous_actions[sizeof(exception_signals) /
					 sizeof(exception_signals[0])];
static unsigned int installed_count;
static _Thread_local volatile sig_atomic_t exception_active;

static void exception_handler(int number, siginfo_t *info, void *argument)
{
	ucontext_t *context = argument;
	greg_t *registers = context->uc_mcontext.gregs;
	struct kobox_linux_exception_frame frame = {.size = sizeof(frame)};
	kobox_linux_exception_fn dispatch;
	int saved_errno = errno;
	unsigned int index;

	dispatch = atomic_load_explicit(&exception_dispatch, memory_order_acquire);
	if (!dispatch || exception_active || info->si_code <= 0 ||
	    kobox_posix_current_cpu(&frame.cpu))
		goto fatal;
	exception_active = 1;
	for (index = 0; index < KOBOX_EXCEPTION_REGISTERS; index++)
		frame.registers[index] = registers[register_index[index]];
	frame.ip = registers[REG_RIP];
	frame.sp = registers[REG_RSP];
	frame.flags = registers[REG_EFL];
	frame.vector = registers[REG_TRAPNO];
	frame.error_code = registers[REG_ERR];
	frame.fault_address = (uintptr_t)info->si_addr;
	if (dispatch(&frame) != KOBOX_EXCEPTION_RESUME)
		goto fatal;
	for (index = 0; index < KOBOX_EXCEPTION_REGISTERS; index++)
		registers[register_index[index]] = frame.registers[index];
	registers[REG_RIP] = frame.ip;
	registers[REG_RSP] = frame.sp;
	/* The virtual Linux IRQ mask must never become a host RFLAGS.IF. */
	registers[REG_EFL] = (frame.flags & ~UINT64_C(0x200)) |
		(registers[REG_EFL] & UINT64_C(0x200));
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
