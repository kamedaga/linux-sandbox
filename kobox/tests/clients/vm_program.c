// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm_program.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/futex.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

_Noreturn void kobox_vm_child_program(uint64_t base)
{
	static const char payload[] = "inherited-data";
	volatile unsigned char *bytes = (void *)(uintptr_t)(base + 128);
	volatile uint64_t *page = (void *)(uintptr_t)(base + 4096);
	uint64_t parent = *(volatile uint64_t *)(uintptr_t)(base + 256);
	bool thread = *(volatile unsigned int *)(uintptr_t)(base + 288);
	unsigned int exit_mode = *(volatile unsigned int *)(uintptr_t)(base + 320);
	uint64_t fd;
	unsigned int index;
	int status = 1;

	if (kobox_vm_program_syscall(SYS_sched_setaffinity, 0, sizeof(uint64_t), base + 264, 0, 0, 0) ||
	    kobox_vm_program_syscall(SYS_getcpu, base + 280, base + 284, 0, 0, 0, 0) ||
	    *(volatile unsigned int *)(uintptr_t)(base + 280) != 1)
		goto exit;
	if (thread) {
		while (!atomic_load_explicit((_Atomic unsigned int *)(uintptr_t)(base + 312),
					     memory_order_acquire))
			(void)kobox_vm_program_syscall(SYS_futex, base + 312, FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
		/* The exit-group Gate must also stop genuinely running userspace,
		 * not just a task already parked inside the Linux syscall path.
		 */
		if (exit_mode == 2)
			for (;;)
				atomic_fetch_add_explicit((_Atomic uint64_t *)(uintptr_t)(base + 328),
						  1, memory_order_relaxed);
		if (kobox_vm_program_syscall(SYS_getpid, 0, 0, 0, 0, 0, 0) != parent ||
		    kobox_vm_program_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0) == parent)
			goto exit;
	} else if (kobox_vm_program_syscall(SYS_getppid, 0, 0, 0, 0, 0, 0) != parent ||
		   kobox_vm_program_syscall(SYS_getpid, 0, 0, 0, 0, 0, 0) == parent) {
		goto exit;
	}
	if (kobox_vm_program_syscall(SYS_geteuid, 0, 0, 0, 0, 0, 0) != 1000 ||
	    kobox_vm_program_syscall(SYS_fcntl, 17, F_GETFD, 0, 0, 0, 0) != FD_CLOEXEC)
		goto exit;
	fd = kobox_vm_program_syscall(SYS_dup, 17, 0, 0, 0, 0, 0);
	if (fd >= (uint64_t)-4095 || fd == 17 || kobox_vm_program_syscall(SYS_close, 17, 0, 0, 0, 0, 0))
		goto exit;
	if (kobox_vm_program_syscall(SYS_pread64, fd, base + 128, sizeof(payload), 0, 0, 0) != sizeof(payload))
		goto exit;
	for (index = 0; index < sizeof(payload); index++)
		if (bytes[index] != (unsigned char)payload[index])
			goto exit;
	if (*page != 0x1122334455667788ULL)
		goto exit;
	if (kobox_vm_stack_access(base + 4096, 0xaabbccddeeff0011ULL) !=
	    0xaabbccddeeff0011ULL)
		goto exit;
	if (kobox_vm_program_syscall(SYS_lseek, fd, 7, SEEK_SET, 0, 0, 0) != 7 ||
	    kobox_vm_program_syscall(SYS_pwrite64, fd, base + 4096, sizeof(*page), 32, 0, 0) != sizeof(*page) ||
	    kobox_vm_program_syscall(SYS_pwrite64, fd, base + 280, sizeof(unsigned int), 40, 0, 0) != sizeof(unsigned int) ||
	    kobox_vm_program_syscall(SYS_close, fd, 0, 0, 0, 0, 0))
		goto exit;
	status = 0;
exit:
	if (thread)
		atomic_store_explicit((_Atomic unsigned int *)(uintptr_t)(base + 304),
				      status, memory_order_release);
	(void)kobox_vm_program_syscall(thread && exit_mode != 3 ? SYS_exit : SYS_exit_group,
		       status, 0, 0, 0, 0, 0);
	__builtin_trap();
}

