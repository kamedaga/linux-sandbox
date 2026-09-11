// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm_service.h"

#include <errno.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "VM service check failed at line %d: %s\n", __LINE__, #expression); \
		return 1; \
	} \
} while (0)

static void notify(void *context)
{
	if (sem_post(context))
		__builtin_trap();
}

static int await_event(struct kobox_posix_vm_remote *remote, sem_t *ready,
	struct kobox_posix_vm_completion *event)
{
	struct timespec deadline;
	int result;

	if (clock_gettime(CLOCK_REALTIME, &deadline))
		return errno;
	deadline.tv_sec += 5;
	while ((result = kobox_posix_vm_remote_event(remote, event)) == EAGAIN)
		if (sem_timedwait(ready, &deadline) && errno != EINTR)
			return errno;
	return result ? result : event->error;
}

static int syscall_rounds(struct kobox_posix_vm_service *service,
			  struct kobox_posix_memory_backing *ram, const char *client,
			  sem_t *ready)
{
	struct kobox_posix_vm_remote *remote = NULL;
	struct kobox_posix_vm_completion event;
	uint64_t sequence = 0, syscall_sequence = 0;
	struct kobox_x86_user_regs registers;
	struct kobox_x86_fp_state fp;
	const uint64_t arguments[6] = {3, UINT64_MAX, 0x1122334455667788ULL,
		0, 99, KOBOX_VM_TEST_WINDOW_BASE};
	unsigned int round;
	pid_t pid;
	int result;

	result = kobox_posix_vm_remote_create(service, client, ram,
		KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE, &remote, &pid);
	if (result)
		fprintf(stderr, "syscall client create: %s (%d)\n", strerror(result), result);
	CHECK(!result);
	CHECK(kobox_posix_vm_remote_syscall_probe(remote, SYS_close, arguments, 0) == EPERM);
	CHECK(kobox_posix_vm_remote_syscall_probe(remote, SYS_close, NULL, 0) == EINVAL);
	CHECK(!kobox_posix_vm_remote_enable_syscalls(remote));
	for (round = 0; round < 32; round++) {
		int64_t value = round & 1 ? -EBADF : 0x112233445566LL;
		uint64_t number = round & 1 ? SYS_writev : SYS_close;

		CHECK(!kobox_posix_vm_remote_syscall_probe(remote, number, arguments, sequence));
		CHECK(!await_event(remote, ready, &event));
		CHECK(event.event.kind == KOBOX_POSIX_VM_SYSCALL &&
		      event.event.syscall.number == number && event.sequence == sequence + 1);
		CHECK(!memcmp(event.event.syscall.arguments, arguments, sizeof(arguments)));
		sequence = event.sequence;
		CHECK(kobox_posix_vm_remote_syscall_probe(remote, number, arguments, sequence - 1) == ESTALE);
		CHECK(event.event.syscall.sequence == syscall_sequence + 1);
		syscall_sequence = event.event.syscall.sequence;
		CHECK(kobox_posix_vm_remote_snapshot(remote, sequence - 1, &registers, &fp) == ESTALE);
		CHECK(!kobox_posix_vm_remote_snapshot(remote, sequence, &registers, &fp));
		CHECK(registers.ip == event.event.user.ip && registers.sp == event.event.user.sp);
		CHECK(kobox_posix_vm_remote_restore(remote, sequence, &registers, &fp) == EBUSY);
		CHECK(kobox_posix_vm_remote_resume(remote, sequence) == EBUSY);
		CHECK(kobox_posix_vm_remote_syscall_return(remote, sequence - 1,
							 syscall_sequence, &event.event.user) == ESTALE);
		event.event.user.ax = value;
		CHECK(!kobox_posix_vm_remote_syscall_return(remote, sequence,
			syscall_sequence, &event.event.user));
		CHECK(kobox_posix_vm_remote_syscall_return(remote, sequence,
							 syscall_sequence, &event.event.user) == ESTALE);
		CHECK(!kobox_posix_vm_remote_snapshot(remote, sequence, &registers, &fp));
		CHECK((int64_t)registers.ax == value);
		CHECK(kobox_posix_vm_remote_restore(remote, sequence - 1, &registers, &fp) == ESTALE);
		CHECK(!kobox_posix_vm_remote_restore(remote, sequence, &registers, &fp));
		CHECK(!kobox_posix_vm_remote_resume(remote, sequence));
		CHECK(!await_event(remote, ready, &event));
		CHECK(event.event.kind == KOBOX_POSIX_VM_STOP && event.value == (uint64_t)value &&
		      event.sequence == sequence + 1);
		sequence = event.sequence;
	}
	CHECK(!kobox_posix_vm_remote_close(remote));
	CHECK(kobox_posix_vm_remote_syscall_return(remote, sequence, syscall_sequence,
		&event.event.user) == ESRCH);
	return 0;
}

