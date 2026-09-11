// SPDX-License-Identifier: GPL-2.0-only

#include "../../boot/exec_gate.h"
#include <asm/signal.h>
#include <asm/sigcontext.h>
#include <asm/ucontext.h>
#include <asm/unistd.h>
#include <linux/mman.h>
#include <linux/signal.h>
#include <linux/time_types.h>
#include <errno.h>

void kobox_elf_sigreturn(void);
long kobox_elf_signal_call(unsigned long number, unsigned long a0,
			   unsigned long a1, unsigned long a2);
int kobox_elf_signal_spin(unsigned int *ready);
int kobox_elf_signal_fault(void *address);
void kobox_elf_bad_sigreturn(void);
extern const char kobox_elf_signal_call_begin[], kobox_elf_signal_call_end[];
extern const char kobox_elf_signal_spin_begin[], kobox_elf_signal_spin_end[];
extern const char kobox_elf_signal_fault_begin[], kobox_elf_signal_fault_end[];
int kobox_elf_signal_test(void);

static unsigned long alternate;
static unsigned int on_alternate, delivered, handler_error;
static unsigned int interrupted;
static unsigned int malformed;
static int acknowledge;
static void *fault_address;

static long call(unsigned long number, unsigned long a0, unsigned long a1,
		 unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
	register unsigned long r10 __asm__("r10") = a3;
	register unsigned long r8 __asm__("r8") = a4;
	register unsigned long r9 __asm__("r9") = a5;

	__asm__ volatile("syscall" : "+a"(number) :
		"D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9) :
		"rcx", "r11", "memory");
	return number;
}

static int fail(unsigned int line)
{
	struct kobox_exec_failure failure = {
		.pid = call(__NR_getpid, 0, 0, 0, 0, 0, 0), .line = line,
		.error = ((unsigned long)interrupted << 32) | handler_error,
	};

	call(__NR_pwrite64, 19, (uintptr_t)&failure, sizeof(failure),
	     KOBOX_EXEC_FAILURE_OFFSET, 0, 0);
	return 1;
}

#define CHECK(condition) do { if (!(condition)) return fail(__LINE__); } while (0)
#define SIGNAL_BIT(number) (1UL << ((number) - 1))

static void handler(int signal, struct siginfo *info, void *argument)
{
	struct ucontext *context = argument;
	struct _fpstate *fp = context->uc_mcontext.fpstate;
	unsigned long mask = 0, local = (unsigned long)&mask;
	unsigned long expected = SIGNAL_BIT(SIGWINCH) | SIGNAL_BIT(SIGUSR1) |
		SIGNAL_BIT(SIGUSR2);
	unsigned int mxcsr = 0x1f80;
	int expected_signal = interrupted == 4 ? SIGSEGV : SIGUSR1;

	if (interrupted == 4)
		expected = SIGNAL_BIT(SIGWINCH) | SIGNAL_BIT(SIGSEGV) | SIGNAL_BIT(SIGUSR2);
	if (signal != expected_signal || info->si_signo != expected_signal)
		handler_error = 10;
	else if (!fp)
		handler_error = 11;
	else if (fp->xmm_space[8 * 4] != 0x12345678)
		handler_error = 12;
	else if (fp->xmm_space[0] != 0x12345678)
		handler_error = 13;
	else if (fp->mxcsr != 0x7f80)
		handler_error = 16;
	else if (context->uc_mcontext.r12 != 0x11223344)
		handler_error = 14;
	else if (!!(local >= alternate && local < alternate + 65536) != !!on_alternate)
		handler_error = 15;
	if (call(__NR_rt_sigprocmask, SIG_SETMASK, 0, (uintptr_t)&mask, sizeof(mask), 0, 0) ||
	    mask != expected)
		handler_error = 2;
	if (interrupted == 1 && (context->uc_mcontext.rip != (uintptr_t)kobox_elf_signal_call_end ||
				(long)context->uc_mcontext.rax != -EINTR))
		handler_error = 3;
	if (interrupted == 2 && (context->uc_mcontext.rip != (uintptr_t)kobox_elf_signal_call_begin ||
				context->uc_mcontext.rax != __NR_read))
		handler_error = 4;
	if (interrupted == 3 && (context->uc_mcontext.rip < (uintptr_t)kobox_elf_signal_spin_begin ||
				context->uc_mcontext.rip >= (uintptr_t)kobox_elf_signal_spin_end))
		handler_error = 5;
	if (interrupted == 4) {
		if (context->uc_mcontext.rip != (uintptr_t)kobox_elf_signal_fault_begin ||
		    info->si_addr != fault_address || info->si_code != SEGV_ACCERR)
			handler_error = 17;
		context->uc_mcontext.rip = (uintptr_t)kobox_elf_signal_fault_end;
	}
	if (interrupted == 1 || interrupted == 2) {
		char byte = 's';

		if (call(__NR_write, acknowledge, (uintptr_t)&byte, 1, 0, 0, 0) != 1)
			handler_error = 6;
	}
	/* Change the saved GPR and clobber live FP registers. Only the real
	 * rt_sigreturn restores the FP bank and applies this saved GPR value.
	 */
	context->uc_mcontext.r12 = 0x55667788;
	if (malformed == 1)
		context->uc_mcontext.fpstate = (void *)1;
	else if (malformed == 2)
		fp->mxcsr |= 1U << 31;
	__asm__ volatile("pxor %%xmm8, %%xmm8" : : : "xmm8");
	__asm__ volatile("fninit; ldmxcsr %0" : : "m"(mxcsr));
	delivered++;
}

