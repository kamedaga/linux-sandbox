/* SPDX-License-Identifier: GPL-2.0-only */
#include "linux_ptrace.h"

#include <asm/processor-flags.h>
#include <errno.h>
#include <string.h>
#include <sys/ptrace.h>

void kobox_x86_linux_regs_export(struct kobox_x86_user_regs *to,
			   const struct user_regs_struct *from)
{
	*to = (struct kobox_x86_user_regs) {
		.ax = from->rax, .bx = from->rbx, .cx = from->rcx,
		.dx = from->rdx, .si = from->rsi, .di = from->rdi, .bp = from->rbp,
		.r8 = from->r8, .r9 = from->r9, .r10 = from->r10, .r11 = from->r11,
		.r12 = from->r12, .r13 = from->r13, .r14 = from->r14, .r15 = from->r15,
		.ip = from->rip, .sp = from->rsp, .flags = from->eflags,
		.orig_ax = from->orig_rax, .fs_base = from->fs_base, .gs_base = from->gs_base,
		.cs = from->cs, .ss = from->ss, .ds = from->ds, .es = from->es,
		.fs = from->fs, .gs = from->gs,
	};
}


bool kobox_x86_linux_regs_import(struct user_regs_struct *native,
				const struct kobox_x86_user_regs *registers)
{
	const uint64_t user_flags = X86_EFLAGS_CF | X86_EFLAGS_PF | X86_EFLAGS_AF |
		X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_TF | X86_EFLAGS_DF |
		X86_EFLAGS_OF | X86_EFLAGS_RF | X86_EFLAGS_AC | X86_EFLAGS_ID;

	/* This adapter is the 64-bit user execution boundary. Reject context
	 * changes outside it before PTRACE_SETREGS can partially alter a task.
	 */
	if (registers->cs != native->cs || registers->ss != native->ss ||
	    (registers->ds && registers->ds != native->ss) ||
	    (registers->es && registers->es != native->ss) ||
	    (registers->fs && registers->fs != native->ss) ||
	    (registers->gs && registers->gs != native->ss) ||
	    registers->ip >= (1ULL << 47) || registers->sp >= (1ULL << 47) ||
	    registers->fs_base >= (1ULL << 47) || registers->gs_base >= (1ULL << 47) ||
	    ((registers->flags ^ native->eflags) & ~user_flags))
		return false;
	native->rax = registers->ax;
	native->rbx = registers->bx;
	native->rcx = registers->cx;
	native->rdx = registers->dx;
	native->rsi = registers->si;
	native->rdi = registers->di;
	native->rbp = registers->bp;
	native->r8 = registers->r8;
	native->r9 = registers->r9;
	native->r10 = registers->r10;
	native->r11 = registers->r11;
	native->r12 = registers->r12;
	native->r13 = registers->r13;
	native->r14 = registers->r14;
	native->r15 = registers->r15;
	native->rip = registers->ip;
	native->rsp = registers->sp;
	native->eflags = registers->flags;
	native->fs_base = registers->fs_base;
	native->gs_base = registers->gs_base;
	native->ds = registers->ds;
	native->es = registers->es;
	native->fs = registers->fs;
	native->gs = registers->gs;
	/* The host syscall was skipped and consumed. Guest restart uses guest
	 * IP/registers; never arm a host syscall restart with the guest number.
	 */
	native->orig_rax = -1UL;
	return true;
}

void kobox_x86_linux_syscall_prepare(struct user_regs_struct *native,
				     uint64_t entry, uint64_t number,
				     const uint64_t args[6])
{
	native->rip = entry;
	native->rax = number;
	native->orig_rax = -1UL;
	native->rdi = args[0];
	native->rsi = args[1];
	native->rdx = args[2];
	native->r10 = args[3];
	native->r8 = args[4];
	native->r9 = args[5];
}

void kobox_x86_linux_clone_prepare(struct user_regs_struct *native,
				   uint64_t entry, uint64_t number, uint64_t flags)
{
	/* x86-64 clone consumes five arguments; retain the untouched sixth. */
	const uint64_t args[6] = { flags, 0, 0, 0, 0, native->r9 };

	kobox_x86_linux_syscall_prepare(native, entry, number, args);
}

int kobox_x86_linux_fp_read(pid_t pid, struct kobox_x86_fp_state *state)
{
	struct user_fpregs_struct native;

	_Static_assert(sizeof(native) == sizeof(*state), "FXSAVE format mismatch");
	if (ptrace(PTRACE_GETFPREGS, pid, NULL, &native))
		return errno;
	memcpy(state, &native, sizeof(*state));
	return 0;
}

int kobox_x86_linux_fp_write(pid_t pid, const struct kobox_x86_fp_state *state)
{
	struct user_fpregs_struct native;

	_Static_assert(sizeof(native) == sizeof(*state), "FXSAVE format mismatch");
	memcpy(&native, state, sizeof(native));
	return ptrace(PTRACE_SETFPREGS, pid, NULL, &native) ? errno : 0;
}
