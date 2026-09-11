// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../../boot/exec_gate.h"
#include <asm/prctl.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/auxvec.h>
#include <linux/futex.h>
#include <linux/memfd.h>
#include <linux/sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>

static unsigned long generation;
static unsigned long tls_word = 0x12345678;
static const char payload[] = "ELF user memory";
#ifdef KOBOX_ELF_SIGNALS
int kobox_elf_signal_test(void);
#endif
#ifdef KOBOX_ELF_VFORK
int kobox_elf_vfork_test(char **envp, char *cpu);
int kobox_elf_vfork_exec(void);
#endif

struct shared_result {
	struct kobox_exec_result record;
	unsigned long child;
	unsigned int ready, release;
	int sockets[2];
};

long kobox_elf_clone(unsigned long flags, void *stack,
		     struct shared_result *shared, unsigned long cpu);

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

static int fail(unsigned int line, long error)
{
	struct kobox_exec_failure failure = {
		.pid = call(SYS_getpid, 0, 0, 0, 0, 0, 0), .line = line, .error = error,
	};

	call(SYS_pwrite64, 19, (uintptr_t)&failure, sizeof(failure),
	     KOBOX_EXEC_FAILURE_OFFSET, 0, 0);
	return 1;
}

#define CHECK(condition) do { if (!(condition)) return fail(__LINE__, 0); } while (0)

static int wait_flag(unsigned int *flag)
{
	const struct timespec timeout = {.tv_sec = 5};
	long result;

	while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
		result = call(SYS_futex, (uintptr_t)flag, FUTEX_WAIT, 0,
			      (uintptr_t)&timeout, 0, 0);
		CHECK(!result || result == -EAGAIN || result == -EINTR);
	}
	return 0;
}

static int set_flag(unsigned int *flag)
{
	__atomic_store_n(flag, 1, __ATOMIC_RELEASE);
	CHECK(call(SYS_futex, (uintptr_t)flag, FUTEX_WAKE, 1, 0, 0, 0) >= 0);
	return 0;
}

static int send_rights(int socket)
{
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(2 * sizeof(int))];
	} control = {0};
	struct iovec vector = {.iov_base = (void *)payload, .iov_len = sizeof(payload)};
	struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);
	int *files = (void *)CMSG_DATA(header);
	long fd;

	fd = call(SYS_memfd_create, (uintptr_t)payload, MFD_CLOEXEC, 0, 0, 0, 0);
	CHECK(fd >= 0);
	CHECK(call(SYS_write, fd, (uintptr_t)payload, sizeof(payload), 0, 0, 0) == sizeof(payload));
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(2 * sizeof(int));
	files[0] = 18;
	files[1] = fd;
	CHECK(call(SYS_sendmsg, socket, (uintptr_t)&message, 0, 0, 0, 0) == sizeof(payload));
	CHECK(!call(SYS_close, fd, 0, 0, 0, 0, 0));
	return 0;
}

