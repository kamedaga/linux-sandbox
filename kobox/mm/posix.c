// SPDX-License-Identifier: GPL-2.0-only

#include "posix.h"
#include "../host/posix/vm_service.h"
#include "../task/posix_machine.h"

#include <errno.h>

static int map(void *space, uint64_t address, uint64_t physical,
	size_t size, unsigned int protection)
{
	unsigned int native = 0;

	if (protection & ~(KOBOX_VM_READ | KOBOX_VM_WRITE | KOBOX_VM_EXECUTE))
		return -EINVAL;
	if (protection & KOBOX_VM_READ)
		native |= KOBOX_POSIX_MEMORY_READ;
	if (protection & KOBOX_VM_WRITE)
		native |= KOBOX_POSIX_MEMORY_WRITE;
	if (protection & KOBOX_VM_EXECUTE)
		native |= KOBOX_POSIX_MEMORY_EXECUTE;
	return -kobox_posix_vm_remote_map(space, address, physical, size, native);
}

static int reset(void *space, uint64_t address, size_t size)
{
	return -kobox_posix_vm_remote_reset(space, address, size);
}

static int close(void *space)
{
	return -kobox_posix_vm_remote_close(space);
}

static int resume(void *space, uint64_t sequence)
{
	return -kobox_posix_vm_remote_resume(space, sequence);
}

static int clone_space(void *space, uint64_t sequence, uint64_t syscall_sequence,
		       bool share_mm, void **child, struct kobox_x86_fp_state *fp)
{
	struct kobox_posix_vm_remote *remote = NULL;
	pid_t pid;
	int result = kobox_posix_vm_remote_clone(space, sequence, syscall_sequence, share_mm,
					      &remote, &pid, fp);

	if (!result)
		*child = remote;
	return -result;
}

static int enable_syscalls(void *space)
{
	return -kobox_posix_vm_remote_enable_syscalls(space);
}

static int syscall_return(void *space, uint64_t sequence,
	uint64_t syscall_sequence, const struct kobox_x86_user_regs *registers)
{
	return -kobox_posix_vm_remote_syscall_return(space, sequence,
						  syscall_sequence, registers);
}

static int start(void *space, const struct kobox_x86_user_regs *registers,
		 const struct kobox_x86_fp_state *fp)
{
	return -kobox_posix_vm_remote_start(space, registers, fp);
}

static int write_fpregs(void *space, uint64_t sequence,
			const struct kobox_x86_fp_state *fp)
{
	return -kobox_posix_vm_remote_write_fpregs(space, sequence, fp);
}

static int snapshot(void *space, uint64_t sequence,
	struct kobox_x86_user_regs *registers, struct kobox_x86_fp_state *fp)
{
	return -kobox_posix_vm_remote_snapshot(space, sequence, registers, fp);
}

static int restore(void *space, uint64_t sequence,
	const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp)
{
	return -kobox_posix_vm_remote_restore(space, sequence, registers, fp);
}

static int event(void *space, struct kobox_linux_vm_event *event)
{
	struct kobox_posix_vm_completion native;
	int result = kobox_posix_vm_remote_event(space, &native);

	if (result)
		return -result;
	*event = (struct kobox_linux_vm_event) {
		.sequence = native.sequence, .value = native.value,
		.error = -native.error, .exit_status = native.event.exit_status,
		.user = native.event.user,
		.fault = {.address = native.event.address, .ip = native.event.ip,
			.sp = native.event.sp, .flags = native.event.flags, .error = native.event.error},
	};
	switch (native.event.kind) {
	case KOBOX_POSIX_VM_STOP:
		event->kind = KOBOX_VM_EVENT_STOP;
		break;
	case KOBOX_POSIX_VM_FAULT:
		event->kind = KOBOX_VM_EVENT_FAULT;
		break;
	case KOBOX_POSIX_VM_SYSCALL:
		event->kind = KOBOX_VM_EVENT_SYSCALL;
		event->syscall.number = native.event.syscall.number;
		event->syscall.sequence = native.event.syscall.sequence;
		for (unsigned int index = 0; index < 6; index++)
			event->syscall.arguments[index] = native.event.syscall.arguments[index];
		break;
	case KOBOX_POSIX_VM_EXIT:
		event->kind = KOBOX_VM_EVENT_EXIT;
		break;
	default:
		return -EPROTO;
	}
	return 0;
}

const struct kobox_linux_vm_host_operations kobox_vm_posix_operations = {
	.size = sizeof(kobox_vm_posix_operations),
	.map = map, .reset = reset, .close = close, .resume = resume, .event = event,
	.clone = clone_space,
	.enable_syscalls = enable_syscalls, .syscall_return = syscall_return,
	.start = start, .write_fpregs = write_fpregs,
	.snapshot = snapshot, .restore = restore,
};

void kobox_vm_posix_notify(void *context)
{
	(void)context;
	/* Routing CPU 0 does not choose the task; Linux's waitqueue wakeup and
	 * scheduler select its CPU and perform any required remote reschedule.
	 */
	if (kobox_task_posix_operations.cpu_notify(0, KOBOX_LINUX_TASK_VM_EVENT))
		__builtin_trap();
}

int kobox_vm_posix_probe(void *space, uint64_t address, unsigned int write,
	uint64_t value, uint64_t sequence)
{
	return -kobox_posix_vm_remote_probe(space, address, write, value, sequence);
}

int kobox_vm_posix_syscall_probe(void *space, uint64_t number,
	const uint64_t arguments[6], uint64_t sequence)
{
	return -kobox_posix_vm_remote_syscall_probe(space, number, arguments, sequence);
}
