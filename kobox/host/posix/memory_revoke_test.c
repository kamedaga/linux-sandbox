// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct progress {
	atomic_uint ready, revoked, faults, accesses;
};

static sigjmp_buf fault_return;
static volatile sig_atomic_t probing;
static volatile unsigned char *probe_address;

static void require(bool condition, unsigned int line)
{
	if (!condition) {
		fprintf(stderr, "memory revoke failed at %u (errno=%d)\n",
			line, errno);
		_exit(1);
	}
}
#define REQUIRE(condition) require(!!(condition), __LINE__)

static void bus_fault(int signal, siginfo_t *info, void *context)
{
	(void)context;
	if (signal != SIGBUS || !probing || info->si_code != BUS_ADRERR ||
	    info->si_addr != (void *)probe_address)
		_exit(2);
	probing = 0;
	siglongjmp(fault_return, 1);
}

static bool access_faults(volatile unsigned char *address, bool write)
{
	probe_address = address;
	if (sigsetjmp(fault_return, 1))
		return true;
	probing = 1;
	if (write)
		*address = 0x71;
	else
		(void)*address;
	probing = 0;
	return false;
}

static void send_fd(int socket, int descriptor)
{
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control = {0};
	char byte = 0;
	struct iovec iov = {.iov_base = &byte, .iov_len = 1};
	struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control)};
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);

	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
	REQUIRE(sendmsg(socket, &message, MSG_NOSIGNAL) == 1);
}

static int receive_fd(int socket)
{
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control = {0};
	char byte = 1;
	struct iovec iov = {.iov_base = &byte, .iov_len = 1};
	struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control)};
	struct cmsghdr *header;
	int descriptor;

	REQUIRE(recvmsg(socket, &message, MSG_CMSG_CLOEXEC) == 1);
	REQUIRE(!byte && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
	header = CMSG_FIRSTHDR(&message);
	REQUIRE(header && header->cmsg_level == SOL_SOCKET &&
		header->cmsg_type == SCM_RIGHTS &&
		header->cmsg_len == CMSG_LEN(sizeof(int)));
	memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
	REQUIRE(!CMSG_NXTHDR(&message, header));
	return descriptor;
}

static void *map(int descriptor, size_t size, int flags)
{
	void *address = mmap(NULL, size, PROT_READ | PROT_WRITE,
			     flags, descriptor, 0);

	REQUIRE(address != MAP_FAILED);
	return address;
}

static void peer(int socket, struct progress *progress, size_t size)
{
	struct sigaction action = {.sa_sigaction = bus_fault,
		.sa_flags = SA_SIGINFO};
	unsigned char *shared, *alias, *private, *cold, *fresh;
	struct stat old_stat, new_stat;
	int old_fd, duplicate, new_fd;
	unsigned int i;
	char byte;

	alarm(10);
	REQUIRE(!sigemptyset(&action.sa_mask));
	REQUIRE(!sigaction(SIGBUS, &action, NULL));
	old_fd = receive_fd(socket);
	REQUIRE(fcntl(old_fd, F_ADD_SEALS, F_SEAL_SHRINK) == -1 && errno == EPERM);
	duplicate = fcntl(old_fd, F_DUPFD_CLOEXEC, 0);
	REQUIRE(duplicate >= 0);
	shared = map(old_fd, size, MAP_SHARED);
	alias = map(duplicate, size, MAP_SHARED);
	private = map(old_fd, size, MAP_PRIVATE);
	REQUIRE(shared[0] == 0x42 && alias[0] == 0x42);
	private[0] = 0x24; /* A real pre-existing COW page must also be zapped. */
	REQUIRE(shared[0] == 0x42 && private[0] == 0x24);
	atomic_store_explicit(&progress->ready, 1, memory_order_release);
	while (!atomic_load_explicit(&progress->revoked, memory_order_acquire)) {
		bool fault = access_faults(shared, true);

		fault |= access_faults(private, false);
		cold = map(duplicate, size, MAP_SHARED);
		fault |= access_faults(cold, false);
		REQUIRE(!munmap(cold, size));
		atomic_fetch_add_explicit(&progress->accesses, 1, memory_order_relaxed);
		if (fault)
			atomic_fetch_add_explicit(&progress->faults, 1, memory_order_release);
	}
	for (i = 0; i < 32; i++) {
		REQUIRE(access_faults(shared, false));
		REQUIRE(access_faults(alias, true));
		REQUIRE(access_faults(private, false));
	}
	REQUIRE(ftruncate(duplicate, (off_t)size) == -1 && errno == EPERM);
	REQUIRE(pwrite(duplicate, "x", 1, 0) == -1 && errno == EPERM);
	REQUIRE(pread(duplicate, &byte, 1, 0) == 0);
	cold = map(duplicate, size, MAP_SHARED);
	REQUIRE(access_faults(cold, false));
	REQUIRE(!fstat(old_fd, &old_stat) && !old_stat.st_size);
	new_fd = receive_fd(socket);
	REQUIRE(!fstat(new_fd, &new_stat) && new_stat.st_size == (off_t)size);
	REQUIRE(old_stat.st_dev != new_stat.st_dev ||
		old_stat.st_ino != new_stat.st_ino);
	fresh = map(new_fd, size, MAP_SHARED);
	REQUIRE(fresh[0] == 0x93);
	REQUIRE(access_faults(shared, true) && access_faults(cold, false));
	REQUIRE(fresh[0] == 0x93);
	REQUIRE(!munmap(fresh, size) && !munmap(cold, size));
	REQUIRE(!munmap(private, size) && !munmap(alias, size));
	REQUIRE(!munmap(shared, size));
	REQUIRE(!close(old_fd) && !close(duplicate) && !close(new_fd));
	REQUIRE(!close(socket));
	_exit(0);
}

