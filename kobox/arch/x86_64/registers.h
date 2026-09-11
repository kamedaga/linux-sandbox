/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_REGISTERS_H
#define KOBOX_X86_REGISTERS_H

#include "user.h"

struct task_struct;
bool kobox_x86_user_mode(const struct kobox_x86_user_regs *registers);
int kobox_x86_user_set_tls(struct task_struct *task, unsigned long tls);
void kobox_x86_user_syscall_prepare(void);
void kobox_x86_user_syscall_dispatch(void);
void kobox_x86_user_return_work(void);
void kobox_x86_user_fault(unsigned long address, unsigned long error);
unsigned long kobox_x86_user_syscall_number(void);
unsigned long kobox_x86_user_ip(void);
void kobox_x86_user_export(struct kobox_x86_user_regs *output);
void kobox_x86_user_import(const struct kobox_x86_user_regs *input);

#endif
