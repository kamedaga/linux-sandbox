// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm.h"

#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "VM transport check failed at line %d: %s\n", \
			__LINE__, #expression); \
		return 1; \
	} \
} while (0)

static int access_begin(struct kobox_posix_vm *space, uint64_t address,
			unsigned int write, uint64_t value,
			struct kobox_posix_vm_event *event)
{
	int result;

	space->control->address = address;
	space->control->write = write;
	space->control->value = value;
	result = kobox_posix_vm_resume(space);
	return result ? result : kobox_posix_vm_wait(space, false, event);
}

static int access_resume(struct kobox_posix_vm *space, struct kobox_posix_vm_event *event)
{
	int result = kobox_posix_vm_resume(space);

	return result ? result : kobox_posix_vm_wait(space, false, event);
}

static bool access_done(struct kobox_posix_vm *space, struct kobox_posix_vm_event *event,
			uint64_t value)
{
	return event->kind == KOBOX_POSIX_VM_STOP &&
		atomic_load_explicit(&space->control->event, memory_order_acquire) ==
		KOBOX_VM_CLIENT_DONE && space->control->result == value;
}

static int reset_checked(struct kobox_posix_vm *space, uint64_t address)
{
	struct user_regs_struct regs;
	siginfo_t info;
	int result = kobox_posix_vm_reset(space, address, 4096);

	if (result) {
		fprintf(stderr, "reset error=%d running=%u capturing=%u sigreturn=%u pending=%u\n",
			result, space->running, space->capturing_fault,
			space->signal_return, space->event_pending);
		if (!ptrace(PTRACE_GETREGS, space->pid, NULL, &regs) &&
		    !ptrace(PTRACE_GETSIGINFO, space->pid, NULL, &info))
			fprintf(stderr, "stop signal=%d code=%d address=%p trampoline_delta=%lld\n",
				info.si_signo, info.si_code, info.si_addr,
				(long long)(regs.rip - space->syscall_entry));
	}
	return result;
}

