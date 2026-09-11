/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_LINUX_PTRACE_H
#define KOBOX_X86_LINUX_PTRACE_H

#include "user.h"

#include <stdbool.h>
#include <sys/types.h>
#include <sys/user.h>

/* Linux/x86 native ABI glue. Process ownership and stop/resume stay in the
 * OS backend; conversion never overlays Linux-core structures on host state.
 */
void kobox_x86_linux_regs_export(struct kobox_x86_user_regs *to,
				const struct user_regs_struct *from);
bool kobox_x86_linux_regs_import(struct user_regs_struct *native,
				const struct kobox_x86_user_regs *registers);
void kobox_x86_linux_syscall_prepare(struct user_regs_struct *native,
				     uint64_t entry, uint64_t number,
				     const uint64_t args[6]);
void kobox_x86_linux_clone_prepare(struct user_regs_struct *native,
				   uint64_t entry, uint64_t number, uint64_t flags);
int kobox_x86_linux_fp_read(pid_t pid, struct kobox_x86_fp_state *state);
int kobox_x86_linux_fp_write(pid_t pid, const struct kobox_x86_fp_state *state);

static inline uint64_t kobox_x86_linux_ip(const struct user_regs_struct *native)
{
	return native->rip;
}

static inline bool kobox_x86_linux_syscall_done(
	const struct user_regs_struct *native, uint64_t entry)
{
	/* syscall (2 bytes), then int3 (1 byte) in the native trampoline. */
	return native->rip == entry + 3;
}

static inline uint64_t kobox_x86_linux_result(
	const struct user_regs_struct *native)
{
	return native->rax;
}

static inline uint64_t kobox_x86_linux_flags(
	const struct user_regs_struct *native)
{
	return native->eflags;
}

static inline uint64_t kobox_x86_linux_syscall_number(
	const struct user_regs_struct *native)
{
	return native->orig_rax;
}

static inline void kobox_x86_linux_syscall_skip(struct user_regs_struct *native)
{
	native->orig_rax = -1UL;
}

static inline void kobox_x86_linux_clone_return(struct user_regs_struct *native)
{
	native->rax = 0;
	native->orig_rax = -1UL;
}

#endif
