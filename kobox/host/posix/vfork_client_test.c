// SPDX-License-Identifier: GPL-2.0-only

#include "../../boot/exec_gate.h"
#include <asm/unistd.h>
#include <linux/mman.h>
#include <linux/resource.h>
#include <linux/signal.h>
#include <linux/time_types.h>
#include <errno.h>

struct vfork_state {
	unsigned int returned, phase, mode;
	unsigned long pid;
	unsigned long *kill_target;
	char **envp;
	char *argv[4];
};

long kobox_elf_vfork(struct vfork_state *state);
void kobox_elf_vfork_child(struct vfork_state *state);
int kobox_elf_vfork_test(char **envp, char *cpu);
int kobox_elf_vfork_exec(void);

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
	};

	call(__NR_pwrite64, 19, (uintptr_t)&failure, sizeof(failure),
	     KOBOX_EXEC_FAILURE_OFFSET, 0, 0);
	return 1;
}

#define CHECK(condition) do { if (!(condition)) return fail(__LINE__); } while (0)

static _Noreturn void child_exit(int result)
{
	call(__NR_exit, result, 0, 0, 0, 0, 0);
	__builtin_trap();
}

static int child_run(struct vfork_state *state)
{
	const struct __kernel_timespec delay = {.tv_nsec = 1000000};
	unsigned long parent = call(__NR_getppid, 0, 0, 0, 0, 0, 0);

	state->pid = call(__NR_getpid, 0, 0, 0, 0, 0, 0);
	CHECK(state->pid != parent);
	state->phase = 1;
	if (state->mode == 1) {
		CHECK(call(__NR_execve, (uintptr_t)"/bad", (uintptr_t)state->argv,
			   (uintptr_t)state->envp, 0, 0, 0) == -ENOEXEC);
		state->phase = 2;
	}
	/* Real timed sleeps yield both logical CPUs while the parent must
	 * remain inside upstream wait_for_vfork_done(), including failed exec.
	 */
	for (unsigned int iteration = 0; iteration < 8; iteration++) {
		CHECK(!call(__NR_nanosleep, (uintptr_t)&delay, 0, 0, 0, 0, 0));
		CHECK(!__atomic_load_n(&state->returned, __ATOMIC_ACQUIRE));
	}
	state->phase = 3;
	if (state->mode == 2) {
		call(__NR_execve, (uintptr_t)state->argv[0], (uintptr_t)state->argv,
		     (uintptr_t)state->envp, 0, 0, 0);
		return fail(__LINE__);
	}
	if (state->mode == 3) {
		__atomic_store_n(state->kill_target, state->pid, __ATOMIC_RELEASE);
		for (unsigned int iteration = 0; iteration < 2000; iteration++) {
			CHECK(!call(__NR_nanosleep, (uintptr_t)&delay, 0, 0, 0, 0, 0));
			CHECK(!__atomic_load_n(&state->returned, __ATOMIC_ACQUIRE));
		}
		return fail(__LINE__);
	}
	return 42;
}

void kobox_elf_vfork_child(struct vfork_state *state)
{
	child_exit(child_run(state));
}

int kobox_elf_vfork_exec(void)
{
	const struct __kernel_timespec delay = {.tv_nsec = 1000000};
	unsigned long pid = call(__NR_getpid, 0, 0, 0, 0, 0, 0);
	unsigned int returned = 0, attempts = 0;

	CHECK(call(__NR_pwrite64, 19, (uintptr_t)&pid, sizeof(pid), 1024, 0, 0) == sizeof(pid));
	/* Exec must release the parent without waiting for this child to exit. */
	while (!returned) {
		CHECK(attempts++ < 2000);
		CHECK(call(__NR_pread64, 19, (uintptr_t)&returned, sizeof(returned),
			   2048, 0, 0) == sizeof(returned));
		CHECK(!call(__NR_nanosleep, (uintptr_t)&delay, 0, 0, 0, 0, 0));
	}
	return 42;
}