static int receive_rights(int socket)
{
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(2 * sizeof(int))];
	} control = {0};
	char bytes[sizeof(payload)] = {0}, contents[sizeof(payload)] = {0};
	struct iovec vector = {.iov_base = bytes, .iov_len = sizeof(bytes)};
	struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
	struct cmsghdr *header;
	int *files;
	unsigned int i;

	CHECK(call(SYS_recvmsg, socket, (uintptr_t)&message, MSG_CMSG_CLOEXEC, 0, 0, 0) == sizeof(payload));
	CHECK(!(message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)));
	header = CMSG_FIRSTHDR(&message);
	CHECK(header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
	CHECK(header->cmsg_len == CMSG_LEN(2 * sizeof(int)) && !CMSG_NXTHDR(&message, header));
	files = (void *)CMSG_DATA(header);
	CHECK(files[0] >= 0 && files[1] >= 0 && files[0] != files[1]);
	CHECK(call(SYS_fcntl, files[0], F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC);
	CHECK(call(SYS_fcntl, files[1], F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC);
	CHECK(call(SYS_pread64, files[1], (uintptr_t)contents, sizeof(contents), 0, 0, 0) == sizeof(contents));
	for (i = 0; i < sizeof(payload); i++)
		CHECK(bytes[i] == payload[i] && contents[i] == payload[i]);
	/* Replace the inherited reference with the one queued by the now-dead peer. */
	CHECK(!call(SYS_close, 17, 0, 0, 0, 0, 0));
	CHECK(call(SYS_dup3, files[0], 17, 0, 0, 0, 0) == 17);
	CHECK(!call(SYS_close, files[0], 0, 0, 0, 0, 0));
	CHECK(call(SYS_close, files[0], 0, 0, 0, 0, 0) == -EBADF);
	CHECK(!call(SYS_close, files[1], 0, 0, 0, 0, 0));
	return 0;
}

int kobox_elf_peer(struct shared_result *shared, unsigned long parent_cpu)
{
	unsigned long mask = 1UL << (parent_cpu ^ 1), observed;
	unsigned int cpu = 99;
	int result;

	CHECK(!call(SYS_sched_setaffinity, 0, sizeof(mask), (uintptr_t)&mask, 0, 0, 0));
	CHECK(!call(SYS_getcpu, (uintptr_t)&cpu, 0, 0, 0, 0, 0));
	CHECK(cpu == (parent_cpu ^ 1));
	CHECK(!call(SYS_close, shared->sockets[0], 0, 0, 0, 0, 0));
	CHECK(!set_flag(&shared->ready));
	CHECK(!wait_flag(&shared->release));
	/* Exec must replace only the parent MM, not this surviving CLONE_VM peer. */
	CHECK(generation == 99);
	__asm__ volatile("movq %%fs:0, %0" : "=r"(observed));
	CHECK(observed == tls_word);
	CHECK(call(SYS_getppid, 0, 0, 0, 0, 0, 0) == (long)shared->record.pid);
	CHECK(call(SYS_geteuid, 0, 0, 0, 0, 0, 0) == 1000);
	CHECK(call(SYS_fcntl, 18, F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC);
	result = send_rights(shared->sockets[1]);
	if (result)
		return result;
	CHECK(!call(SYS_close, shared->sockets[1], 0, 0, 0, 0, 0));
	CHECK(!call(SYS_close, 18, 0, 0, 0, 0, 0));
	CHECK(!call(SYS_close, 17, 0, 0, 0, 0, 0));
	CHECK(!call(SYS_munmap, (uintptr_t)shared, 4096, 0, 0, 0, 0));
	return 0;
}

static struct shared_result *map_result(void)
{
	return (void *)call(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
			   MAP_SHARED, 17, 0);
}

int kobox_elf_client_test(unsigned long *stack)
{
	char **argv = (void *)(stack + 1), **envp = argv + stack[0] + 1;
	char *next[] = {"/client", "again", argv[2], 0};
	struct kobox_exec_result record = {0};
	struct iovec vectors[2] = {
		{.iov_base = (void *)payload, .iov_len = 7},
		{.iov_base = (void *)(payload + 7), .iov_len = sizeof(payload) - 7},
	};
	unsigned long *auxv, mask, fs = ~0UL, observed, page_size = 0, entry = 0;
	long pid = call(SYS_getpid, 0, 0, 0, 0, 0, 0), fd;
	unsigned int cpu = 99;

	if (stack[0] != 3)
		return fail(__LINE__, stack[0]);
	if (argv[3])
		return fail(__LINE__, (uintptr_t)argv[3]);
	if (!envp[0] || envp[1])
		return fail(__LINE__, (uintptr_t)envp[1]);
	CHECK(envp[0][0] == 'K' && envp[0][6] == 'e');
	auxv = (void *)(envp + 2);
	for (; auxv[0] != AT_NULL; auxv += 2) {
		if (auxv[0] == AT_PAGESZ)
			page_size = auxv[1];
		if (auxv[0] == AT_ENTRY)
			entry = auxv[1];
	}
	CHECK(page_size == 4096 && entry && !generation);
	CHECK(argv[2][0] == '0' || argv[2][0] == '1');
	mask = 1UL << (argv[2][0] - '0');
	CHECK(!call(SYS_sched_setaffinity, 0, sizeof(mask), (uintptr_t)&mask, 0, 0, 0));
	CHECK(!call(SYS_getcpu, (uintptr_t)&cpu, 0, 0, 0, 0, 0));
	CHECK(cpu == (unsigned int)(argv[2][0] - '0'));
	CHECK(!call(SYS_arch_prctl, ARCH_GET_FS, (uintptr_t)&fs, 0, 0, 0, 0) && !fs);
#ifdef KOBOX_ELF_VFORK
	if (argv[1][0] == 'v')
		return kobox_elf_vfork_exec();
#endif
	if (argv[1][0] == 'a') {
		struct shared_result *shared;
		unsigned long child;
		int status = -1;

		CHECK(call(SYS_geteuid, 0, 0, 0, 0, 0, 0) == 1000);
		CHECK(call(SYS_pread64, 17, (uintptr_t)&record, sizeof(record), 0, 0, 0) == sizeof(record));
		CHECK(record.pid == (unsigned long)pid && record.phase == 1 && record.cpu == cpu);
		CHECK(call(SYS_fcntl, 18, F_GETFD, 0, 0, 0, 0) == -EBADF);
		shared = map_result();
		CHECK((unsigned long)shared < (unsigned long)-4095);
		CHECK(__atomic_load_n(&shared->ready, __ATOMIC_ACQUIRE) == 1);
		child = shared->child;
		CHECK(child && child != (unsigned long)pid);
		CHECK(!set_flag(&shared->release));
		CHECK(call(SYS_wait4, child, (uintptr_t)&status, 0, 0, 0, 0) == (long)child);
		/* Preserve the peer's failure record, including its source line. */
		if (status)
			return 1;
		CHECK(!receive_rights(shared->sockets[0]));
		CHECK(!call(SYS_close, shared->sockets[0], 0, 0, 0, 0, 0));
		CHECK(!call(SYS_munmap, (uintptr_t)shared, 4096, 0, 0, 0, 0));
		record.phase = 2;
		CHECK(call(SYS_pwrite64, 17, (uintptr_t)&record, sizeof(record), 0, 0, 0) == sizeof(record));
		CHECK(!call(SYS_close, 17, 0, 0, 0, 0, 0));
		return 0;
	}
	CHECK(argv[1][0] == 'i');
#ifdef KOBOX_ELF_SIGNALS
	if (kobox_elf_signal_test())
		return 1;
#endif
#ifdef KOBOX_ELF_VFORK
	if (kobox_elf_vfork_test(envp, argv[2]))
		return 1;
#endif
	CHECK(!call(SYS_setresuid, 1000, 1000, 1000, 0, 0, 0));
	CHECK(call(SYS_dup3, 17, 18, O_CLOEXEC, 0, 0, 0) == 18);
	fd = call(SYS_memfd_create, (uintptr_t)payload, MFD_CLOEXEC, 0, 0, 0, 0);
	CHECK(fd >= 0);
	CHECK(call(SYS_writev, fd, (uintptr_t)vectors, 2, 0, 0, 0) == sizeof(payload));
	vectors[0].iov_base = (void *)(uintptr_t)1;
	CHECK(call(SYS_writev, fd, (uintptr_t)vectors, 1, 0, 0, 0) == -EFAULT);
	CHECK(!call(SYS_close, fd, 0, 0, 0, 0, 0));
	CHECK(call(SYS_execve, (uintptr_t)"/bad", (uintptr_t)next, (uintptr_t)envp, 0, 0, 0) == -ENOEXEC);
	CHECK(call(SYS_fcntl, 18, F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC);
	CHECK(call(SYS_getpid, 0, 0, 0, 0, 0, 0) == pid);
	CHECK(!call(SYS_arch_prctl, ARCH_SET_FS, (uintptr_t)&tls_word, 0, 0, 0, 0));
	__asm__ volatile("movq %%fs:0, %0" : "=r"(observed));
	CHECK(observed == tls_word);
	generation = 99;
	record = (struct kobox_exec_result){.pid = pid, .phase = 1, .cpu = cpu};
	CHECK(call(SYS_write, 17, (uintptr_t)&record, sizeof(record), 0, 0, 0) == sizeof(record));
	{
		struct shared_result *shared;
		long child, child_stack;

		CHECK(!call(SYS_ftruncate, 17, 4096, 0, 0, 0, 0));
		shared = map_result();
		CHECK((unsigned long)shared < (unsigned long)-4095);
		CHECK(!call(SYS_socketpair, AF_UNIX, SOCK_DGRAM, 0,
			    (uintptr_t)shared->sockets, 0, 0));
		child_stack = call(SYS_mmap, 0, 65536, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		CHECK((unsigned long)child_stack < (unsigned long)-4095);
		child = kobox_elf_clone(CLONE_VM | SIGCHLD,
			(void *)(child_stack + 65536), shared, cpu);
		CHECK(child > 0);
		CHECK(!call(SYS_close, shared->sockets[1], 0, 0, 0, 0, 0));
		shared->child = child;
		CHECK(!wait_flag(&shared->ready));
	}
	fd = call(SYS_execve, (uintptr_t)next[0], (uintptr_t)next, (uintptr_t)envp, 0, 0, 0);
	return fail(__LINE__, fd);
}
