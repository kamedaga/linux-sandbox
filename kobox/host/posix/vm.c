// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm.h"
#include "../../arch/x86_64/user_layout.h"

#include "../../arch/x86_64/linux_ptrace.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/audit.h>
#include <linux/sched.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TRACE_OPTIONS (PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD)

/* Native address-space ownership only; Linux owns task/group semantics. */
struct kobox_posix_vm_group {
	struct kobox_posix_vm *contexts;
};

static int join_group(struct kobox_posix_vm *space,
		      struct kobox_posix_vm_group *group)
{
	if (!group) {
		group = calloc(1, sizeof(*group));
		if (!group)
			return ENOMEM;
	}
	space->group = group;
	space->group_next = group->contexts;
	group->contexts = space;
	return 0;
}

static void leave_group(struct kobox_posix_vm *space)
{
	struct kobox_posix_vm **cursor;

	if (!space->group)
		return;
	for (cursor = &space->group->contexts; *cursor != space;
	     cursor = &(*cursor)->group_next)
		if (!*cursor)
			__builtin_trap();
	*cursor = space->group_next;
	if (!space->group->contexts)
		free(space->group);
	space->group = NULL;
	space->group_next = NULL;
}

static int owned(struct kobox_posix_vm *space)
{
	if (!space || space->owner != (pid_t)syscall(SYS_gettid))
		return EINVAL;
	return space->pid > 0 ? 0 : ESRCH;
}

static bool valid_range(const struct kobox_posix_vm *space, uint64_t address, size_t size)
{
	return space && size && !(address & 4095) && !(size & 4095) &&
		address >= space->window_start &&
		address - space->window_start < space->window_size &&
		size <= space->window_size - (address - space->window_start);
}

static bool full_user_space(const struct kobox_posix_vm *space)
{
	return space->window_start == KOBOX_X86_USER_START &&
		space->window_size == KOBOX_X86_USER_END - KOBOX_X86_USER_START;
}

static int native_protection(unsigned int protection, int *native)
{
	if (protection & ~(KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE |
			   KOBOX_POSIX_MEMORY_EXECUTE))
		return EINVAL;
	*native = (protection & KOBOX_POSIX_MEMORY_READ ? PROT_READ : 0) |
		(protection & KOBOX_POSIX_MEMORY_WRITE ? PROT_WRITE : 0) |
		(protection & KOBOX_POSIX_MEMORY_EXECUTE ? PROT_EXEC : 0);
	return 0;
}

