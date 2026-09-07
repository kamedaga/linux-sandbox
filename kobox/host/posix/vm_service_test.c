// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm_service.h"

#include <errno.h>
#include <semaphore.h>
#include <stdio.h>
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
	CHECK(kobox_posix_vm_remote_create(service, "/nonexistent/kobox-client", &ram,
		&failed, &missing) == ENOENT && !failed);
	for (index = 0; index < 2; index++) {
		CHECK(kobox_posix_vm_remote_create(service, argv[1], &ram, &remotes[index], &pids[index]) == 0);
		CHECK(kobox_posix_vm_remote_probe(remotes[index], KOBOX_VM_WINDOW_BASE,
			index, 0x112233, 0) == 0);
	}
	CHECK(pids[0] != pids[1] && pids[0] != getpid() && pids[1] != getpid());
	CHECK(kobox_posix_vm_service_destroy(service) == EBUSY);
	for (index = 0; index < 2; index++) {
		CHECK(await_event(remotes[index], &ready, &event) == 0);
		CHECK(event.event.kind == KOBOX_POSIX_VM_FAULT && event.sequence == 1 &&
			event.event.address == KOBOX_VM_WINDOW_BASE);
		sequences[index] = event.sequence;
		CHECK(kobox_posix_vm_remote_map(remotes[index], KOBOX_VM_WINDOW_BASE, 0, 4096, 3) == 0);
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
	CHECK(kobox_posix_vm_remote_reset(remotes[0], KOBOX_VM_WINDOW_BASE, 4096) == 0);
	CHECK(await_event(remotes[0], &ready, &event) == 0 &&
		event.event.kind == KOBOX_POSIX_VM_EXIT &&
		WIFSIGNALED(event.event.exit_status) && WTERMSIG(event.event.exit_status) == SIGKILL);
	CHECK(kobox_posix_vm_remote_event(remotes[0], &event) == ESRCH);
	for (index = 0; index < 2; index++) {
		CHECK(kobox_posix_vm_remote_close(remotes[index]) == 0);
		CHECK(kobox_posix_vm_remote_reset(remotes[index], KOBOX_VM_WINDOW_BASE, 4096) == 0);
		CHECK(kobox_posix_vm_remote_map(remotes[index], KOBOX_VM_WINDOW_BASE, 0, 4096, 3) == ESRCH);
		errno = 0;
		CHECK(waitpid(pids[index], &status, WNOHANG) == -1 && errno == ECHILD);
	}
	CHECK(kobox_posix_vm_service_destroy(service) == 0);
	CHECK(kobox_posix_memory_backing_destroy(&ram) == 0);
	CHECK(sem_destroy(&ready) == 0);
	puts("POSIX async two-process VM service passed (not the Linux MM/VMA Gate)");
	return 0;
}
