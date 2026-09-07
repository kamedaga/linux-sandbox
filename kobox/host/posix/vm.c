// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int owned(struct kobox_posix_vm *space)
{
	if (!space || space->owner != (pid_t)syscall(SYS_gettid))
		return EINVAL;
	return space->pid > 0 ? 0 : ESRCH;
}

static bool valid_range(uint64_t address, size_t size)
{
	return size && !(address & 4095) && !(size & 4095) &&
		address >= KOBOX_VM_WINDOW_BASE &&
		address - KOBOX_VM_WINDOW_BASE < KOBOX_VM_WINDOW_SIZE &&
		size <= KOBOX_VM_WINDOW_SIZE - (address - KOBOX_VM_WINDOW_BASE);
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
	regs.rip = space->syscall_entry;
	regs.rax = number;
	regs.orig_rax = -1UL;
	regs.rdi = args[0];
	regs.rsi = args[1];
	regs.rdx = args[2];
	regs.r10 = args[3];
	regs.r8 = args[4];
	regs.r9 = args[5];
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
		    regs.rip == space->syscall_entry) {
			if (ptrace(PTRACE_GETSIGINFO, space->pid, NULL, &info))
				return errno;
			if ((info.si_code != SEGV_MAPERR && info.si_code != SEGV_ACCERR) ||
			    (uintptr_t)info.si_addr < KOBOX_VM_WINDOW_BASE ||
			    (uintptr_t)info.si_addr - KOBOX_VM_WINDOW_BASE >= KOBOX_VM_WINDOW_SIZE)
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
	if (regs.rip != space->syscall_entry + 3)
		return EPROTO;
	*value = regs.rax;
	if (ptrace(PTRACE_SETREGS, space->pid, NULL, &saved))
		return errno;
	if (*value >= (uint64_t)-4095)
		return -(int64_t)*value;
	return 0;
}

static int mapping_syscall(struct kobox_posix_vm *space, unsigned long number,
			   const uint64_t args[6], uint64_t *value)
{
	bool running;
	int result = owned(space);

	if (result)
		return result;
	running = space->running;
	result = kobox_posix_vm_pause(space);
	if (!result)
		result = remote_syscall(space, number, args, value);
	if (!result && running && !space->event_pending)
		result = kobox_posix_vm_resume(space);
	return result;
}

int kobox_posix_vm_map(struct kobox_posix_vm *space, uint64_t address,
		      size_t offset, size_t size, unsigned int protection)
{
	uint64_t args[6], value;
	int native, result;

	result = owned(space);
	if (result)
		return result;
	if (!valid_range(address, size) || (offset & 4095) ||
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

	if (!valid_range(address, size))
		return EINVAL;
	result = mapping_syscall(space, SYS_mmap, args, &value);
	return result ? result : value == address ? 0 : EFAULT;
}

int kobox_posix_vm_protect(struct kobox_posix_vm *space, uint64_t address,
			  size_t size, unsigned int protection)
{
	uint64_t args[6] = {address, size}, value;
	int native, result;

	if (!valid_range(address, size))
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
	if (space->running || space->event_pending)
		return EBUSY;
	return resume_native(space, PTRACE_SYSCALL, 0);
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
				if (regs.orig_rax != SYS_rt_sigreturn || !space->signal_return_allowed)
					return EPERM;
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

int kobox_posix_vm_destroy(struct kobox_posix_vm *space)
{
	int result, status;

	if (!space || space->owner != (pid_t)syscall(SYS_gettid))
		return EINVAL;
	if (space->pid > 0) {
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
	return 0;
}

int kobox_posix_vm_create(struct kobox_posix_vm *space, const char *client,
			 struct kobox_posix_memory_backing *ram)
{
	posix_spawn_file_actions_t actions;
	char *const arguments[] = {(char *)client, NULL};
	unsigned long code;
	int ram_copy = -1, control_copy = -1;
	int result, status;

	if (!space || space->owner || !client || !ram || !ram->initialized)
		return EINVAL;
	space->owner = syscall(SYS_gettid);
	space->ram_size = ram->size;
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
	if (ptrace(PTRACE_SEIZE, space->pid, NULL,
		   (void *)(uintptr_t)(PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD))) {
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