static int fork_rounds(struct kobox_posix_vm_service *service,
		       struct kobox_posix_memory_backing *ram, const char *client, sem_t *ready)
{
	struct kobox_posix_vm_remote *parent = NULL, *child = NULL;
	struct kobox_posix_vm_completion event;
	struct kobox_x86_user_regs registers;
	struct kobox_x86_fp_state fp;
	const uint64_t arguments[6] = {0};
	uint64_t syscall_sequence;
	pid_t parent_pid, child_pid;

	CHECK(!kobox_posix_vm_remote_create(service, client, ram,
		KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE, &parent, &parent_pid));
	CHECK(!kobox_posix_vm_remote_enable_syscalls(parent));
	CHECK(!kobox_posix_vm_remote_syscall_probe(parent, SYS_fork, arguments, 0));
	CHECK(!await_event(parent, ready, &event));
	CHECK(event.event.kind == KOBOX_POSIX_VM_SYSCALL && event.sequence == 1);
	registers = event.event.user;
	syscall_sequence = event.event.syscall.sequence;
	CHECK(kobox_posix_vm_remote_clone(parent, 0, syscall_sequence, false, &child, &child_pid, &fp) == ESTALE);
	CHECK(!child);
	CHECK(!kobox_posix_vm_remote_clone(parent, 1, syscall_sequence, false, &child, &child_pid, &fp));
	CHECK(child_pid > 0 && child_pid != parent_pid);
	CHECK(kobox_posix_vm_remote_resume(child, 0) == EBUSY);
	registers.ax = 0;
	CHECK(!kobox_posix_vm_remote_syscall_return(child, 0, syscall_sequence, &registers));
	CHECK(!kobox_posix_vm_remote_close(parent));
	CHECK(!kobox_posix_vm_remote_resume(child, 0));
	CHECK(!await_event(child, ready, &event));
	CHECK(event.event.kind == KOBOX_POSIX_VM_STOP && event.sequence == 1 && !event.value);
	CHECK(kobox_posix_vm_service_destroy(service) == EBUSY);
	CHECK(!kobox_posix_vm_remote_close(child));
	return 0;
}

struct syscall_exit_race {
	struct kobox_posix_vm_remote *remote;
	pthread_barrier_t start;
	uint64_t sequence, syscall_sequence;
	struct kobox_x86_user_regs registers;
	int close_result, reply_result;
};

static void race_start(struct syscall_exit_race *race)
{
	int result = pthread_barrier_wait(&race->start);

	if (result && result != PTHREAD_BARRIER_SERIAL_THREAD)
		__builtin_trap();
}

static void *close_pending_syscall(void *argument)
{
	struct syscall_exit_race *race = argument;

	race_start(race);
	race->close_result = kobox_posix_vm_remote_close(race->remote);
	return NULL;
}

static void *reply_pending_syscall(void *argument)
{
	struct syscall_exit_race *race = argument;

	race_start(race);
	race->reply_result = kobox_posix_vm_remote_syscall_return(race->remote,
		race->sequence, race->syscall_sequence, &race->registers);
	return NULL;
}