static int creation_failure(struct vfork_state *state)
{
	struct rlimit limit = {.rlim_cur = 0, .rlim_max = 64};
	long child;
	int status;

	/* Use the actual upstream process limit, not a fake clone result. */
	CHECK(!call(__NR_setresuid, 1001, 1001, 1001, 0, 0, 0));
	CHECK(!call(__NR_setrlimit, RLIMIT_NPROC, (uintptr_t)&limit, 0, 0, 0, 0));
	for (unsigned int iteration = 0; iteration < 8; iteration++)
		CHECK(kobox_elf_vfork(state) == -EAGAIN);
	CHECK(!state->phase && !state->pid);
	limit.rlim_cur = 64;
	CHECK(!call(__NR_setrlimit, RLIMIT_NPROC, (uintptr_t)&limit, 0, 0, 0, 0));
	child = kobox_elf_vfork(state);
	CHECK(child > 0);
	CHECK(call(__NR_wait4, child, (uintptr_t)&status, 0, 0, 0, 0) == child);
	CHECK(status == (42 << 8) && state->phase == 3);
	return 0;
}

static int kill_child(unsigned long *target)
{
	const struct __kernel_timespec delay = {.tv_nsec = 1000000};
	unsigned int cpu, attempts = 0;
	unsigned long pid = 0, affinity;

	CHECK(!call(__NR_getcpu, (uintptr_t)&cpu, 0, 0, 0, 0, 0));
	affinity = 1UL << (cpu ^ 1);
	CHECK(!call(__NR_sched_setaffinity, 0, sizeof(affinity), (uintptr_t)&affinity, 0, 0, 0));
	while (!(pid = __atomic_load_n(target, __ATOMIC_ACQUIRE))) {
		CHECK(attempts++ < 2000);
		CHECK(!call(__NR_nanosleep, (uintptr_t)&delay, 0, 0, 0, 0, 0));
	}
	CHECK(!call(__NR_kill, pid, SIGKILL, 0, 0, 0, 0));
	return 0;
}

int kobox_elf_vfork_test(char **envp, char *cpu)
{
	struct vfork_state *state;
	unsigned long exec_pid, *target;
	long child, killer;
	int status;

	/* MAP_PRIVATE is deliberately shared only by vfork/CLONE_VM. */
	state = (void *)call(__NR_mmap, 0, 4096, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1UL, 0);
	CHECK((unsigned long)state < (unsigned long)-4095);
	target = (void *)call(__NR_mmap, 0, 4096, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_ANONYMOUS, -1UL, 0);
	CHECK((unsigned long)target < (unsigned long)-4095);
	for (unsigned int iteration = 0; iteration < 16; iteration++) {
		*state = (struct vfork_state) {
			.mode = iteration % 4, .envp = envp,
			.kill_target = target,
			.argv = {"/client", "vfork-exec", cpu, 0},
		};
		CHECK(call(__NR_pwrite64, 19, (uintptr_t)&state->returned,
			   sizeof(state->returned), 2048, 0, 0) == sizeof(state->returned));
		killer = 0;
		if (state->mode == 3) {
			__atomic_store_n(target, 0, __ATOMIC_RELEASE);
			killer = call(__NR_fork, 0, 0, 0, 0, 0, 0);
			CHECK(killer >= 0);
			if (!killer)
				child_exit(kill_child(target));
		}
		child = kobox_elf_vfork(state);
		__atomic_store_n(&state->returned, 1, __ATOMIC_RELEASE);
		CHECK(child > 0 && state->pid == (unsigned long)child && state->phase == 3);
		CHECK(call(__NR_pwrite64, 19, (uintptr_t)&state->returned,
			   sizeof(state->returned), 2048, 0, 0) == sizeof(state->returned));
		CHECK(call(__NR_wait4, child, (uintptr_t)&status, 0, 0, 0, 0) == child);
		CHECK(status == (state->mode == 3 ? SIGKILL : 42 << 8));
		if (killer)
			CHECK(call(__NR_wait4, killer, (uintptr_t)&status, 0, 0, 0, 0) == killer && !status);
		if (state->mode == 2) {
			CHECK(call(__NR_pread64, 19, (uintptr_t)&exec_pid, sizeof(exec_pid),
				   1024, 0, 0) == sizeof(exec_pid));
			CHECK(exec_pid == (unsigned long)child);
		}
	}
	*state = (struct vfork_state) {0};
	child = call(__NR_fork, 0, 0, 0, 0, 0, 0);
	CHECK(child >= 0);
	if (!child)
		child_exit(creation_failure(state));
	CHECK(call(__NR_wait4, child, (uintptr_t)&status, 0, 0, 0, 0) == child && !status);
	CHECK(!state->phase && !state->pid);
	CHECK(!call(__NR_munmap, (uintptr_t)state, 4096, 0, 0, 0, 0));
	CHECK(!call(__NR_munmap, (uintptr_t)target, 4096, 0, 0, 0, 0));
	return 0;
}
