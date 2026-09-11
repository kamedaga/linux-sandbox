// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../host/posix/exception.h"
#include "../host/posix/host.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "exception check failed at line %d: %s\n", \
			__LINE__, #expression); \
		return 1; \
	} \
} while (0)

static struct kobox_posix_cpu cpus[2];
static struct kobox_posix_memory_window inaccessible;
static atomic_uint observed_cpus;

static void unexpected_notification(void *context, uint32_t cpu,
				    enum kobox_posix_notification notification,
				    uint64_t count)
{
	(void)context;
	(void)cpu;
	(void)notification;
	(void)count;
	_exit(120);
}

static enum kobox_linux_exception_result recover_probe(
	struct kobox_linux_exception_frame *frame)
{
	/* Vector 14 is the x86 page-fault vector, not a Linux structure field. */
	if (frame->size != sizeof(*frame) || frame->cpu >= 2 ||
	    frame->vector != 14 || frame->fault_address != (uintptr_t)inaccessible.address)
		return KOBOX_EXCEPTION_FATAL;
	atomic_fetch_or_explicit(&observed_cpus, 1U << frame->cpu, memory_order_relaxed);
	frame->ip = frame->registers[KOBOX_EXCEPTION_R11];
	frame->registers[KOBOX_EXCEPTION_AX] = 0x12345678 + frame->cpu;
	frame->flags &= ~UINT64_C(0x200);
	errno = ERANGE;
	return KOBOX_EXCEPTION_RESUME;
}

static void *run_probe(void *argument)
{
	struct kobox_posix_cpu *cpu = argument;
	unsigned long result;
	unsigned long flags;
	uint32_t current_cpu;

	if (kobox_posix_cpu_enter(cpu) ||
	    kobox_posix_current_cpu(&current_cpu) ||
	    current_cpu != cpu->logical_cpu)
		return (void *)1;
	errno = ENOTTY;
	/* Use an assembly label, not a guessed instruction length. */
	__asm__ volatile("leaq 2f(%%rip), %%r11\n\t"
		     "movq (%1), %%rax\n\t"
		     "2:"
		     : "=a" (result)
		     : "r" (inaccessible.address)
		     : "r11", "cc", "memory");
	__asm__ volatile("pushfq; popq %0" : "=r" (flags) : : "memory");
	if (result != 0x12345678 + current_cpu || errno != ENOTTY ||
	    !(flags & 0x200) || kobox_posix_cpu_leave(cpu))
		return (void *)1;
	return NULL;
}

int main(void)
{
	struct kobox_posix_thread threads[2] = {0};
	struct sigaction original;
	struct sigaction restored;
	void *result;
	uint32_t current_cpu;
	unsigned int cpu;
	pid_t child;
	int status;

	CHECK(sigaction(SIGILL, NULL, &original) == 0);
	CHECK(kobox_posix_current_cpu(&current_cpu) == ENXIO);
	CHECK(kobox_posix_exceptions_install(NULL) == EINVAL);
	CHECK(kobox_posix_exceptions_install(recover_probe) == 0);
	CHECK(kobox_posix_exceptions_install(recover_probe) == EBUSY);
	CHECK(kobox_posix_memory_window_init(&inaccessible, 4096) == 0);
	for (cpu = 0; cpu < 2; cpu++) {
		CHECK(kobox_posix_cpu_init(&cpus[cpu], cpu,
			unexpected_notification, NULL) == 0);
		CHECK(kobox_posix_thread_start(&threads[cpu], run_probe,
			&cpus[cpu]) == 0);
	}
	for (cpu = 0; cpu < 2; cpu++) {
		CHECK(kobox_posix_thread_join(&threads[cpu], &result) == 0);
		CHECK(result == NULL);
	}
	CHECK(atomic_load(&observed_cpus) == 3);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		if (kobox_posix_cpu_enter(&cpus[0]))
			_exit(121);
		__asm__ volatile("hlt");
		_exit(122);
	}
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGSEGV);
	CHECK(kobox_posix_exceptions_remove() == 0);
	CHECK(sigaction(SIGILL, NULL, &restored) == 0);
	CHECK(original.sa_handler == restored.sa_handler);
	for (cpu = 0; cpu < 2; cpu++)
		CHECK(kobox_posix_cpu_destroy(&cpus[cpu]) == 0);
	CHECK(kobox_posix_memory_window_destroy(&inaccessible) == 0);
	return 0;
}