static int wait_native(struct kobox_posix_vm *space, bool nonblock, int *status)
{
	pid_t result;

	do {
		result = waitpid(space->pid, status, __WALL | (nonblock ? WNOHANG : 0));
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		return errno;
	if (!result)
		return EAGAIN;
	space->running = false;
	if (WIFEXITED(*status) || WIFSIGNALED(*status)) {
		space->exit_status = *status;
		space->pid = 0;
	}
	return 0;
}

static int resume_native(struct kobox_posix_vm *space, int mode, int signal)
{
	if (ptrace(mode, space->pid, NULL, (void *)(uintptr_t)signal))
		return errno;
	space->running = true;
	return 0;
}

static int remote_syscall(struct kobox_posix_vm *space, unsigned long number,
			  const uint64_t args[6], uint64_t *value)
{
	struct user_regs_struct saved, regs;
	siginfo_t info;
	bool deferred_fault = false;
	int status, result;

	result = owned(space);
	if (result)
		return result;
	if (space->running)
		return EBUSY;
	if (ptrace(PTRACE_GETREGS, space->pid, NULL, &saved))
		return errno;
	regs = saved;
	kobox_x86_linux_syscall_prepare(&regs, space->syscall_entry, number, args);
	if (ptrace(PTRACE_SETREGS, space->pid, NULL, &regs))
		return errno;
	result = resume_native(space, PTRACE_CONT, 0);
	if (result)
		return result;
	for (;;) {
		result = wait_native(space, false, &status);
		if (result)
			return result;
		if (!space->pid)
			return ESRCH;
		if (!WIFSTOPPED(status))
			return EPROTO;
		if (ptrace(PTRACE_GETREGS, space->pid, NULL, &regs))
			return errno;
		if (WSTOPSIG(status) == SIGSEGV && !deferred_fault &&
		    kobox_x86_linux_ip(&regs) == space->syscall_entry) {
			if (ptrace(PTRACE_GETSIGINFO, space->pid, NULL, &info))
				return errno;
			if ((info.si_code != SEGV_MAPERR && info.si_code != SEGV_ACCERR) ||
			    (uintptr_t)info.si_addr < space->window_start ||
			    (uintptr_t)info.si_addr - space->window_start >= space->window_size)
				return EPROTO;
			/* INTERRUPT can stop just before an already-generated access
			 * fault is delivered. It belongs to saved, not the syscall
			 * trampoline (which has not executed). Suppress this delivery;
			 * restoring saved retries the original instruction against
			 * the completed mappings, producing a fresh real fault if
			 * still necessary. Never fabricate a trampoline fault frame.
			 */
			deferred_fault = true;
		} else if (WSTOPSIG(status) == SIGTRAP) {
			if ((unsigned int)status >> 16 != PTRACE_EVENT_STOP)
				break;
			/* An interrupt which lost to an earlier stop is still pending. */
		} else {
			return EPROTO;
		}
		result = resume_native(space, PTRACE_CONT, 0);
		if (result)
			return result;
	}
	if (!kobox_x86_linux_syscall_done(&regs, space->syscall_entry))
		return EPROTO;
	*value = kobox_x86_linux_result(&regs);
	if (ptrace(PTRACE_SETREGS, space->pid, NULL, &saved))
		return errno;
	if (*value >= (uint64_t)-4095)
		return -(int64_t)*value;
	return 0;
}

static int reap_context(struct kobox_posix_vm *space)
{
	int result, status;

	if (space->pid <= 0)
		return 0;
	if (kill(space->pid, SIGKILL) && errno != ESRCH)
		return errno;
	while (space->pid > 0) {
		result = wait_native(space, false, &status);
		if (result)
			return result;
		if (space->pid > 0 && ptrace(PTRACE_CONT, space->pid, NULL,
					   (void *)(uintptr_t)SIGKILL))
			return errno;
	}
	return 0;
}

static int context_mapping_syscall(struct kobox_posix_vm *space, unsigned long number,
			   const uint64_t args[6], uint64_t *value)
{
	bool running;
	int result = owned(space);

	if (result)
		return result;
	if (space->syscall_failed)
		return EPERM;
	running = space->running;
	result = kobox_posix_vm_pause(space);
	if (!result)
		result = remote_syscall(space, number, args, value);
	if (!result && running && !space->event_pending)
		result = kobox_posix_vm_resume(space);
	return result;
}

static int mapping_syscall(struct kobox_posix_vm *space, unsigned long number,
			   const uint64_t args[6], uint64_t *value)
{
	struct kobox_posix_vm *peer;
	int result;

	if (!space || space->owner != (pid_t)syscall(SYS_gettid))
		return EINVAL;
	if (!space->group)
		return ESRCH;
	/* MM operations survive one context's death. Reaping that context is
	 * not a TLB invalidation while another native CLONE_VM holder exists.
	 */
	for (peer = space->group->contexts; peer; peer = peer->group_next) {
		if (!peer->pid)
			continue;
		result = context_mapping_syscall(peer, number, args, value);
		if (result != ESRCH)
			return result;
		result = reap_context(peer);
		if (result)
			return result;
	}
	return ESRCH;
}

int kobox_posix_vm_map(struct kobox_posix_vm *space, uint64_t address,
		      size_t offset, size_t size, unsigned int protection)
{
	uint64_t args[6], value;
	int native, result;

	if (!space || space->owner != (pid_t)syscall(SYS_gettid))
		return EINVAL;
	if (!space->group)
		return ESRCH;
	if (!valid_range(space, address, size) || (offset & 4095) ||
	    offset > space->ram_size || size > space->ram_size - offset)
		return EINVAL;
	result = native_protection(protection, &native);
	if (result)
		return result;
	args[0] = address;
	args[1] = size;
	args[2] = native;
	args[3] = MAP_SHARED | MAP_FIXED;
	args[4] = KOBOX_VM_RAM_FD;
	args[5] = offset;
	result = mapping_syscall(space, SYS_mmap, args, &value);
	return result ? result : value == address ? 0 : EFAULT;
}

int kobox_posix_vm_reset(struct kobox_posix_vm *space, uint64_t address, size_t size)
{
	uint64_t args[6] = {address, size, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1UL, 0};
	uint64_t value;
	int result;

	if (!valid_range(space, address, size))
		return EINVAL;
	result = mapping_syscall(space, SYS_mmap, args, &value);
	return result ? result : value == address ? 0 : EFAULT;
}

int kobox_posix_vm_protect(struct kobox_posix_vm *space, uint64_t address,
			  size_t size, unsigned int protection)
{
	uint64_t args[6] = {address, size}, value;
	int native, result;

	if (!valid_range(space, address, size))
		return EINVAL;
	result = native_protection(protection, &native);
	if (result)
		return result;
	args[2] = native;
	result = mapping_syscall(space, SYS_mprotect, args, &value);
	return result ? result : value ? EIO : 0;
}

int kobox_posix_vm_resume(struct kobox_posix_vm *space)
{
	int result = owned(space);

	if (result)
		return result;
	if (space->syscall_failed)
		return EPERM;
	if (space->running || space->event_pending || space->syscall_pending)
		return EBUSY;
	return resume_native(space, PTRACE_SYSCALL, 0);
}

int kobox_posix_vm_enable_syscalls(struct kobox_posix_vm *space)
{
	int result = owned(space);

	if (result)
		return result;
	if (space->syscalls_enabled)
		return EALREADY;
	if (space->running || space->event_pending || space->capturing_fault ||
	    space->signal_return_allowed || !space->control ||
	    atomic_load_explicit(&space->control->event, memory_order_acquire) != KOBOX_VM_CLIENT_READY)
		return EBUSY;
	space->syscalls_enabled = true;
	return 0;
}

int kobox_posix_vm_read_registers(struct kobox_posix_vm *space,
	struct kobox_x86_user_regs *registers)
{
	struct user_regs_struct native;
	int result = owned(space);

	if (result)
		return result;
	if (!registers)
		return EINVAL;
	if (space->running)
		return EBUSY;
	if (ptrace(PTRACE_GETREGS, space->pid, NULL, &native))
		return errno;
	kobox_x86_linux_regs_export(registers, &native);
	return 0;
}

int kobox_posix_vm_read_fpregs(struct kobox_posix_vm *space,
	struct kobox_x86_fp_state *state)
{
	int result = owned(space);

	if (result)
		return result;
	if (!state)
		return EINVAL;
	if (space->running)
		return EBUSY;
	return kobox_x86_linux_fp_read(space->pid, state);
}

static int write_registers(struct kobox_posix_vm *space,
			   const struct kobox_x86_user_regs *registers)
{
	struct user_regs_struct regs;

	if (!registers)
		return EINVAL;
	if (full_user_space(space) &&
	    (registers->ip < space->window_start || registers->ip >= KOBOX_X86_USER_END ||
	     registers->sp < space->window_start || registers->sp > KOBOX_X86_USER_END ||
	     registers->fs_base >= KOBOX_X86_USER_END || registers->gs_base >= KOBOX_X86_USER_END))
		return EINVAL;
	if (ptrace(PTRACE_GETREGS, space->pid, NULL, &regs))
		return errno;
	if (!kobox_x86_linux_regs_import(&regs, registers))
		return EINVAL;
	if (ptrace(PTRACE_SETREGS, space->pid, NULL, &regs))
		return errno;
	return 0;
}

int kobox_posix_vm_write_fpregs(struct kobox_posix_vm *space,
	const struct kobox_x86_fp_state *state)
{
	int result = owned(space);

	if (result)
		return result;
	if (!state)
		return EINVAL;
	if (space->running)
		return EBUSY;
	return kobox_x86_linux_fp_write(space->pid, state);
}

int kobox_posix_vm_start(struct kobox_posix_vm *space,
	const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp)
{
	struct user_regs_struct saved;
	struct kobox_x86_fp_state saved_fp;
	int result = owned(space);

	if (result)
		return result;
	if (!space->syscalls_enabled || space->syscall_failed)
		return EPERM;
	if (!fp)
		return EINVAL;
	if (space->user_started || space->syscall_sequence || space->running ||
	    space->syscall_pending || space->event_pending || space->capturing_fault ||
	    space->signal_return_allowed ||
	    atomic_load_explicit(&space->control->event, memory_order_acquire) !=
	    KOBOX_VM_CLIENT_READY)
		return EBUSY;
	if (ptrace(PTRACE_GETREGS, space->pid, NULL, &saved))
		return errno;
	result = kobox_posix_vm_read_fpregs(space, &saved_fp);
	if (result)
		return result;
	result = write_registers(space, registers);
	if (result)
		return result;
	result = kobox_posix_vm_write_fpregs(space, fp);
	if (result && (ptrace(PTRACE_SETREGS, space->pid, NULL, &saved) ||
		       kobox_posix_vm_write_fpregs(space, &saved_fp)))
		space->syscall_failed = true;
	if (!result)
		space->user_started = true;
	return result;
}

int kobox_posix_vm_syscall_return(struct kobox_posix_vm *space,
	uint64_t sequence, const struct kobox_x86_user_regs *registers)
{
	int result = owned(space);

	if (result)
		return result;
	if (space->syscall_failed)
		return EPERM;
	if (!space->syscall_pending || sequence != space->syscall_sequence)
		return ESTALE;
	if (space->running || space->event_pending)
		return EBUSY;
	result = write_registers(space, registers);
	if (result)
		return result;
	space->syscall_pending = false;
	return 0;
}

static int capture_syscall(struct kobox_posix_vm *space,
			   struct kobox_posix_vm_event *event)
{
	struct __ptrace_syscall_info info;
	struct user_regs_struct regs;
	int status, result;
	long size;

	size = ptrace(PTRACE_GET_SYSCALL_INFO, space->pid, sizeof(info), &info);
	if (size < 0)
		return errno;
	if ((size_t)size < offsetof(struct __ptrace_syscall_info, entry) + sizeof(info.entry) ||
	    info.op != PTRACE_SYSCALL_INFO_ENTRY || info.arch != AUDIT_ARCH_X86_64)
		return EPROTO;
	if (space->syscall_pending || space->syscall_sequence == UINT64_MAX)
		return EOVERFLOW;
	*event = (struct kobox_posix_vm_event) {
		.kind = KOBOX_POSIX_VM_SYSCALL,
		.ip = info.instruction_pointer, .sp = info.stack_pointer,
		.syscall = {.number = info.entry.nr, .sequence = space->syscall_sequence + 1},
	};
	memcpy(event->syscall.arguments, info.entry.args, sizeof(event->syscall.arguments));
	if (ptrace(PTRACE_GETREGS, space->pid, NULL, &regs))
		return errno;
	kobox_x86_linux_regs_export(&event->user, &regs);
	event->flags = kobox_x86_linux_flags(&regs);
	kobox_x86_linux_syscall_skip(&regs);
	if (ptrace(PTRACE_SETREGS, space->pid, NULL, &regs))
		return errno;
	/* Consume the skipped host syscall BEFORE publishing the request. This
	 * leaves a stable syscall-exit stop where VM injection can safely run
	 * while Linux services a nested user pointer or a page fault.
	 */
	for (;;) {
		result = resume_native(space, PTRACE_SYSCALL, 0);
		if (!result)
			result = wait_native(space, false, &status);
		if (result)
			return result;
		if (!space->pid) {
			*event = (struct kobox_posix_vm_event) {
				.kind = KOBOX_POSIX_VM_EXIT, .exit_status = status,
			};
			return 0;
		}
		if (!WIFSTOPPED(status))
			return EPROTO;
		if ((unsigned int)status >> 16 == PTRACE_EVENT_STOP && WSTOPSIG(status) == SIGTRAP)
			continue;
		if (WSTOPSIG(status) != (SIGTRAP | 0x80))
			return EPROTO;
		break;
	}
	size = ptrace(PTRACE_GET_SYSCALL_INFO, space->pid, sizeof(info), &info);
	if (size < 0)
		return errno;
	if ((size_t)size < offsetof(struct __ptrace_syscall_info, exit.is_error) +
			  sizeof(info.exit.is_error) ||
	    info.op != PTRACE_SYSCALL_INFO_EXIT || !info.exit.is_error || info.exit.rval != -ENOSYS)
		return EPROTO;
	space->syscall_sequence++;
	space->syscall_pending = true;
	return 0;
}

int kobox_posix_vm_pause(struct kobox_posix_vm *space)
{
	struct kobox_posix_vm_event event;
	int result = owned(space);

	if (result || !space->running)
		return result;
	if (ptrace(PTRACE_INTERRUPT, space->pid, NULL, NULL))
		return errno;
	space->pause_requested = true;
	result = kobox_posix_vm_wait(space, false, &event);
	space->pause_requested = false;
	if (result)
		return result;
	if (event.kind == KOBOX_POSIX_VM_EXIT)
		return ESRCH;
	if (event.kind != KOBOX_POSIX_VM_PAUSED) {
		space->pending_event = event;
		space->event_pending = true;
	}
	return 0;
}

int kobox_posix_vm_snapshot(struct kobox_posix_vm *space,
	struct kobox_x86_user_regs *registers, struct kobox_x86_fp_state *fp)
{
	struct kobox_posix_vm_event event;
	int result = owned(space);

	if (result)
		return result;
	if (!registers || !fp)
		return EINVAL;
	result = kobox_posix_vm_pause(space);
	if (result)
		return result;
	if (space->event_pending)
		return EBUSY;
	/* A fault report stops in the native handler, not in client code.
	 * Consume its native sigreturn, stopping at syscall exit before the
	 * faulting client instruction can run again or fault a second time.
	 */
	if (space->signal_return_allowed) {
		space->pause_requested = true;
		result = resume_native(space, PTRACE_SYSCALL, 0);
		if (!result)
			result = kobox_posix_vm_wait(space, false, &event);
		space->pause_requested = false;
		if (result)
			return result;
		if (event.kind != KOBOX_POSIX_VM_PAUSED) {
			space->pending_event = event;
			space->event_pending = true;
			return EBUSY;
		}
	}
	result = kobox_posix_vm_read_registers(space, registers);
	if (!result)
		result = kobox_posix_vm_read_fpregs(space, fp);
	return result;
}

int kobox_posix_vm_restore(struct kobox_posix_vm *space,
	const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp)
{
	struct user_regs_struct saved;
	struct kobox_x86_fp_state saved_fp;
	int result = owned(space);

	if (result)
		return result;
	if (!registers || !fp)
		return EINVAL;
	if (space->syscall_failed)
		return EPERM;
	if (space->running || space->event_pending || space->syscall_pending ||
	    space->capturing_fault || space->signal_return_allowed)
		return EBUSY;
	if (ptrace(PTRACE_GETREGS, space->pid, NULL, &saved))
		return errno;
	result = kobox_posix_vm_read_fpregs(space, &saved_fp);
	if (result)
		return result;
	result = write_registers(space, registers);
	if (result)
		return result;
	result = kobox_posix_vm_write_fpregs(space, fp);
	if (result && (ptrace(PTRACE_SETREGS, space->pid, NULL, &saved) ||
		       kobox_posix_vm_write_fpregs(space, &saved_fp)))
		space->syscall_failed = true;
	return result;
}

int kobox_posix_vm_wait(struct kobox_posix_vm *space, bool nonblock,
		       struct kobox_posix_vm_event *event)
{
	struct user_regs_struct regs;
	siginfo_t info;
	int status, result;

	result = owned(space);
	if (result || !event)
		return result ? result : EINVAL;
	if (space->event_pending) {
		*event = space->pending_event;
		space->event_pending = false;
		return 0;
	}
	for (;;) {
		result = wait_native(space, nonblock, &status);
		if (result)
			return result;
		if (!space->pid) {
			*event = (struct kobox_posix_vm_event) {
				.kind = KOBOX_POSIX_VM_EXIT, .exit_status = status,
			};
			return 0;
		}
		if (!WIFSTOPPED(status))
			return EPROTO;
		if ((unsigned int)status >> 16 == PTRACE_EVENT_STOP) {
			if (WSTOPSIG(status) != SIGTRAP)
				return EPROTO;
			if (space->pause_requested && !space->signal_return) {
				*event = (struct kobox_posix_vm_event) {.kind = KOBOX_POSIX_VM_PAUSED};
				return 0;
			}
			/* An interrupt may have lost a race with a fault stop.
			 * Drain its later event; it is not a client completion.
			 */
			result = resume_native(space, PTRACE_SYSCALL, 0);
			if (result)
				return result;
			continue;
		}
		if (WSTOPSIG(status) == (SIGTRAP | 0x80)) {
			if (ptrace(PTRACE_GETREGS, space->pid, NULL, &regs))
				return errno;
			/* Tracee code cannot use the inherited RAM fd for its own
			 * syscalls. Only native signal return is allowed here;
			 * mapping syscalls run solely in remote_syscall().
			 */
			if (!space->signal_return) {
				if (kobox_x86_linux_syscall_number(&regs) != SYS_rt_sigreturn || !space->signal_return_allowed) {
					if (!space->syscalls_enabled) {
						space->syscall_failed = true;
						return EPERM;
					}
					result = capture_syscall(space, event);
					if (result)
						space->syscall_failed = true;
					return result;
				}
				space->signal_return = true;
			} else {
				space->signal_return = false;
				space->signal_return_allowed = false;
				if (space->pause_requested) {
					*event = (struct kobox_posix_vm_event) {
						.kind = KOBOX_POSIX_VM_PAUSED,
					};
					return 0;
				}
			}
			result = resume_native(space, PTRACE_SYSCALL, 0);
			if (result)
				return result;
			continue;
		}
		if (WSTOPSIG(status) == SIGSEGV && !space->capturing_fault) {
			if (ptrace(PTRACE_GETSIGINFO, space->pid, NULL, &info))
				return errno;
			if (info.si_code <= 0)
				return EPROTO;
			space->capturing_fault = true;
			result = resume_native(space, PTRACE_SYSCALL, SIGSEGV);
			if (result)
				return result;
			continue;
		}
		if (WSTOPSIG(status) != SIGTRAP)
			return EPROTO;
		if (space->capturing_fault) {
			if (atomic_load_explicit(&space->control->event, memory_order_acquire) !=
			    KOBOX_VM_CLIENT_FAULT)
				return EPROTO;
			space->capturing_fault = false;
			space->signal_return_allowed = true;
			*event = (struct kobox_posix_vm_event) {
				.kind = KOBOX_POSIX_VM_FAULT,
				.address = space->control->fault_address,
				.ip = space->control->fault_ip,
				.sp = space->control->fault_sp,
				.flags = space->control->fault_flags,
				.error = space->control->fault_error,
			};
		} else {
			*event = (struct kobox_posix_vm_event) {.kind = KOBOX_POSIX_VM_STOP};
		}
		return 0;
	}
}

static int await_bootstrap(struct kobox_posix_vm *space)
{
	struct timespec pause = {.tv_sec = 1};
	uint64_t start, now;
	int result, status;

	result = kobox_posix_monotonic_ns(&start);
	if (result)
		return result;
	while (atomic_load_explicit(&space->control->event, memory_order_acquire) !=
	       KOBOX_VM_CLIENT_READY) {
		result = wait_native(space, true, &status);
		if (result != EAGAIN)
			return result ? result : ECHILD;
		result = kobox_posix_monotonic_ns(&now);
		if (result || now - start > UINT64_C(5000000000))
			return result ? result : ETIMEDOUT;
		(void)syscall(SYS_futex, &space->control->event, FUTEX_WAIT, 0, &pause, NULL, 0);
	}
	return 0;
}

struct native_children {
	pid_t *pids;
	size_t count;
};

static int snapshot_children(struct native_children *children)
{
	char path[96];
	pid_t pid, *grown;
	FILE *stream;
	int result = 0, scanned;

	snprintf(path, sizeof(path), "/proc/self/task/%ld/children", syscall(SYS_gettid));
	stream = fopen(path, "re");
	if (!stream)
		return errno;
	while ((scanned = fscanf(stream, "%d", &pid)) == 1) {
		if (pid <= 0 || children->count >= SIZE_MAX / sizeof(pid) - 1) {
			result = EOVERFLOW;
			break;
		}
		grown = realloc(children->pids, (children->count + 1) * sizeof(pid));
		if (!grown) {
			result = ENOMEM;
			break;
		}
		children->pids = grown;
		children->pids[children->count++] = pid;
	}
	if (!result && (ferror(stream) || scanned != EOF))
		result = EIO;
	if (fclose(stream) && !result)
		result = errno;
	return result;
}

static bool contains_child(const struct native_children *children, pid_t pid)
{
	size_t index;

	for (index = 0; index < children->count; index++)
		if (children->pids[index] == pid)
			return true;
	return false;
}

static int reap_new_children(const struct native_children *before)
{
	struct native_children after = {0};
	size_t index;
	int result = snapshot_children(&after);

	for (index = 0; !result && index < after.count; index++) {
		struct kobox_posix_vm orphan = {
			.pid = after.pids[index], .owner = syscall(SYS_gettid),
		};

		if (!contains_child(before, orphan.pid))
			result = kobox_posix_vm_destroy(&orphan);
	}
	free(after.pids);
	return result;
}

static int allocate_context(struct kobox_posix_vm *child, uint64_t *address)
{
	uint64_t args[6] = {0, KOBOX_VM_CLIENT_CONTEXT_SIZE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1UL, 0};
	int result;

	if (!full_user_space(child))
		return remote_syscall(child, SYS_mmap, args, address);
	/* Native machine storage must not consume a guest VMA. NOREPLACE also
	 * preserves inherited native mappings and other CLONE_VM contexts.
	 */
	args[3] |= MAP_FIXED_NOREPLACE;
	for (args[0] = KOBOX_X86_MACHINE_ALLOC;
	     args[0] <= KOBOX_X86_MACHINE_END - KOBOX_VM_CLIENT_CONTEXT_SIZE;
	     args[0] += KOBOX_VM_CLIENT_CONTEXT_SIZE) {
		result = remote_syscall(child, SYS_mmap, args, address);
		if (!result && *address != args[0]) {
			uint64_t unmap[6] = {*address, KOBOX_VM_CLIENT_CONTEXT_SIZE};
			uint64_t ignored;

			/* Older native kernels may ignore MAP_FIXED_NOREPLACE. */
			result = remote_syscall(child, SYS_munmap, unmap, &ignored);
			return result ? result : EOPNOTSUPP;
		}
		if (result != EEXIST)
			return result;
	}
	return ENOMEM;
}

static int clone_control(struct kobox_posix_vm *parent, struct kobox_posix_vm *child,
			 bool share_mm)
{
	uint64_t args[6] = {AT_FDCWD,
		parent->control_address + offsetof(struct kobox_vm_client_control, transfer_path),
		O_RDWR | O_CLOEXEC}, value, fd;
	char saved[sizeof(parent->control->transfer_path)];
	stack_t stack = {.ss_size = KOBOX_VM_CLIENT_STACK_SIZE};
	int result, length;

	memcpy(saved, parent->control->transfer_path, sizeof(saved));
	length = snprintf(parent->control->transfer_path, sizeof(saved),
		"/proc/%d/fd/%d", getpid(), child->control_backing.descriptor);
	if (length < 0 || (size_t)length >= sizeof(saved)) {
		memcpy(parent->control->transfer_path, saved, sizeof(saved));
		return EOVERFLOW;
	}
	/* This is a native resource import into the stopped machine bootstrap,
	 * never an open() dispatched on behalf of the Linux client FD table.
	 */
	result = remote_syscall(child, SYS_openat, args, &fd);
	memcpy(parent->control->transfer_path, saved, sizeof(saved));
	if (result)
		return result;
	if (fd <= KOBOX_VM_CONTROL_FD)
		return EPROTO;
	if (share_mm) {
		result = allocate_context(child, &value);
		if (result)
			return result;
		child->control_address = value;
	}
	args[0] = child->control_address;
	args[1] = 4096;
	args[2] = PROT_READ | PROT_WRITE;
	args[3] = MAP_SHARED | MAP_FIXED;
	args[4] = fd;
	args[5] = 0;
	result = remote_syscall(child, SYS_mmap, args, &value);
	if (result || value != child->control_address)
		return result ? result : EPROTO;
	args[0] = fd;
	args[1] = KOBOX_VM_CONTROL_FD;
	args[2] = 0;
	result = remote_syscall(child, SYS_dup3, args, &value);
	if (!result && value != KOBOX_VM_CONTROL_FD)
		result = EPROTO;
	if (!result)
		result = remote_syscall(child, SYS_close, args, &value);
	if (result || !share_mm)
		return result;
	child->control->control_address = child->control_address;
	args[0] = child->control_address + KOBOX_VM_CLIENT_STACK_OFFSET;
	args[1] = stack.ss_size;
	args[2] = PROT_READ | PROT_WRITE;
	args[3] = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
	args[4] = -1UL;
	args[5] = 0;
	result = remote_syscall(child, SYS_mmap, args, &value);
	if (result || value != args[0])
		return result ? result : EPROTO;
	stack.ss_sp = (void *)(uintptr_t)value;
	/* The native CLONE_VM child starts with sigaltstack disabled. Install
	 * its own registration before any guest register frame can be resumed.
	 */
	_Static_assert(sizeof(stack) <= sizeof(child->control->transfer_path),
		       "native bootstrap scratch must hold stack_t");
	memcpy(saved, child->control->transfer_path, sizeof(saved));
	memcpy(child->control->transfer_path, &stack, sizeof(stack));
	args[0] = child->control_address +
		offsetof(struct kobox_vm_client_control, transfer_path);
	args[1] = 0;
	result = remote_syscall(child, SYS_sigaltstack, args, &value);
	memcpy(child->control->transfer_path, saved, sizeof(saved));
	return result;
}

static int prune_inherited_contexts(struct kobox_posix_vm *parent,
				    struct kobox_posix_vm *child)
{
	struct kobox_posix_vm *peer;
	uint64_t args[6] = {0, KOBOX_VM_CLIENT_CONTEXT_SIZE}, value;
	int result;

	/* A private fork copied the entire native MM, including other threads'
	 * machine stacks. Those contexts do not exist in the new address space.
	 */
	for (peer = parent->group->contexts; peer; peer = peer->group_next) {
		if (peer == parent || !peer->control_address)
			continue;
		args[0] = peer->control_address;
		result = remote_syscall(child, SYS_munmap, args, &value);
		if (result)
			return result;
	}
	return 0;
}

int kobox_posix_vm_clone(struct kobox_posix_vm *parent, uint64_t sequence,
			bool share_mm, struct kobox_posix_vm *child)
{
	struct native_children before = {0};
	struct user_regs_struct saved, registers;
	unsigned long pid;
	bool started = false, restored = false;
	int status, result = owned(parent), cleanup;

	if (result)
		return result;
	if (!parent->group || !child || child->owner || child->pid || child->control || child->group ||
	    child->control_backing.initialized)
		return EINVAL;
	if (parent->syscall_failed)
		return EPERM;
	if (!parent->syscall_pending || sequence != parent->syscall_sequence)
		return ESTALE;
	if (parent->running || parent->event_pending || parent->capturing_fault ||
	    parent->signal_return_allowed)
		return EBUSY;
	result = snapshot_children(&before);
	if (result)
		goto out;
	if (!contains_child(&before, parent->pid)) {
		result = ECHILD;
		goto out;
	}
	child->owner = parent->owner;
	child->ram_size = parent->ram_size;
	child->window_start = parent->window_start;
	child->window_size = parent->window_size;
	child->user_started = true;
	child->syscall_entry = parent->syscall_entry;
	child->control_address = share_mm ? 0 : parent->control_address;
	result = join_group(child, share_mm ? parent->group : NULL);
	if (result)
		goto fail;
	result = kobox_posix_memory_backing_init(&child->control_backing, 4096);
	if (result)
		goto fail;
	child->control = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
			      child->control_backing.descriptor, 0);
	if (child->control == MAP_FAILED) {
		child->control = NULL;
		result = errno;
		goto fail;
	}
	memcpy(child->control, parent->control, sizeof(*child->control));
	if (ptrace(PTRACE_GETREGS, parent->pid, NULL, &saved)) {
		result = errno;
		goto fail;
	}
	if (ptrace(PTRACE_SETOPTIONS, parent->pid, NULL,
		   (void *)(uintptr_t)(TRACE_OPTIONS | PTRACE_O_TRACEFORK))) {
		result = errno;
		goto fail;
	}
	registers = saved;
	/* Native contexts belong to this machine service, not each other.
	 * Linux copy_process() separately owns the guest parent/child tree.
	 */
	/* Independent native thread groups allow one context to be killed and
	 * reaped without killing its peers. CLONE_VM is real shared memory;
	 * guest CLONE_THREAD/FILES/SIGHAND are handled solely by upstream.
	 */
	kobox_x86_linux_clone_prepare(&registers, parent->syscall_entry,
		SYS_clone, CLONE_PARENT | SIGCHLD | (share_mm ? CLONE_VM : 0));
	started = true;
	if (ptrace(PTRACE_SETREGS, parent->pid, NULL, &registers)) {
		result = errno;
		goto fail;
	}
	result = resume_native(parent, PTRACE_CONT, 0);
	if (!result)
		result = wait_native(parent, false, &status);
	if (result || !parent->pid) {
		result = result ? result : ESRCH;
		goto fail;
	}
	if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP ||
	    (unsigned int)status >> 16 != PTRACE_EVENT_FORK) {
		result = EPROTO;
		/* A resource-limited native clone can fail without a fork event.
		 * Its completed syscall must still leave the guest parent usable.
		 */
		if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP &&
		    !((unsigned int)status >> 16) &&
		    !ptrace(PTRACE_GETREGS, parent->pid, NULL, &registers) &&
		    kobox_x86_linux_syscall_done(&registers, parent->syscall_entry) &&
		    kobox_x86_linux_result(&registers) >= (uint64_t)-4095) {
			result = -(int64_t)kobox_x86_linux_result(&registers);
			if (!ptrace(PTRACE_SETREGS, parent->pid, NULL, &saved) &&
			    !ptrace(PTRACE_SETOPTIONS, parent->pid, NULL,
				    (void *)(uintptr_t)TRACE_OPTIONS))
				restored = true;
		}
		goto fail;
	}
	if (ptrace(PTRACE_GETEVENTMSG, parent->pid, NULL, &pid)) {
		result = errno;
		goto fail;
	}
	child->pid = pid;
	result = wait_native(child, false, &status);
	if (result || !child->pid || !WIFSTOPPED(status) ||
	    (unsigned int)status >> 16 != PTRACE_EVENT_STOP) {
		result = result ? result : EPROTO;
		goto fail;
	}
	/* Consume both native syscall returns before restoring guest context. */
	result = resume_native(parent, PTRACE_CONT, 0);
	if (!result)
		result = wait_native(parent, false, &status);
	if (result || !parent->pid || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP ||
	    (unsigned int)status >> 16 || ptrace(PTRACE_GETREGS, parent->pid, NULL, &registers) ||
	    !kobox_x86_linux_syscall_done(&registers, parent->syscall_entry) || kobox_x86_linux_result(&registers) != pid) {
		result = result ? result : EPROTO;
		goto fail;
	}
	if (ptrace(PTRACE_SETREGS, parent->pid, NULL, &saved) ||
	    ptrace(PTRACE_SETOPTIONS, parent->pid, NULL, (void *)(uintptr_t)TRACE_OPTIONS)) {
		result = errno;
		goto fail;
	}
	restored = true;
	result = resume_native(child, PTRACE_CONT, 0);
	if (!result)
		result = wait_native(child, false, &status);
	if (result || !child->pid || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP ||
	    (unsigned int)status >> 16 || ptrace(PTRACE_GETREGS, child->pid, NULL, &registers) ||
	    !kobox_x86_linux_syscall_done(&registers, child->syscall_entry) || kobox_x86_linux_result(&registers)) {
		result = result ? result : EPROTO;
		goto fail;
	}
	if (ptrace(PTRACE_SETOPTIONS, child->pid, NULL, (void *)(uintptr_t)TRACE_OPTIONS)) {
		result = errno;
		goto fail;
	}
	if (!share_mm) {
		result = prune_inherited_contexts(parent, child);
		if (result)
			goto fail;
	}
	result = clone_control(parent, child, share_mm);
	if (result)
		goto fail;
	registers = saved;
	kobox_x86_linux_clone_return(&registers);
	if (ptrace(PTRACE_SETREGS, child->pid, NULL, &registers)) {
		result = errno;
		goto fail;
	}
	child->syscalls_enabled = true;
	child->syscall_pending = true;
	child->syscall_sequence = sequence;
	result = 0;
	goto out;
