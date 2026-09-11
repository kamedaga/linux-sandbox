/* SPDX-License-Identifier: GPL-2.0-only */
#include "registers.h"

#include <linux/export.h>
#include <linux/entry-common.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <asm/segment.h>
#include <asm/proto.h>
#include <asm/trapnr.h>
#include <uapi/asm/prctl.h>

bool kobox_x86_user_mode(const struct kobox_x86_user_regs *registers)
{
	return registers->cs == __USER_CS && registers->ss == __USER_DS;
}

int kobox_x86_user_set_tls(struct task_struct *task, unsigned long tls)
{
	return do_arch_prctl_64(task, ARCH_SET_FS, tls);
}

void kobox_x86_user_syscall_prepare(void)
{
	current_pt_regs()->ax = -ENOSYS;
}

void kobox_x86_user_syscall_dispatch(void)
{
	struct pt_regs *regs = current_pt_regs();
	long number = syscall_enter_from_user_mode_work(regs, (int)regs->orig_ax);

	if (number != -1)
		regs->ax = x64_sys_call(regs, (unsigned int)number);
}

void kobox_x86_user_return_work(void)
{
	syscall_exit_to_user_mode_work(current_pt_regs());
}

void kobox_x86_user_fault(unsigned long address, unsigned long error)
{
	current->thread.trap_nr = X86_TRAP_PF;
	current->thread.error_code = error;
	current->thread.cr2 = address;
}

unsigned long kobox_x86_user_syscall_number(void)
{
	return current_pt_regs()->orig_ax;
}

unsigned long kobox_x86_user_ip(void)
{
	return current_pt_regs()->ip;
}

unsigned long kobox_user_fsbase_read(void)
{
	return current->thread.fsbase;
}
EXPORT_SYMBOL(kobox_user_fsbase_read);

void kobox_user_fsbase_write(unsigned long base)
{
	current->thread.fsbase = base;
}
EXPORT_SYMBOL(kobox_user_fsbase_write);

unsigned long x86_gsbase_read_cpu_inactive(void)
{
	return current->thread.gsbase;
}

void x86_gsbase_write_cpu_inactive(unsigned long base)
{
	current->thread.gsbase = base;
}

unsigned short kobox_user_read_segment(enum kobox_user_segment segment)
{
	switch (segment) {
	case KOBOX_SEG_fs:
		return current->thread.fsindex;
	case KOBOX_SEG_gs:
		return current->thread.gsindex;
	case KOBOX_SEG_ds:
		return current->thread.ds;
	case KOBOX_SEG_es:
		return current->thread.es;
	case KOBOX_SEG_cs:
		return current_pt_regs()->cs;
	case KOBOX_SEG_ss:
		return current_pt_regs()->ss;
	}
	BUG();
}
EXPORT_SYMBOL(kobox_user_read_segment);

void kobox_user_load_segment(enum kobox_user_segment segment, unsigned short value)
{
	/* The fixed core exposes long-mode flat segments, not an emulated LDT. */
	if (value && value != __USER_DS && value != __USER_CS)
		panic("unsupported hosted user segment %x", value);
	switch (segment) {
	case KOBOX_SEG_fs:
		current->thread.fsindex = value;
		current->thread.fsbase = 0;
		break;
	case KOBOX_SEG_gs:
		current->thread.gsindex = value;
		current->thread.gsbase = 0;
		break;
	case KOBOX_SEG_ds:
		current->thread.ds = value;
		break;
	case KOBOX_SEG_es:
		current->thread.es = value;
		break;
	case KOBOX_SEG_cs:
		current_pt_regs()->cs = value;
		break;
	case KOBOX_SEG_ss:
		current_pt_regs()->ss = value;
		break;
	default:
		BUG();
	}
}
EXPORT_SYMBOL(kobox_user_load_segment);

void kobox_x86_user_export(struct kobox_x86_user_regs *output)
{
	struct pt_regs *regs = current_pt_regs();

	*output = (struct kobox_x86_user_regs) {
		.ax = regs->ax, .bx = regs->bx, .cx = regs->cx,
		.dx = regs->dx, .si = regs->si, .di = regs->di,
		.bp = regs->bp, .r8 = regs->r8, .r9 = regs->r9,
		.r10 = regs->r10, .r11 = regs->r11, .r12 = regs->r12,
		.r13 = regs->r13, .r14 = regs->r14, .r15 = regs->r15,
		.ip = regs->ip, .sp = regs->sp, .flags = regs->flags,
		.orig_ax = regs->orig_ax, .cs = regs->cs, .ss = regs->ss,
		.fs_base = current->thread.fsbase, .gs_base = current->thread.gsbase,
		.fs = current->thread.fsindex, .gs = current->thread.gsindex,
		.ds = current->thread.ds, .es = current->thread.es,
	};
}

void kobox_x86_user_import(const struct kobox_x86_user_regs *input)
{
	struct pt_regs *regs = current_pt_regs();

	*regs = (struct pt_regs) {
		.ax = input->ax, .bx = input->bx, .cx = input->cx,
		.dx = input->dx, .si = input->si, .di = input->di,
		.bp = input->bp, .r8 = input->r8, .r9 = input->r9,
		.r10 = input->r10, .r11 = input->r11, .r12 = input->r12,
		.r13 = input->r13, .r14 = input->r14, .r15 = input->r15,
		.ip = input->ip, .sp = input->sp, .flags = input->flags,
		.orig_ax = input->orig_ax, .cs = input->cs, .ss = input->ss,
	};
	current->thread.fsbase = input->fs_base;
	current->thread.gsbase = input->gs_base;
	current->thread.fsindex = input->fs;
	current->thread.gsindex = input->gs;
	current->thread.ds = input->ds;
	current->thread.es = input->es;
}
