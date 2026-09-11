// SPDX-License-Identifier: GPL-2.0-only

#include <asm/signal.h>
#include <asm/unistd.h>
#include <linux/time_types.h>
#include <errno.h>

void kobox_sync_sigreturn(void);
long kobox_sync_interrupt_wait(int fd, unsigned long request, void *argument);

static volatile unsigned int delivered;

static long call(unsigned long number, unsigned long a0, unsigned long a1,
		 unsigned long a2, unsigned long a3)
{
	register unsigned long r10 __asm__("r10") = a3;

	__asm__ volatile("syscall" : "+a"(number) :
		"D"(a0), "S"(a1), "d"(a2), "r"(r10) :
		"rcx", "r11", "memory");
	return number;
}

static void handler(int signal)
{
	if (signal == SIGUSR1)
		delivered++;
}

long kobox_sync_interrupt_wait(int fd, unsigned long request, void *argument)
{
	struct sigaction action = {
		.sa_handler = handler, .sa_flags = SA_RESTORER,
		.sa_restorer = kobox_sync_sigreturn,
	}, previous;
	struct __kernel_timespec delay = {.tv_nsec = 20000000};
	long parent, child, result, waited, restored;
	int status = -1;

	delivered = 0;
	parent = call(__NR_getpid, 0, 0, 0, 0);
	result = call(__NR_rt_sigaction, SIGUSR1, (unsigned long)&action,
		      (unsigned long)&previous, sizeof(action.sa_mask));
	if (result)
		return result;
	child = call(__NR_fork, 0, 0, 0, 0);
	if (!child) {
		result = call(__NR_nanosleep, (unsigned long)&delay, 0, 0, 0);
		if (!result)
			result = call(__NR_kill, parent, SIGUSR1, 0, 0);
		call(__NR_exit_group, !!result, 0, 0, 0);
		__builtin_trap();
	}
	if (child < 0) {
		call(__NR_rt_sigaction, SIGUSR1, (unsigned long)&previous,
		     0, sizeof(action.sa_mask));
		return child;
	}
	/* No SA_RESTART: only an actually interrupted ioctl returns EINTR.
	 * A signal delivered before entering the wait cannot pass this check.
	 */
	result = call(__NR_ioctl, fd, request, (unsigned long)argument, 0);
	do {
		waited = call(__NR_wait4, child, (unsigned long)&status, 0, 0);
	} while (waited == -EINTR);
	restored = call(__NR_rt_sigaction, SIGUSR1, (unsigned long)&previous,
			0, sizeof(action.sa_mask));
	if (result != -EINTR || delivered != 1 || waited != child || status)
		return -EINVAL;
	return restored;
}