fail:
	if (started && !restored)
		parent->syscall_failed = true;
	cleanup = kobox_posix_vm_destroy(child);
	if (!cleanup && started)
		cleanup = reap_new_children(&before);
	/* Failure to account for a native child cannot become a failed guest
	 * fork followed by continued execution. EXITKILL contains this fatal
	 * machine failure if the owner can no longer enumerate/reap children.
	 */
	if (cleanup)
		__builtin_trap();
out:
	free(before.pids);
	return result;
}

int kobox_posix_vm_destroy(struct kobox_posix_vm *space)
{
	struct kobox_posix_vm *peer;
	uint64_t args[6] = {0, KOBOX_VM_CLIENT_CONTEXT_SIZE}, value;
	int result;

	if (!space || space->owner != (pid_t)syscall(SYS_gettid))
		return EINVAL;
	result = reap_context(space);
	if (result)
		return result;
	/* Reaping one CLONE_VM context does not unmap its private bootstrap
	 * allocation. A surviving peer performs that leaf operation, without
	 * waiting for guest execution. No peer means the native MM is gone.
	 */
	if (space->group && space->control_address) {
	retry_peer:
		for (peer = space->group->contexts; peer; peer = peer->group_next)
			if (peer != space && peer->pid > 0)
				break;
		if (peer) {
			args[0] = space->control_address;
			result = mapping_syscall(peer, SYS_munmap, args, &value);
			if (result == ESRCH) {
				/* ESRCH is not completed unmapping. Reap the exact
				 * peer before trying another holder of this MM.
				 */
				result = kobox_posix_vm_destroy(peer);
				if (result)
					return result;
				goto retry_peer;
			}
			if (result)
				return result;
		}
		space->control_address = 0;
	}
	if (space->control) {
		if (munmap(space->control, 4096))
			return errno;
		space->control = NULL;
	}
	if (space->control_backing.initialized) {
		result = kobox_posix_memory_backing_destroy(&space->control_backing);
		if (result)
			return result;
	}
	leave_group(space);
	return 0;
}