static _Noreturn void child_exit(int result)
{
	call(__NR_exit, result, 0, 0, 0, 0, 0);
	__builtin_trap();
}

static int send_signal(unsigned long parent, unsigned int *ready, int input, int output)
{
	const struct __kernel_timespec delay = {.tv_nsec = 1000000};
	unsigned int cpu, attempts = 0;
	unsigned long mask;
	char byte;

	CHECK(!call(__NR_getcpu, (uintptr_t)&cpu, 0, 0, 0, 0, 0));
	mask = 1UL << (cpu ^ 1);
	CHECK(!call(__NR_sched_setaffinity, 0, sizeof(mask), (uintptr_t)&mask, 0, 0, 0));
	while (!__atomic_load_n(ready, __ATOMIC_ACQUIRE)) {
		CHECK(attempts++ < 5000);
		CHECK(!call(__NR_nanosleep, (uintptr_t)&delay, 0, 0, 0, 0, 0));
	}
	/* The handler verifies the exact interrupted read/restart IP, so a
	 * delivery which precedes the blocking syscall cannot pass this test.
	 */
	if (interrupted != 3)
		for (attempts = 0; attempts < 10; attempts++)
			CHECK(!call(__NR_nanosleep, (uintptr_t)&delay, 0, 0, 0, 0, 0));
	CHECK(!call(__NR_kill, parent, SIGUSR1, 0, 0, 0, 0));
	if (interrupted != 3) {
		CHECK(call(__NR_read, input, (uintptr_t)&byte, 1, 0, 0, 0) == 1 && byte == 's');
		byte = 'r';
		CHECK(call(__NR_write, output, (uintptr_t)&byte, 1, 0, 0, 0) == 1);
	}
	return 0;
}

static int asynchronous(struct sigaction *action, unsigned long parent)
{
	unsigned int *ready;
	unsigned int before = delivered;
	int data[2], ack[2], status = -1;
	long child, result;
	char byte = 0;

	ready = (void *)call(__NR_mmap, 0, 4096, PROT_READ | PROT_WRITE,
			     MAP_SHARED | MAP_ANONYMOUS, -1UL, 0);
	CHECK((unsigned long)ready < (unsigned long)-4095);
	CHECK(!call(__NR_pipe2, (uintptr_t)data, 0, 0, 0, 0, 0));
	CHECK(!call(__NR_pipe2, (uintptr_t)ack, 0, 0, 0, 0, 0));
	acknowledge = ack[1];
	action->sa_flags = SA_SIGINFO | SA_RESTORER | SA_ONSTACK |
		(interrupted == 2 ? SA_RESTART : 0);
	CHECK(!call(__NR_rt_sigaction, SIGUSR1, (uintptr_t)action, 0,
		    sizeof(action->sa_mask), 0, 0));
	child = call(__NR_fork, 0, 0, 0, 0, 0, 0);
	CHECK(child >= 0);
	if (!child)
		child_exit(send_signal(parent, ready, ack[0], data[1]));
	if (interrupted == 3) {
		result = kobox_elf_signal_spin(ready);
		CHECK(!result);
	} else {
		__atomic_store_n(ready, 1, __ATOMIC_RELEASE);
		result = kobox_elf_signal_call(__NR_read, data[0], (uintptr_t)&byte, 1);
		CHECK(interrupted == 1 ? result == -EINTR : result == 1 && byte == 'r');
	}
	CHECK(delivered == before + 1 && !handler_error);
	CHECK(call(__NR_wait4, child, (uintptr_t)&status, 0, 0, 0, 0) == child && !status);
	for (unsigned int index = 0; index < 2; index++) {
		CHECK(!call(__NR_close, data[index], 0, 0, 0, 0, 0));
		CHECK(!call(__NR_close, ack[index], 0, 0, 0, 0, 0));
	}
	CHECK(!call(__NR_munmap, (uintptr_t)ready, 4096, 0, 0, 0, 0));
	return 0;
}

