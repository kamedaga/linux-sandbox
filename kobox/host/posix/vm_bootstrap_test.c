// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm.h"
#include "../../arch/x86_64/user_layout.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

extern const unsigned char kobox_vm_probe_begin[], kobox_vm_probe_end[];
extern const unsigned char kobox_vm_probe_spin[];

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "native bootstrap line %d: %s\n", \
			__LINE__, #expression); \
		return 1; \
	} \
} while (0)

static int empty_user_mm(pid_t pid)
{
	char path[96], line[512], permissions[5];
	unsigned long start, end;
	FILE *stream;
	unsigned int found = 0;

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	stream = fopen(path, "re");
	CHECK(stream);
	while (fgets(line, sizeof(line), stream)) {
		CHECK(sscanf(line, "%lx-%lx %4s", &start, &end, permissions) == 3);
		if (start >= KOBOX_X86_USER_END)
			continue;
		CHECK(start == KOBOX_X86_USER_START && end == KOBOX_X86_USER_END);
		CHECK(!strcmp(permissions, "---p"));
		found++;
	}
	CHECK(!ferror(stream) && !fclose(stream) && found == 1);
	return 0;
}

static int next_event(struct kobox_posix_vm *space,
		      struct kobox_posix_vm_event *event)
{
	CHECK(!kobox_posix_vm_resume(space));
	CHECK(!kobox_posix_vm_wait(space, false, event));
	return 0;
}