_Noreturn void kobox_vm_root_program(uint64_t base, unsigned int cpu)
{
	const char payload[] = "inherited-data";
	volatile unsigned char *bytes = (void *)(uintptr_t)base;
	volatile uint64_t *page = (void *)(uintptr_t)(base + 4096);
	struct iovec vectors[2];
	uint64_t pid = kobox_vm_program_syscall(SYS_getpid, 0, 0, 0, 0, 0, 0);
	uint64_t fd, child;
	unsigned int index;
	int status = 1;

	/* Only getpid is handed off by the launcher. Every later syscall,
	 * fault, fork and wait is driven by this native user program itself.
	 */
	*(volatile uint64_t *)(uintptr_t)(base + 256) = pid;
	*(volatile uint64_t *)(uintptr_t)(base + 264) = 1ULL << cpu;
	if (kobox_vm_program_syscall(SYS_sched_setaffinity, 0, sizeof(uint64_t), base + 264, 0, 0, 0) ||
	    kobox_vm_program_syscall(SYS_getcpu, base + 344, 0, 0, 0, 0, 0) ||
	    *(volatile unsigned int *)(uintptr_t)(base + 344) != cpu ||
	    kobox_vm_program_syscall(SYS_setresuid, 1000, 1000, 1000, 0, 0, 0) ||
	    kobox_vm_program_syscall(SYS_geteuid, 0, 0, 0, 0, 0, 0) != 1000)
		goto exit;
	for (index = 0; index < sizeof(payload); index++)
		bytes[index] = payload[index];
	fd = kobox_vm_program_syscall(SYS_memfd_create, base, MFD_CLOEXEC, 0, 0, 0, 0);
	if (fd >= (uint64_t)-4095 || fd == 17 ||
	    kobox_vm_program_syscall(SYS_dup3, fd, 17, O_CLOEXEC, 0, 0, 0) != 17 ||
	    kobox_vm_program_syscall(SYS_close, fd, 0, 0, 0, 0, 0))
		goto exit;
	vectors[0] = (struct iovec){.iov_base = (void *)payload, .iov_len = 7};
	vectors[1] = (struct iovec){.iov_base = (void *)(payload + 7), .iov_len = 8};
	if (kobox_vm_program_syscall(SYS_writev, 17, (uintptr_t)vectors, 2, 0, 0, 0) != sizeof(payload))
		goto exit;
	vectors[0].iov_base = (void *)(uintptr_t)(base + 8 * 4096);
	if (kobox_vm_program_syscall(SYS_writev, 17, (uintptr_t)vectors, 1, 0, 0, 0) != (uint64_t)-EFAULT)
		goto exit;
	*page = 0x1122334455667788ULL;
	*(volatile uint64_t *)(uintptr_t)(base + 264) = 2;
	child = kobox_vm_program_syscall(SYS_fork, 0, 0, 0, 0, 0, 0);
	if (!child)
		kobox_vm_child_program(base);
	if (child >= (uint64_t)-4095 ||
	    kobox_vm_program_syscall(SYS_wait4, child, base + 304, 0, 0, 0, 0) != child ||
	    *(volatile unsigned int *)(uintptr_t)(base + 304) ||
	    *page != 0x1122334455667788ULL ||
	    kobox_vm_program_syscall(SYS_lseek, 17, 0, SEEK_CUR, 0, 0, 0) != 7 ||
	    kobox_vm_program_syscall(SYS_pread64, 17, base + 1024, 8, 32, 0, 0) != 8 ||
	    *(volatile uint64_t *)(uintptr_t)(base + 1024) != 0xaabbccddeeff0011ULL ||
	    kobox_vm_program_syscall(SYS_close, 17, 0, 0, 0, 0, 0) ||
	    kobox_vm_program_syscall(SYS_close, 17, 0, 0, 0, 0, 0) != (uint64_t)-EBADF)
		goto exit;
	*(volatile uint32_t *)(uintptr_t)(base + 336) = 0xc11e17;
	status = 0;
exit:
	(void)kobox_vm_program_syscall(SYS_exit_group, status, 0, 0, 0, 0, 0);
	__builtin_trap();
}