static int invalid_frames(void)
{
	int status;
	long child;

	for (malformed = 1; malformed <= 3; malformed++) {
		child = call(__NR_fork, 0, 0, 0, 0, 0, 0);
		CHECK(child >= 0);
		if (!child) {
			if (malformed == 3)
				kobox_elf_bad_sigreturn();
			else
				kobox_elf_signal_call(__NR_kill,
					call(__NR_getpid, 0, 0, 0, 0, 0, 0), SIGUSR1, 0);
			child_exit(fail(__LINE__));
		}
		status = 0;
		CHECK(call(__NR_wait4, child, (uintptr_t)&status, 0, 0, 0, 0) == child);
		CHECK((status & 0x7f) == SIGSEGV);
	}
	malformed = 0;
	return 0;
}

int kobox_elf_signal_test(void)
{
	union {
		__sighandler_t raw;
		void (*siginfo)(int, struct siginfo *, void *);
	} entry = {.siginfo = handler};
	struct sigaction action = {
		.sa_handler = entry.raw, .sa_restorer = kobox_elf_sigreturn,
		.sa_mask = SIGNAL_BIT(SIGUSR2),
	};
	unsigned long blocked = SIGNAL_BIT(SIGWINCH), observed = 0;
	unsigned long pid = call(__NR_getpid, 0, 0, 0, 0, 0, 0);
	stack_t stack;

	alternate = call(__NR_mmap, 0, 65536, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1UL, 0);
	CHECK(alternate < (unsigned long)-4095);
	stack = (stack_t){.ss_sp = (void *)alternate, .ss_size = 65536};
	CHECK(!call(__NR_sigaltstack, (uintptr_t)&stack, 0, 0, 0, 0, 0));
	CHECK(!call(__NR_rt_sigprocmask, SIG_SETMASK, (uintptr_t)&blocked, 0,
		    sizeof(blocked), 0, 0));
	for (on_alternate = 0; on_alternate < 2; on_alternate++) {
		action.sa_flags = SA_SIGINFO | SA_RESTORER | (on_alternate ? SA_ONSTACK : 0);
		CHECK(!call(__NR_rt_sigaction, SIGUSR1, (uintptr_t)&action, 0,
			    sizeof(action.sa_mask), 0, 0));
		CHECK(!kobox_elf_signal_call(__NR_kill, pid, SIGUSR1, 0));
		CHECK(delivered == on_alternate + 1 && !handler_error);
		CHECK(!call(__NR_rt_sigprocmask, SIG_SETMASK, 0, (uintptr_t)&observed,
			    sizeof(observed), 0, 0));
		CHECK(observed == blocked);
	}
	on_alternate = 1;
	for (interrupted = 1; interrupted <= 3; interrupted++) {
		if (asynchronous(&action, pid))
			return 1;
		CHECK(!call(__NR_rt_sigprocmask, SIG_SETMASK, 0, (uintptr_t)&observed,
			    sizeof(observed), 0, 0));
		CHECK(observed == blocked);
	}
	interrupted = 4;
	fault_address = (void *)call(__NR_mmap, 0, 4096, PROT_NONE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1UL, 0);
	CHECK((unsigned long)fault_address < (unsigned long)-4095);
	CHECK(!call(__NR_rt_sigaction, SIGSEGV, (uintptr_t)&action, 0,
		    sizeof(action.sa_mask), 0, 0));
	CHECK(!kobox_elf_signal_fault(fault_address));
	CHECK(delivered == 6 && !handler_error);
	CHECK(!call(__NR_munmap, (uintptr_t)fault_address, 4096, 0, 0, 0, 0));
	action = (struct sigaction){.sa_handler = SIG_DFL};
	CHECK(!call(__NR_rt_sigaction, SIGSEGV, (uintptr_t)&action, 0,
		    sizeof(action.sa_mask), 0, 0));
	interrupted = 0;
	if (invalid_frames())
		return 1;
	stack.ss_flags = SS_DISABLE;
	CHECK(!call(__NR_sigaltstack, (uintptr_t)&stack, 0, 0, 0, 0, 0));
	blocked = 0;
	CHECK(!call(__NR_rt_sigprocmask, SIG_SETMASK, (uintptr_t)&blocked, 0,
		    sizeof(blocked), 0, 0));
	CHECK(!call(__NR_munmap, alternate, 65536, 0, 0, 0, 0));
	return 0;
}