static int exercise(const char *client, uint64_t base, size_t window_size)
{
	struct kobox_posix_memory_backing ram = {0};
	struct kobox_posix_vm spaces[2] = {0};
	struct kobox_posix_vm failed = {0};
	struct kobox_posix_vm_event event;
	unsigned int rw = KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE;
	uint64_t *direct;
	pid_t pids[2];
	int status, index;

	CHECK(kobox_posix_memory_backing_init(&ram, 4 * 4096) == 0);
	direct = mmap(NULL, ram.size, PROT_READ | PROT_WRITE, MAP_SHARED, ram.descriptor, 0);
	CHECK(direct != MAP_FAILED);
	direct[0] = 0x13579;
	direct[4096 / sizeof(*direct)] = 0xabcde;
	CHECK(kobox_posix_vm_create(&failed, client, &ram, 0, window_size) == EINVAL);
	CHECK(kobox_posix_vm_create(&failed, client, &ram, base + 1, window_size) == EINVAL);
	CHECK(kobox_posix_vm_create(&failed, client, &ram, base, 0) == EINVAL);
	CHECK(kobox_posix_vm_create(&failed, client, &ram, base, window_size + 1) == EINVAL);
	CHECK(kobox_posix_vm_create(&failed, client, &ram, base, SIZE_MAX - 4095) == EINVAL);
	CHECK(kobox_posix_vm_create(&failed, client, &ram, UINT64_C(1) << 47, 4096) == EINVAL);
	CHECK(!failed.owner && !failed.pid && !failed.control);
	CHECK(kobox_posix_vm_reset(NULL, base, 4096) == EINVAL);
	CHECK(kobox_posix_vm_protect(NULL, base, 4096, rw) == EINVAL);
	CHECK(kobox_posix_vm_create(&failed, "/nonexistent/kobox-vm-client", &ram,
		base, window_size) == ENOENT);
	CHECK(!failed.pid && !failed.control && !failed.control_backing.initialized);
	for (index = 0; index < 2; index++) {
		CHECK(kobox_posix_vm_create(&spaces[index], client, &ram, base, window_size) == 0);
		pids[index] = spaces[index].pid;
		CHECK(pids[index] > 0 && pids[index] != getpid());
	}
	CHECK(pids[0] != pids[1]);
	/* Exercise the last page, including a relocated window much larger
	 * than the old diagnostic reservation. Bounds remain host-owned even
	 * if the client changes its bootstrap copy after initialization.
	 */
	spaces[0].control->window_start = 0;
	spaces[0].control->window_size = UINT64_MAX;
	CHECK(kobox_posix_vm_reset(&spaces[0], base - 4096, 4096) == EINVAL);
	CHECK(kobox_posix_vm_reset(&spaces[0], base, SIZE_MAX - 4095) == EINVAL);
	CHECK(access_begin(&spaces[0], base + window_size - 4096, 0, 0, &event) == 0);
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == base + window_size - 4096);
	CHECK(kobox_posix_vm_map(&spaces[0], base + window_size - 4096, 0, 4096, rw) == 0);
	CHECK(access_resume(&spaces[0], &event) == 0 && access_done(&spaces[0], &event, direct[0]));
	CHECK(kobox_posix_vm_reset(&spaces[0], base + window_size - 4096, 4096) == 0);
	/* A push with RSP inside an inaccessible guest page must reach the
	 * context's separate signal stack, then retry the real instruction.
	 */
	CHECK(access_begin(&spaces[0], base + 4096, 8, 0xfeed1234, &event) == 0);
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT &&
	      event.address == base + 4096 && event.sp == base + 4096 + 8 &&
	      (event.error & 6) == 6);
	CHECK(kobox_posix_vm_map(&spaces[0], base + 4096, 4096, 4096, rw) == 0);
	CHECK(access_resume(&spaces[0], &event) == 0 &&
	      access_done(&spaces[0], &event, 0xfeed1234));
	CHECK(direct[4096 / sizeof(*direct)] == 0xfeed1234);
	direct[4096 / sizeof(*direct)] = 0xabcde;
	CHECK(kobox_posix_vm_wait(&spaces[0], true, &event) == EAGAIN);
	CHECK(access_begin(&spaces[0], base, 0, 0, &event) == 0);
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == base &&
	      (event.error & 6) == 4 && event.ip && event.sp && (event.flags & 0x200));
	CHECK(kobox_posix_vm_map(&spaces[0], base, 0, 2 * 4096, rw) == 0);
	CHECK(access_resume(&spaces[0], &event) == 0 && access_done(&spaces[0], &event, 0x13579));
	CHECK(kobox_posix_vm_map(&spaces[1], base, 0, 2 * 4096, rw) == 0);
	CHECK(access_begin(&spaces[1], base, 1, 0x24680, &event) == 0);
	CHECK(access_done(&spaces[1], &event, 0x24680) && direct[0] == 0x24680);
	CHECK(access_begin(&spaces[0], base, 0, 0, &event) == 0);
	CHECK(access_done(&spaces[0], &event, 0x24680));
	CHECK(kobox_posix_vm_protect(&spaces[0], base, 4096, KOBOX_POSIX_MEMORY_READ) == 0);
	CHECK(access_begin(&spaces[0], base, 1, 0xaaa55, &event) == 0);
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == base &&
	      (event.error & 7) == 7 && direct[0] == 0x24680);
	CHECK(access_begin(&spaces[1], base, 1, 0x33445, &event) == 0);
	CHECK(access_done(&spaces[1], &event, 0x33445));
	CHECK(kobox_posix_vm_protect(&spaces[0], base, 4096, rw) == 0);
	CHECK(access_resume(&spaces[0], &event) == 0 && access_done(&spaces[0], &event, 0xaaa55));
	CHECK(direct[0] == 0xaaa55);
	CHECK(kobox_posix_vm_reset(&spaces[0], base, 4096) == 0);
	CHECK(access_begin(&spaces[0], base + 4096, 0, 0, &event) == 0);
	CHECK(access_done(&spaces[0], &event, 0xabcde));
	CHECK(access_begin(&spaces[1], base, 0, 0, &event) == 0);
	CHECK(access_done(&spaces[1], &event, 0xaaa55));
	CHECK(access_begin(&spaces[0], base, 0, 0, &event) == 0);
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == base);
	CHECK(kobox_posix_vm_map(&spaces[0], base, 4096, 4096, rw) == 0);
	CHECK(access_resume(&spaces[0], &event) == 0 && access_done(&spaces[0], &event, 0xabcde));
	CHECK(kobox_posix_vm_map(&spaces[0], base + 1, 0, 4096, rw) == EINVAL);
	CHECK(kobox_posix_vm_map(&spaces[0], base, ram.size, 4096, rw) == EINVAL);
	CHECK(kobox_posix_vm_map(&spaces[0], base - 4096, 0, 4096, rw) == EINVAL);
	CHECK(kobox_posix_vm_protect(&spaces[0], base, 0, rw) == EINVAL);
	CHECK(kobox_posix_vm_protect(&spaces[0], base, 4096, 8) == EINVAL);
	CHECK(kobox_posix_vm_reset(&spaces[0], base + window_size, 4096) == EINVAL);
	for (index = 0; index < 200; index++) {
		CHECK(kobox_posix_vm_reset(&spaces[0], base, 4096) == 0);
		spaces[0].control->address = base;
		spaces[0].control->write = 0;
		CHECK(kobox_posix_vm_resume(&spaces[0]) == 0);
		/* Interrupt can race fault delivery, its handler, or sigreturn. */
		CHECK(reset_checked(&spaces[0], base) == 0);
		CHECK(kobox_posix_vm_wait(&spaces[0], false, &event) == 0);
		CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == base);
		CHECK(kobox_posix_vm_map(&spaces[0], base, 4096, 4096, rw) == 0);
		CHECK(kobox_posix_vm_resume(&spaces[0]) == 0);
		CHECK(reset_checked(&spaces[0], base) == 0);
		CHECK(kobox_posix_vm_wait(&spaces[0], false, &event) == 0);
		if (event.kind == KOBOX_POSIX_VM_FAULT) {
			CHECK(kobox_posix_vm_map(&spaces[0], base, 4096, 4096, rw) == 0);
			CHECK(access_resume(&spaces[0], &event) == 0);
		}
		CHECK(access_done(&spaces[0], &event, 0xabcde));
	}
	CHECK(kobox_posix_vm_map(&spaces[0], base, 4096, 4096, rw) == 0);
	/* Revoke a genuinely running, unbounded writer. Stopping must not wait
	 * for a Linux fault worker or for the writer to cooperate.
	 */
	spaces[0].control->address = base;
	spaces[0].control->write = 3;
	spaces[0].control->value = 0xabcde;
	atomic_store_explicit(&spaces[0].control->loop, 1, memory_order_release);
	CHECK(kobox_posix_vm_resume(&spaces[0]) == 0);
	while (atomic_load_explicit(&spaces[0].control->iterations, memory_order_acquire) < 100)
		__asm__ volatile("pause" : : : "memory");
	CHECK(kobox_posix_vm_reset(&spaces[0], base, 4096) == 0);
	CHECK(kobox_posix_vm_wait(&spaces[0], false, &event) == 0);
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == base && (event.error & 6) == 6);
	CHECK(kobox_posix_vm_map(&spaces[0], base, 4096, 4096, rw) == 0);
	atomic_store_explicit(&spaces[0].control->loop, 0, memory_order_release);
	CHECK(access_resume(&spaces[0], &event) == 0 && access_done(&spaces[0], &event, 0xabcde));
	CHECK(kobox_posix_vm_protect(&spaces[1], base, 4096, KOBOX_POSIX_MEMORY_READ) == 0);
	CHECK(access_begin(&spaces[1], base, 2, 0x99999, &event) == EPERM);
	CHECK(kobox_posix_vm_resume(&spaces[1]) == EPERM);
	CHECK(kobox_posix_vm_map(&spaces[1], base, 0, 4096, rw) == EPERM);
	CHECK(direct[0] == 0xaaa55);
	for (index = 0; index < 2; index++) {
		CHECK(kobox_posix_vm_destroy(&spaces[index]) == 0);
		CHECK(!spaces[index].pid && !spaces[index].control);
		CHECK(WIFSIGNALED(spaces[index].exit_status) &&
		      WTERMSIG(spaces[index].exit_status) == SIGKILL);
		errno = 0;
		CHECK(waitpid(pids[index], &status, WNOHANG) == -1 && errno == ECHILD);
		CHECK(kobox_posix_vm_map(&spaces[index], base, 0, 4096, rw) == ESRCH);
	}
	CHECK(munmap(direct, ram.size) == 0);
	CHECK(kobox_posix_memory_backing_destroy(&ram) == 0);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(exercise(argv[1], KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE) == 0);
	CHECK(exercise(argv[1], KOBOX_VM_TEST_WINDOW_BASE + (UINT64_C(1) << 32),
		64UL * 1024 * 1024) == 0);
	puts("POSIX two-process VM transport passed (not the Linux MM/VMA Gate)");
	return 0;
}
