// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm_client.h"
#include "../../tests/clients/vm_program.h"
#include "../../arch/x86_64/linux_call.h"
#include "../../arch/x86_64/linux_signal.h"

#include <signal.h>
#include <stdbool.h>
#include <errno.h>
#include <linux/futex.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

static _Noreturn void client_loop(struct kobox_vm_client_control *control);

uint64_t kobox_vm_program_syscall(uint64_t number, uint64_t a0, uint64_t a1,
	uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	return kobox_x86_linux_call(number, a0, a1, a2, a3, a4, a5);
}

static uint64_t issue_syscall(struct kobox_vm_client_control *control)
{
	return kobox_x86_linux_call(control->syscall_number, control->syscall_arguments[0],
		control->syscall_arguments[1], control->syscall_arguments[2],
		control->syscall_arguments[3], control->syscall_arguments[4],
		control->syscall_arguments[5]);
}
static void stop_event(struct kobox_vm_client_control *owner,
		       enum kobox_vm_client_event event)
{
	atomic_store_explicit(&owner->event, event, memory_order_release);
	kobox_x86_linux_stop();
}

static void access_fault(int number, siginfo_t *info, void *argument)
{
	ucontext_t *context = argument;
	struct kobox_vm_client_control *owner;
	struct kobox_linux_exception_frame frame;

	if (number != SIGSEGV || info->si_code <= 0 ||
	    (context->uc_stack.ss_flags & SS_DISABLE) ||
	    !context->uc_stack.ss_sp)
		_exit(121);
	/* Native sigaltstack is per context. Neither guest TLS nor a shared
	 * bootstrap global can identify the owner of a concurrent fault.
	 */
	owner = (void *)((char *)context->uc_stack.ss_sp - KOBOX_VM_CLIENT_STACK_OFFSET);
	kobox_x86_linux_exception_import(&frame, argument, (uintptr_t)info->si_addr);
	owner->fault_address = frame.fault_address;
	owner->fault_ip = frame.ip;
	owner->fault_sp = frame.sp;
	owner->fault_flags = frame.flags;
	owner->fault_error = frame.error_code;
	stop_event(owner, KOBOX_VM_CLIENT_FAULT);
	/* The tracer resumes this handler only after fault resolution. Native
	 * rt_sigreturn restores the actual interrupted instruction and context.
	 */
}

int main(void)
{
	struct kobox_vm_client_control *control;
	struct sigaction action = {
		.sa_sigaction = access_fault,
		.sa_flags = SA_SIGINFO | SA_ONSTACK,
	};
	stack_t stack = {.ss_size = KOBOX_VM_CLIENT_STACK_SIZE};
	long signal_stack_size = sysconf(_SC_SIGSTKSZ);
	void *window, *reservation;

	if (sysconf(_SC_PAGESIZE) != KOBOX_VM_CLIENT_PAGE_SIZE ||
	    signal_stack_size <= 0 ||
	    (unsigned long)signal_stack_size > KOBOX_VM_CLIENT_STACK_SIZE)
		return 122;
	/* Control, lower guard, signal stack, upper guard. Fork inherits this
	 * registration; the tracer replaces only the child's control page.
	 */
	reservation = mmap(NULL, KOBOX_VM_CLIENT_CONTEXT_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return 122;
	control = mmap(reservation, KOBOX_VM_CLIENT_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_FIXED, KOBOX_VM_CONTROL_FD, 0);
	if (control == MAP_FAILED || !atomic_is_lock_free(&control->event))
		return 122;
	stack.ss_sp = mmap((char *)reservation + KOBOX_VM_CLIENT_STACK_OFFSET,
			   stack.ss_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (stack.ss_sp == MAP_FAILED || sigaltstack(&stack, NULL))
		return 122;
	if (!kobox_vm_window_valid(control->window_start, control->window_size))
		return 123;
	window = mmap((void *)(uintptr_t)control->window_start, control->window_size,
		      PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (window == MAP_FAILED)
		return 123;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL))
		return 124;
	control->syscall_entry = (uintptr_t)kobox_vm_syscall_entry;
	control->client_entry = (uintptr_t)client_loop;
	control->control_address = (uintptr_t)control;
	atomic_store_explicit(&control->event, KOBOX_VM_CLIENT_READY, memory_order_release);
	(void)syscall(SYS_futex, &control->event, FUTEX_WAKE, 1, NULL, NULL, 0);
	/* The trusted bootstrap stays in user mode until the parent has seized
	 * it. PTRACE_INTERRUPT then works even during an unbounded user loop.
	 */
	while (!atomic_load_explicit(&control->attached, memory_order_acquire))
		kobox_x86_relax();
	stop_event(control, KOBOX_VM_CLIENT_READY);
	client_loop(control);
}

static _Noreturn void client_loop(struct kobox_vm_client_control *control)
{
	for (;;) {
		volatile uint64_t *address = (void *)(uintptr_t)control->address;

		atomic_store_explicit(&control->event, KOBOX_VM_CLIENT_RUNNING, memory_order_release);
		if (control->write == 11)
			kobox_vm_root_entry(control->address, control->value,
					    control->address + 32 * 4096);
		if (control->write == 9 || control->write == 10) {
			control->result = kobox_vm_clone_entry(control->value,
				control->address + 4 * 4096, control->address,
				control->write == 10 ? control->address + 296 : 0);
			stop_event(control, KOBOX_VM_CLIENT_DONE);
			continue;
		}
		if (control->write == 8) {
			control->result = kobox_vm_stack_access(control->address, control->value);
			stop_event(control, KOBOX_VM_CLIENT_DONE);
			continue;
		}
		if (control->write == 7) {
			uint64_t pid = kobox_x86_linux_call(SYS_fork, 0, 0, 0, 0, 0, 0);

			if (!pid)
				kobox_vm_child_program(control->address);
			control->result = pid;
			stop_event(control, KOBOX_VM_CLIENT_DONE);
			continue;
		}
		if (control->write == 4) {
			control->result = issue_syscall(control);
			stop_event(control, KOBOX_VM_CLIENT_DONE);
			continue;
		}
		if (control->write == 5 || control->write == 6) {
			uint64_t value;

			if (control->write == 5)
				__asm__ volatile("movq %%fs:0, %0" : "=r"(value) : : "memory");
			else
				__asm__ volatile("movq %%gs:0, %0" : "=r"(value) : : "memory");
			control->result = value;
			stop_event(control, KOBOX_VM_CLIENT_DONE);
			continue;
		}
		/* A negative transport test: this must be intercepted before the
		 * host kernel can bypass the mapping authority's protection.
		 */
		if (control->write == 2)
			(void)syscall(SYS_mprotect, (void *)address, 4096, PROT_READ | PROT_WRITE);
		if (control->write == 3) {
			while (atomic_load_explicit(&control->loop, memory_order_acquire)) {
				*address = control->value;
				atomic_fetch_add_explicit(&control->iterations, 1, memory_order_release);
			}
		}
		if (control->write)
			*address = control->value;
		control->result = *address;
		stop_event(control, KOBOX_VM_CLIENT_DONE);
	}
}
