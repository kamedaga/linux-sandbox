// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "syscall transport line %d: %s\n", __LINE__, #expression); \
		return 1; \
	} \
} while (0)

static int begin(struct kobox_posix_vm *space, uint64_t number,
		 const uint64_t args[6], struct kobox_posix_vm_event *event)
{
	int result;

	space->control->write = 4;
	space->control->syscall_number = number;
	memcpy(space->control->syscall_arguments, args, sizeof(space->control->syscall_arguments));
	result = kobox_posix_vm_resume(space);
	return result ? result : kobox_posix_vm_wait(space, false, event);
}

static int exercise(const char *client)
{
	struct kobox_posix_memory_backing ram = {0};
	struct kobox_posix_vm space = {0};
	struct kobox_posix_vm_event event;
	const uint64_t args[6] = {KOBOX_VM_RAM_FD, KOBOX_VM_TEST_WINDOW_BASE,
		0xfedcba9876543210ULL, 0x1122334455667788ULL, 0, UINT64_MAX};
	const uint64_t numbers[] = {SYS_close, SYS_writev, SYS_dup, SYS_exit_group, UINT64_MAX};
	uint64_t previous = 0;
	unsigned int index;
	int status;
	pid_t pid;

	CHECK(!kobox_posix_memory_backing_init(&ram, 4096));
	CHECK(!kobox_posix_vm_create(&space, client, &ram,
		KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE));
	pid = space.pid;
	CHECK(!kobox_posix_vm_enable_syscalls(&space));
	CHECK(kobox_posix_vm_enable_syscalls(&space) == EALREADY);
	for (index = 0; index < 100; index++) {
		uint64_t number = numbers[index % (sizeof(numbers) / sizeof(numbers[0]))];
		int64_t reply = index & 1 ? -EBADF : 0x1234567800000000LL + index;

		CHECK(!begin(&space, number, args, &event));
		CHECK(event.kind == KOBOX_POSIX_VM_SYSCALL && event.syscall.number == number);
		CHECK(event.syscall.sequence == previous + 1 && event.ip && event.sp);
		CHECK(!memcmp(event.syscall.arguments, args, sizeof(args)));
		CHECK(space.syscall_pending && !space.running);
		CHECK(kobox_posix_vm_resume(&space) == EBUSY);
		event.user.ax = reply;
		CHECK(kobox_posix_vm_syscall_return(&space, previous, &event.user) == ESTALE);
		/* Mapping injection while the original syscall is pending proves
		 * both the stable stop and that host close(3) never executed.
		 */
		CHECK(!kobox_posix_vm_map(&space, KOBOX_VM_TEST_WINDOW_BASE, 0, 4096,
					KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE));
		previous = event.syscall.sequence;
		CHECK(!kobox_posix_vm_syscall_return(&space, previous, &event.user));
		CHECK(kobox_posix_vm_syscall_return(&space, previous, &event.user) == ESTALE);
		CHECK(!kobox_posix_vm_resume(&space));
		CHECK(!kobox_posix_vm_wait(&space, false, &event));
		CHECK(event.kind == KOBOX_POSIX_VM_STOP && space.pid == pid);
		CHECK(atomic_load_explicit(&space.control->event, memory_order_acquire) ==
		      KOBOX_VM_CLIENT_DONE && space.control->result == (uint64_t)reply);
	}
	CHECK(!begin(&space, SYS_close, args, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_SYSCALL);
	CHECK(!kill(pid, SIGKILL));
	CHECK(!kobox_posix_vm_wait(&space, false, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_EXIT && WIFSIGNALED(event.exit_status));
	CHECK(kobox_posix_vm_syscall_return(&space, previous + 1, &event.user) == ESRCH);
	CHECK(!kobox_posix_vm_destroy(&space));
	errno = 0;
	CHECK(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
	CHECK(!kobox_posix_memory_backing_destroy(&ram));
	return 0;
}

static int register_return(const char *client)
{
	struct kobox_posix_memory_backing ram = {0};
	struct kobox_posix_vm space = {0};
	struct kobox_posix_vm_event event;
	struct kobox_x86_user_regs before, changed, after;
	const uint64_t arguments[6] = {0};

	CHECK(!kobox_posix_memory_backing_init(&ram, 4096));
	CHECK(!kobox_posix_vm_create(&space, client, &ram,
		KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE));
	CHECK(!kobox_posix_vm_enable_syscalls(&space));
	CHECK(!begin(&space, SYS_getpid, arguments, &event));
	CHECK(!kobox_posix_vm_read_registers(&space, &before));
	CHECK(event.user.orig_ax == SYS_getpid && before.orig_ax == UINT64_MAX);
	CHECK(event.user.ip == event.ip && event.user.sp == event.sp &&
	      event.user.flags == event.flags && event.user.fs_base);
	changed = event.user;
	changed.cs = 0;
	CHECK(kobox_posix_vm_syscall_return(&space, event.syscall.sequence, &changed) == EINVAL);
	changed = event.user;
	changed.fs_base = 1ULL << 47;
	CHECK(kobox_posix_vm_syscall_return(&space, event.syscall.sequence, &changed) == EINVAL);
	changed = event.user;
	changed.flags ^= 1ULL << 12; /* IOPL is not user-writable. */
	CHECK(kobox_posix_vm_syscall_return(&space, event.syscall.sequence, &changed) == EINVAL);
	CHECK(!kobox_posix_vm_read_registers(&space, &after));
	CHECK(!memcmp(&before, &after, sizeof(before)) && space.syscall_pending);
	changed = event.user;
	changed.ax = 0x123456789abcdef0ULL;
	changed.bx = 2;
	changed.cx = 3;
	changed.dx = 4;
	changed.si = 5;
	changed.di = 6;
	changed.bp = 7;
	changed.r8 = 8;
	changed.r9 = 9;
	changed.r10 = 10;
	changed.r11 = 11;
	changed.r12 = 12;
	changed.r13 = 13;
	changed.r14 = 14;
	changed.r15 = 15;
	changed.ip = KOBOX_VM_TEST_WINDOW_BASE + 32;
	changed.sp = KOBOX_VM_TEST_WINDOW_BASE + 4096;
	changed.fs_base = KOBOX_VM_TEST_WINDOW_BASE + 1024;
	changed.gs_base = KOBOX_VM_TEST_WINDOW_BASE + 2048;
	changed.flags ^= 1; /* Carry, unlike IOPL, may be restored. */
	CHECK(kobox_posix_vm_syscall_return(&space, event.syscall.sequence - 1, &changed) == ESTALE);
	CHECK(!kobox_posix_vm_syscall_return(&space, event.syscall.sequence, &changed));
	CHECK(!kobox_posix_vm_read_registers(&space, &after));
	changed.orig_ax = UINT64_MAX; /* Never arm host syscall restart. */
	CHECK(!memcmp(&changed, &after, sizeof(changed)) && !space.syscall_pending);
	CHECK(kobox_posix_vm_syscall_return(&space, event.syscall.sequence, &changed) == ESTALE);
	/* The arbitrary new GPRs are observed at a stop, not resumed into C
	 * code which assumes its callee-saved registers were preserved.
	 */
	CHECK(!kobox_posix_vm_destroy(&space));
	CHECK(!kobox_posix_memory_backing_destroy(&ram));
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2 && !exercise(argv[1]) && !register_return(argv[1]));
	puts("POSIX syscall capture/reply passed (not the Linux client/FD Gate)");
	return 0;
}