static int run(const char *bootstrap)
{
	const uint64_t code = 0x400000, data = KOBOX_X86_USER_END - 3 * 4096;
	const uint64_t top = KOBOX_X86_USER_END - 4096;
	const unsigned int rw = KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE;
	struct kobox_posix_memory_backing ram = {0};
	struct kobox_posix_vm root = {0}, peer = {0}, second = {0};
	struct kobox_posix_vm replacement = {0}, rejected = {0};
	struct kobox_posix_vm_event event;
	struct kobox_x86_user_regs frame, before, after;
	struct kobox_x86_fp_state fp;
	struct kobox_x86_fp_state bad_fp, after_fp;
	struct user_fpregs_struct native_fp;
	unsigned char *bytes;
	uint64_t *payload;
	pid_t pids[4];
	int status, result;

	CHECK(!kobox_posix_memory_backing_init(&ram, 5 * 4096));
	bytes = mmap(NULL, ram.size, PROT_READ | PROT_WRITE,
		     MAP_SHARED, ram.descriptor, 0);
	CHECK(bytes != MAP_FAILED);
	CHECK((size_t)(kobox_vm_probe_end - kobox_vm_probe_begin) < 4096);
	memcpy(bytes, kobox_vm_probe_begin, kobox_vm_probe_end - kobox_vm_probe_begin);
	payload = (void *)(bytes + 4096);
	*payload = 0x11223344;
	CHECK(kobox_posix_vm_create(&rejected, bootstrap, &ram,
		KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE) == ECHILD);
	CHECK(!rejected.pid && !rejected.control && !rejected.group &&
	      !rejected.control_backing.initialized);
	result = kobox_posix_vm_create(&root, bootstrap, &ram,
		KOBOX_X86_USER_START, KOBOX_X86_USER_END - KOBOX_X86_USER_START);
	if (result)
		fprintf(stderr, "bootstrap create: %s\n", strerror(result));
	CHECK(!result);
	pids[0] = root.pid;
	CHECK(root.syscall_entry >= KOBOX_X86_USER_END);
	CHECK(root.control_address == KOBOX_X86_MACHINE_CONTEXT);
	CHECK(!empty_user_mm(root.pid));
	CHECK(!kobox_posix_vm_read_registers(&root, &before));
	CHECK(!kobox_posix_vm_read_fpregs(&root, &fp));
	frame = before;
	frame.ip = code;
	frame.sp = top;
	frame.r12 = data;
	frame.fs_base = data;
	frame.gs_base = 0;
	frame.ds = frame.ss;
	frame.es = frame.ss;
	CHECK(kobox_posix_vm_start(&root, &frame, &fp) == EPERM);
	CHECK(!kobox_posix_vm_enable_syscalls(&root));
	frame.ip = KOBOX_X86_USER_END;
	CHECK(kobox_posix_vm_start(&root, &frame, &fp) == EINVAL);
	frame.ip = code;
	frame.gs_base = KOBOX_X86_USER_END;
	CHECK(kobox_posix_vm_start(&root, &frame, &fp) == EINVAL);
	frame.gs_base = 0;
	frame.cs = 0;
	CHECK(kobox_posix_vm_start(&root, &frame, &fp) == EINVAL);
	CHECK(!root.user_started);
	CHECK(!kobox_posix_vm_read_registers(&root, &after));
	CHECK(!memcmp(&before, &after, sizeof(before)));
	frame.cs = before.cs;
	/* Invalid MXCSR must roll back both banks, including the bootstrap
	 * register frame outside the client MM, without consuming READY.
	 */
	memcpy(&native_fp, &fp, sizeof(native_fp));
	native_fp.mxcsr |= 1U << 31;
	memcpy(&bad_fp, &native_fp, sizeof(bad_fp));
	CHECK(kobox_posix_vm_start(&root, &frame, &bad_fp) == EINVAL);
	CHECK(!root.user_started && !root.syscall_failed);
	CHECK(!kobox_posix_vm_read_registers(&root, &after));
	CHECK(!memcmp(&before, &after, sizeof(before)));
	CHECK(!kobox_posix_vm_read_fpregs(&root, &after_fp));
	CHECK(!memcmp(&fp, &after_fp, sizeof(fp)));
	CHECK(!kobox_posix_vm_start(&root, &frame, &fp));
	CHECK(kobox_posix_vm_start(&root, &frame, &fp) == EBUSY);
	/* The first instruction, first push and ordinary TLS load must each
	 * produce an actual machine fault in the initially empty user MM.
	 */
	CHECK(!next_event(&root, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == code &&
	      event.ip == code && event.sp == top && (event.error & 16));
	CHECK(!kobox_posix_vm_map(&root, code, 0, 4096,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_EXECUTE));
	/* Discard the native fault-handler frame, not the client FP state. */
	CHECK(!kobox_posix_vm_snapshot(&root, &before, &after_fp));
	CHECK(before.ip == code && before.sp == top && before.r12 == data);
	CHECK(!memcmp(&fp, &after_fp, sizeof(fp)));
	frame = before;
	frame.r13 ^= 0x11223344;
	CHECK(kobox_posix_vm_restore(&root, &frame, &bad_fp) == EINVAL);
	CHECK(!kobox_posix_vm_snapshot(&root, &after, &after_fp));
	CHECK(!memcmp(&before, &after, sizeof(before)));
	CHECK(!memcmp(&fp, &after_fp, sizeof(fp)));
	CHECK(!kobox_posix_vm_restore(&root, &frame, &fp));
	/* Interrupt CPU-only client code and observe its latest GPR/FP bank. */
	frame.ip = code + (kobox_vm_probe_spin - kobox_vm_probe_begin);
	frame.r13 = 0;
	memcpy(&native_fp, &fp, sizeof(native_fp));
	native_fp.xmm_space[0] = 0x12345678;
	memcpy(&after_fp, &native_fp, sizeof(after_fp));
	CHECK(!kobox_posix_vm_restore(&root, &frame, &after_fp));
	for (unsigned int round = 0; round < 32; round++) {
		CHECK(!kobox_posix_vm_resume(&root));
		usleep(1000);
		CHECK(!kobox_posix_vm_snapshot(&root, &after, &after_fp));
		if (after.r13)
			break;
	}
	CHECK(after.r13 && after.ip >= frame.ip &&
	      after.ip < code + (kobox_vm_probe_end - kobox_vm_probe_begin));
	memcpy(&native_fp, &after_fp, sizeof(native_fp));
	CHECK(!native_fp.xmm_space[0] && !root.running);
	frame.ip = code;
	CHECK(!kobox_posix_vm_restore(&root, &frame, &fp));
	CHECK(!next_event(&root, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == top - 8 &&
	      (event.error & 2));
	CHECK(!kobox_posix_vm_map(&root, top - 4096, 2 * 4096, 4096, rw));
	CHECK(!next_event(&root, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == data &&
	      !(event.error & 2));
	CHECK(!kobox_posix_vm_map(&root, data, 4096, 4096, rw));
	CHECK(!next_event(&root, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_SYSCALL &&
	      event.syscall.number == SYS_getpid && event.syscall.arguments[0] == *payload);
	frame = event.user;
	/* A shared peer keeps the old MM while an independent native context
	 * can be cleared and rebound for exec. Neither action chooses a Linux
	 * task or changes its files, credentials or process identity.
	 */
	CHECK(!kobox_posix_vm_clone(&root, event.syscall.sequence, true, &peer));
	CHECK(peer.control_address >= KOBOX_X86_MACHINE_ALLOC &&
	      peer.control_address < KOBOX_X86_MACHINE_END);
	CHECK(!kobox_posix_vm_clone(&root, event.syscall.sequence, true, &second));
	CHECK(second.control_address >= peer.control_address + KOBOX_VM_CLIENT_CONTEXT_SIZE &&
	      second.control_address < KOBOX_X86_MACHINE_END);
	CHECK(!kobox_posix_vm_clone(&root, event.syscall.sequence, false, &replacement));
	pids[1] = peer.pid;
	pids[2] = replacement.pid;
	pids[3] = second.pid;
	CHECK(!kobox_posix_vm_reset(&replacement, KOBOX_X86_USER_START,
		KOBOX_X86_USER_END - KOBOX_X86_USER_START));
	CHECK(!empty_user_mm(replacement.pid));
	CHECK(!kobox_posix_vm_map(&replacement, code, 0, 4096,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_EXECUTE));
	CHECK(!kobox_posix_vm_map(&replacement, data, 3 * 4096, 4096, rw));
	CHECK(!kobox_posix_vm_map(&replacement, top - 4096, 4 * 4096, 4096, rw));
	*(uint64_t *)(bytes + 5 * 4096 - 8) = data;
	CHECK(!kobox_posix_vm_destroy(&root));
	frame.ax = 0x55667788;
	CHECK(!kobox_posix_vm_syscall_return(&peer, event.syscall.sequence, &frame));
	CHECK(!next_event(&peer, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_SYSCALL && event.syscall.number == SYS_exit_group);
	CHECK(*payload == frame.ax && !*(uint64_t *)(bytes + 3 * 4096));
	frame.ax = 0xaabbccdd;
	CHECK(!kobox_posix_vm_syscall_return(&replacement, replacement.syscall_sequence, &frame));
	CHECK(!next_event(&replacement, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_SYSCALL && event.syscall.number == SYS_exit_group);
	CHECK(*(uint64_t *)(bytes + 3 * 4096) == frame.ax && *payload == 0x55667788);
	CHECK(!kobox_posix_vm_destroy(&peer));
	frame.ax = 0x12345678;
	CHECK(!kobox_posix_vm_syscall_return(&second, second.syscall_sequence, &frame));
	CHECK(!next_event(&second, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_SYSCALL && event.syscall.number == SYS_exit_group);
	CHECK(*payload == frame.ax && *(uint64_t *)(bytes + 3 * 4096) == 0xaabbccdd);
	CHECK(!kobox_posix_vm_destroy(&second));
	CHECK(!kobox_posix_vm_destroy(&replacement));
	for (unsigned int i = 0; i < 4; i++) {
		errno = 0;
		CHECK(waitpid(pids[i], &status, WNOHANG) == -1 && errno == ECHILD);
	}
	CHECK(!munmap(bytes, ram.size));
	CHECK(!kobox_posix_memory_backing_destroy(&ram));
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(!run(argv[1]));
	puts("native bootstrap: empty MM, instruction/stack/TLS faults, shared and replacement MM passed");
	return 0;
}