static void failed_revoke(size_t size)
{
	struct kobox_posix_memory_backing backing = {0};
	struct kobox_posix_memory_window window = {0};
	unsigned char *alias;
	void *rejected = NULL;
	struct stat state;

	/* An imported, non-revocable capability must never count as proof.
	 * Normal backing_init seals out this loss of owner authority.
	 */
	backing.descriptor = memfd_create("non-revocable", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	REQUIRE(backing.descriptor >= 0 && !ftruncate(backing.descriptor, (off_t)size));
	backing.size = size;
	backing.initialized = true;
	alias = map(backing.descriptor, size, MAP_SHARED);
	alias[0] = 0x61;
	/* A real host refusal, not a successful no-op implementation. */
	REQUIRE(!fcntl(backing.descriptor, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL));
	REQUIRE(kobox_posix_memory_backing_revoke(&backing) == EPERM);
	REQUIRE(backing.revoked && alias[0] == 0x61);
	REQUIRE(!fstat(backing.descriptor, &state) && state.st_size == (off_t)size);
	REQUIRE(!kobox_posix_memory_window_init(&window, size));
	REQUIRE(kobox_posix_memory_window_map(&window, 0, &backing, 0, size,
		KOBOX_POSIX_MEMORY_READ, &rejected) == ESTALE);
	REQUIRE(!rejected);
	/* Keep ownership until the sole alias is explicitly gone. No reuse
	 * is justified by the failed revoke or by merely closing the FD.
	 */
	REQUIRE(!munmap(alias, size));
	REQUIRE(!kobox_posix_memory_window_destroy(&window));
	REQUIRE(!kobox_posix_memory_backing_destroy(&backing));
}

int main(void)
{
	struct kobox_posix_memory_backing old = {0}, fresh = {0};
	struct progress *progress;
	unsigned char *parent;
	size_t size = (size_t)sysconf(_SC_PAGESIZE);
	int sockets[2], status;
	pid_t child;

	alarm(10);
	progress = mmap(NULL, sizeof(*progress), PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	REQUIRE(progress != MAP_FAILED);
	atomic_init(&progress->ready, 0);
	atomic_init(&progress->revoked, 0);
	atomic_init(&progress->accesses, 0);
	atomic_init(&progress->faults, 0);
	REQUIRE(atomic_is_lock_free(&progress->ready));
	REQUIRE(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets));
	child = fork();
	REQUIRE(child >= 0);
	if (!child) {
		REQUIRE(!close(sockets[0]));
		peer(sockets[1], progress, size);
	}
	REQUIRE(!close(sockets[1]));
	/* No inherited RAM FD or mapping: the independent peer gets a grant. */
	REQUIRE(!kobox_posix_memory_backing_init(&old, size));
	parent = map(old.descriptor, size, MAP_SHARED);
	parent[0] = 0x42;
	send_fd(sockets[0], old.descriptor);
	while (!atomic_load_explicit(&progress->ready, memory_order_acquire) ||
	       !atomic_load_explicit(&progress->accesses, memory_order_relaxed))
		sched_yield();
	/* No DMA users in this CPU-only conformance test. */
	REQUIRE(!kobox_posix_memory_backing_revoke(&old));
	REQUIRE(!kobox_posix_memory_backing_revoke(&old));
	while (!atomic_load_explicit(&progress->faults, memory_order_acquire))
		sched_yield();
	atomic_store_explicit(&progress->revoked, 1, memory_order_release);
	REQUIRE(!munmap(parent, size));
	REQUIRE(!kobox_posix_memory_backing_init(&fresh, size));
	parent = map(fresh.descriptor, size, MAP_SHARED);
	parent[0] = 0x93;
	send_fd(sockets[0], fresh.descriptor);
	REQUIRE(waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status) && !WEXITSTATUS(status));
	REQUIRE(parent[0] == 0x93);
	REQUIRE(!munmap(parent, size) && !close(sockets[0]));
	REQUIRE(!kobox_posix_memory_backing_destroy(&old));
	REQUIRE(!kobox_posix_memory_backing_destroy(&fresh));
	REQUIRE(!munmap(progress, sizeof(*progress)));
	failed_revoke(size);
	puts("Host RAM revoke: remote shared/COW aliases fault, stale FDs cannot "
	     "regrow or reach fresh RAM; failed invalidation rejected");
	return 0;
}
