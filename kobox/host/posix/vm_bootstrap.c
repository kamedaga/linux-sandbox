// SPDX-License-Identifier: GPL-2.0-only

#include "vm_client.h"
#include "../../arch/x86_64/linux_bootstrap.h"
#include "../../arch/x86_64/user_layout.h"

/* This executable has no libc or dynamic loader. Use native Linux UAPI
 * declarations for signal entry, never hand-encoded signal-frame offsets.
 */
#include <asm/unistd.h>
#include <linux/mman.h>
#include <linux/futex.h>
#include <linux/signal.h>

void kobox_vm_native_sigreturn(void);

static _Noreturn void fail(unsigned int code)
{
	kobox_x86_linux_call(__NR_exit_group, code, 0, 0, 0, 0, 0);
	__builtin_trap();
}

static void access_fault(int signal, struct siginfo *info, void *argument)
{
	struct ucontext *context = argument;
	struct kobox_vm_client_control *owner;
	struct kobox_linux_exception_frame frame;

	if (signal != SIGSEGV || info->si_code <= 0 ||
	    context->uc_stack.ss_flags & SS_DISABLE || !context->uc_stack.ss_sp)
		fail(121);
	owner = (void *)((char *)context->uc_stack.ss_sp -
			KOBOX_VM_CLIENT_STACK_OFFSET);
	owner->fault_address = (uintptr_t)info->si_addr;
	kobox_x86_linux_bootstrap_fault(&frame, context);
	owner->fault_ip = frame.ip;
	owner->fault_sp = frame.sp;
	owner->fault_flags = frame.flags;
	owner->fault_error = frame.error_code;
	atomic_store_explicit(&owner->event, KOBOX_VM_CLIENT_FAULT,
			      memory_order_release);
	kobox_x86_linux_stop();
}

_Noreturn void kobox_vm_bootstrap(void)
{
	struct kobox_vm_client_control *control =
		(void *)KOBOX_X86_MACHINE_CONTEXT;
	/* The native UAPI names only sa_handler on x86-64; SA_SIGINFO selects
	 * this three-argument entry at signal delivery, as in libc's union.
	 */
	union {
		__sighandler_t handler;
		void (*siginfo)(int, struct siginfo *, void *);
	} entry = {.siginfo = access_fault};
	struct sigaction action = {
		.sa_handler = entry.handler,
		.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTORER,
		.sa_restorer = kobox_vm_native_sigreturn,
	};
	stack_t stack = {
		.ss_sp = (void *)(KOBOX_X86_MACHINE_CONTEXT +
				 KOBOX_VM_CLIENT_STACK_OFFSET),
		.ss_size = KOBOX_VM_CLIENT_STACK_SIZE,
	};
	unsigned long value;

	value = kobox_x86_linux_call(__NR_mmap, (uintptr_t)control,
		KOBOX_VM_CLIENT_CONTEXT_SIZE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1UL, 0);
	if (value != (uintptr_t)control)
		fail(122);
	value = kobox_x86_linux_call(__NR_mmap, (uintptr_t)control, 4096,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
		KOBOX_VM_CONTROL_FD, 0);
	if (value != (uintptr_t)control ||
	    control->window_start != KOBOX_X86_USER_START ||
	    control->window_size != KOBOX_X86_USER_END - KOBOX_X86_USER_START)
		fail(123);
	value = kobox_x86_linux_call(__NR_mmap, (uintptr_t)stack.ss_sp, stack.ss_size,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		-1UL, 0);
	if (value != (uintptr_t)stack.ss_sp ||
	    kobox_x86_linux_call(__NR_sigaltstack, (uintptr_t)&stack, 0, 0, 0, 0, 0) ||
	    kobox_x86_linux_call(__NR_rt_sigaction, SIGSEGV, (uintptr_t)&action, 0,
			 sizeof(action.sa_mask), 0, 0))
		fail(124);
	/* The executing image and both native stacks are above USER_END.
	 * Replacing this entire interval discards the original native stack,
	 * vDSO and every other bootstrap translation in the guest user range.
	 */
	value = kobox_x86_linux_call(__NR_mmap, control->window_start, control->window_size,
		PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
		-1UL, 0);
	if (value != control->window_start)
		fail(125);
	control->control_address = (uintptr_t)control;
	control->syscall_entry = (uintptr_t)kobox_vm_syscall_entry;
	atomic_store_explicit(&control->event, KOBOX_VM_CLIENT_READY,
			      memory_order_release);
	kobox_x86_linux_call(__NR_futex, (uintptr_t)&control->event, FUTEX_WAKE,
		     1, 0, 0, 0);
	while (!atomic_load_explicit(&control->attached, memory_order_acquire))
		kobox_x86_relax();
	kobox_x86_linux_stop();
	/* Only an explicit machine-frame start may leave READY. There is no
	 * diagnostic loop or host implementation of a guest syscall here.
	 */
	__builtin_trap();
}
