// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm_client.h"

#include <signal.h>
#include <linux/futex.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static struct kobox_vm_client_control *control;

static void stop_event(enum kobox_vm_client_event event)
{
	atomic_store_explicit(&control->event, event, memory_order_release);
	__asm__ volatile("int3" : : : "memory");
}

static void access_fault(int number, siginfo_t *info, void *argument)
{
	ucontext_t *context = argument;

	if (number != SIGSEGV || info->si_code <= 0)
		_exit(121);
	control->fault_address = (uintptr_t)info->si_addr;
	control->fault_ip = context->uc_mcontext.gregs[REG_RIP];
	control->fault_sp = context->uc_mcontext.gregs[REG_RSP];
	control->fault_flags = context->uc_mcontext.gregs[REG_EFL];
	control->fault_error = context->uc_mcontext.gregs[REG_ERR];
	stop_event(KOBOX_VM_CLIENT_FAULT);
	/* The tracer resumes this handler only after fault resolution. Native
	 * rt_sigreturn restores the actual interrupted instruction and context.
	 */
}

int main(void)
{
	struct sigaction action = {.sa_sigaction = access_fault, .sa_flags = SA_SIGINFO};
	void *window;

	control = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, KOBOX_VM_CONTROL_FD, 0);
	if (control == MAP_FAILED || !atomic_is_lock_free(&control->event))
		return 122;
	window = mmap((void *)(uintptr_t)KOBOX_VM_WINDOW_BASE, KOBOX_VM_WINDOW_SIZE,
		      PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (window == MAP_FAILED)
		return 123;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL))
		return 124;
	control->syscall_entry = (uintptr_t)kobox_vm_syscall_entry;
	atomic_store_explicit(&control->event, KOBOX_VM_CLIENT_READY, memory_order_release);
	(void)syscall(SYS_futex, &control->event, FUTEX_WAKE, 1, NULL, NULL, 0);
	/* The trusted bootstrap stays in user mode until the parent has seized
	 * it. PTRACE_INTERRUPT then works even during an unbounded user loop.
	 */
	while (!atomic_load_explicit(&control->attached, memory_order_acquire))
		__asm__ volatile("pause" : : : "memory");
	stop_event(KOBOX_VM_CLIENT_READY);
	for (;;) {
		volatile uint64_t *address = (void *)(uintptr_t)control->address;

		atomic_store_explicit(&control->event, KOBOX_VM_CLIENT_RUNNING, memory_order_release);
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
		stop_event(KOBOX_VM_CLIENT_DONE);
	}
}