static int syscall_exit_rounds(struct kobox_posix_vm_service *service,
			       struct kobox_posix_memory_backing *ram,
			       const char *client, sem_t *ready)
{
	struct kobox_posix_vm_remote *previous = NULL;
	unsigned int round;

	for (round = 0; round < 32; round++) {
		struct syscall_exit_race race = {0};
		struct kobox_posix_vm_completion event;
		pthread_t closer, replier;
		pid_t pid;
		int status;

		CHECK(!kobox_posix_vm_remote_create(service, client, ram,
				KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE,
						   &race.remote, &pid));
		CHECK(!kobox_posix_vm_remote_enable_syscalls(race.remote));
		CHECK(!kobox_posix_vm_remote_probe(race.remote, 0, 4, 0, 0));
		CHECK(!await_event(race.remote, ready, &event));
		CHECK(event.event.kind == KOBOX_POSIX_VM_SYSCALL);
		race.sequence = event.sequence;
		race.syscall_sequence = event.event.syscall.sequence;
		race.registers = event.event.user;
		race.registers.ax = -EBADF;
		/* Identical counters on a new process do not revive an old
		 * remote. Its native child has already been killed and reaped.
		 */
		if (previous)
			CHECK(kobox_posix_vm_remote_syscall_return(previous,
				race.sequence, race.syscall_sequence, &race.registers) == ESRCH);
		CHECK(!pthread_barrier_init(&race.start, NULL, 2));
		CHECK(!pthread_create(&closer, NULL, close_pending_syscall, &race));
		CHECK(!pthread_create(&replier, NULL, reply_pending_syscall, &race));
		CHECK(!pthread_join(closer, NULL));
		CHECK(!pthread_join(replier, NULL));
		CHECK(!pthread_barrier_destroy(&race.start));
		CHECK(!race.close_result);
		/* A reply may win serialization, but must never revive a child
		 * after close acknowledges actual reaping.
		 */
		CHECK(!race.reply_result || race.reply_result == ESRCH);
		CHECK(kobox_posix_vm_remote_resume(race.remote, race.sequence) == ESRCH);
		CHECK(kobox_posix_vm_remote_syscall_return(race.remote,
			race.sequence, race.syscall_sequence, &race.registers) == ESRCH);
		CHECK(kobox_posix_vm_remote_event(race.remote, &event) == ESRCH);
		errno = 0;
		CHECK(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
		previous = race.remote;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct kobox_posix_vm_service *service = NULL;
	struct kobox_posix_vm_remote *remotes[2] = {0}, *failed = NULL;
	struct kobox_posix_memory_backing ram = {0};
	struct kobox_posix_vm_completion event;
	uint64_t sequences[2] = {0};
	pid_t pids[2], missing;
	sem_t ready;
	int index, status;

	CHECK(argc == 2 && sem_init(&ready, 0, 0) == 0);
	CHECK(kobox_posix_memory_backing_init(&ram, 4 * 4096) == 0);
	CHECK(kobox_posix_vm_service_create(&service, notify, &ready) == 0);
	CHECK(!kobox_posix_vm_service_quiescent(service));
	CHECK(!syscall_rounds(service, &ram, argv[1], &ready));
	CHECK(!fork_rounds(service, &ram, argv[1], &ready));
	CHECK(!syscall_exit_rounds(service, &ram, argv[1], &ready));
	CHECK(kobox_posix_vm_remote_create(service, "/nonexistent/kobox-client", &ram,
		KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE,
		&failed, &missing) == ENOENT && !failed);
	for (index = 0; index < 2; index++) {
		CHECK(kobox_posix_vm_remote_create(service, argv[1], &ram,
			KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE,
			&remotes[index], &pids[index]) == 0);
		CHECK(kobox_posix_vm_remote_probe(remotes[index], KOBOX_VM_TEST_WINDOW_BASE,
			index, 0x112233, 0) == 0);
	}
	CHECK(pids[0] != pids[1] && pids[0] != getpid() && pids[1] != getpid());
	CHECK(kobox_posix_vm_service_quiescent(service) == EBUSY);
	CHECK(kobox_posix_vm_service_destroy(service) == EBUSY);
	for (index = 0; index < 2; index++) {
		CHECK(await_event(remotes[index], &ready, &event) == 0);
		CHECK(event.event.kind == KOBOX_POSIX_VM_FAULT && event.sequence == 1 &&
			event.event.address == KOBOX_VM_TEST_WINDOW_BASE);
		sequences[index] = event.sequence;
		CHECK(kobox_posix_vm_remote_map(remotes[index], KOBOX_VM_TEST_WINDOW_BASE, 0, 4096, 3) == 0);
		CHECK(kobox_posix_vm_remote_resume(remotes[index], 0) == ESTALE);
	}
	CHECK(kobox_posix_vm_remote_resume(remotes[1], sequences[1]) == 0);
	CHECK(await_event(remotes[1], &ready, &event) == 0 &&
		event.event.kind == KOBOX_POSIX_VM_STOP && event.value == 0x112233);
	CHECK(kobox_posix_vm_remote_resume(remotes[0], sequences[0]) == 0);
	CHECK(await_event(remotes[0], &ready, &event) == 0 &&
		event.event.kind == KOBOX_POSIX_VM_STOP && event.value == 0x112233);
	/* Revoke while an external kill races observation of a stopped child. */
	CHECK(kill(pids[0], SIGKILL) == 0);
	CHECK(kobox_posix_vm_remote_reset(remotes[0], KOBOX_VM_TEST_WINDOW_BASE, 4096) == 0);
	CHECK(await_event(remotes[0], &ready, &event) == 0 &&
		event.event.kind == KOBOX_POSIX_VM_EXIT &&
		WIFSIGNALED(event.event.exit_status) && WTERMSIG(event.event.exit_status) == SIGKILL);
	CHECK(kobox_posix_vm_remote_event(remotes[0], &event) == ESRCH);
	for (index = 0; index < 2; index++) {
		CHECK(kobox_posix_vm_remote_close(remotes[index]) == 0);
		CHECK(kobox_posix_vm_remote_reset(remotes[index], KOBOX_VM_TEST_WINDOW_BASE, 4096) == 0);
		CHECK(kobox_posix_vm_remote_map(remotes[index], KOBOX_VM_TEST_WINDOW_BASE, 0, 4096, 3) == ESRCH);
		errno = 0;
		CHECK(waitpid(pids[index], &status, WNOHANG) == -1 && errno == ECHILD);
	}
	CHECK(!kobox_posix_vm_service_quiescent(service));
	CHECK(kobox_posix_vm_service_destroy(service) == 0);
	CHECK(kobox_posix_memory_backing_destroy(&ram) == 0);
	CHECK(sem_destroy(&ready) == 0);
	puts("POSIX async two-process VM service passed (not the Linux MM/VMA Gate)");
	return 0;
}