int kobox_posix_vm_create(struct kobox_posix_vm *space, const char *client,
			 struct kobox_posix_memory_backing *ram,
			 uint64_t window_start, size_t window_size)
{
	posix_spawn_file_actions_t actions;
	char *const arguments[] = {(char *)client, NULL};
	unsigned long code;
	int ram_copy = -1, control_copy = -1;
	int result, status;

	if (!space || space->owner || !client || !ram || !ram->initialized ||
	    !kobox_vm_window_valid(window_start, window_size))
		return EINVAL;
	space->owner = syscall(SYS_gettid);
	space->ram_size = ram->size;
	space->window_start = window_start;
	space->window_size = window_size;
	result = join_group(space, NULL);
	if (result)
		goto fail;
	result = kobox_posix_memory_backing_init(&space->control_backing, 4096);
	if (result)
		goto fail;
	space->control = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
			      space->control_backing.descriptor, 0);
	if (space->control == MAP_FAILED) {
		space->control = NULL;
		result = errno;
		goto fail;
	}
	space->control->window_start = window_start;
	space->control->window_size = window_size;
	result = posix_spawn_file_actions_init(&actions);
	if (result)
		goto fail;
	/* Bootstrap destinations must not clobber another action's source. */
	ram_copy = fcntl(ram->descriptor, F_DUPFD_CLOEXEC, 5);
	control_copy = fcntl(space->control_backing.descriptor, F_DUPFD_CLOEXEC, 5);
	if (ram_copy < 0 || control_copy < 0) {
		result = errno;
		posix_spawn_file_actions_destroy(&actions);
		goto fail;
	}
	result = posix_spawn_file_actions_adddup2(&actions, ram_copy, KOBOX_VM_RAM_FD);
	if (!result)
		result = posix_spawn_file_actions_adddup2(&actions,
			control_copy, KOBOX_VM_CONTROL_FD);
	if (!result)
		result = posix_spawn(&space->pid, client, &actions, NULL, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	(void)close(ram_copy);
	(void)close(control_copy);
	ram_copy = -1;
	control_copy = -1;
	if (result)
		goto fail;
	result = await_bootstrap(space);
	if (result)
		goto fail;
	if (ptrace(PTRACE_SEIZE, space->pid, NULL, (void *)(uintptr_t)TRACE_OPTIONS)) {
		result = errno;
		goto fail;
	}
	if (ptrace(PTRACE_INTERRUPT, space->pid, NULL, NULL)) {
		result = errno;
		goto fail;
	}
	result = wait_native(space, false, &status);
	if (result)
		goto fail;
	if (!space->pid || !WIFSTOPPED(status) || (unsigned int)status >> 16 != PTRACE_EVENT_STOP) {
		result = EPROTO;
		goto fail;
	}
	atomic_store_explicit(&space->control->attached, 1, memory_order_release);
	/* Finish the trusted bootstrap outside syscall tracing. All client
	 * execution after its READY stop is syscall-controlled.
	 */
	result = resume_native(space, PTRACE_CONT, 0);
	if (!result)
		result = wait_native(space, false, &status);
	if (result)
		goto fail;
	if (!space->pid || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP ||
	    atomic_load_explicit(&space->control->event, memory_order_acquire) !=
	    KOBOX_VM_CLIENT_READY) {
		result = EPROTO;
		goto fail;
	}
	space->syscall_entry = space->control->syscall_entry;
	space->control_address = space->control->control_address;
	if (!space->control_address || (space->control_address & 4095)) {
		result = EPROTO;
		goto fail;
	}
	errno = 0;
	code = ptrace(PTRACE_PEEKTEXT, space->pid, (void *)(uintptr_t)space->syscall_entry, NULL);
	if (errno || (code & 0xffffff) != 0xcc050f) {
		result = errno ? errno : EPROTO;
		goto fail;
	}
	return 0;
fail:
	if (ram_copy >= 0)
		(void)close(ram_copy);
	if (control_copy >= 0)
		(void)close(control_copy);
	status = kobox_posix_vm_destroy(space);
	return status ? status : result;
}
