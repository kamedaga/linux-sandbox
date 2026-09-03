/* SPDX-License-Identifier: GPL-2.0-only */

#include <asm-offsets.h>

	.section .text.kobox_switch_to_asm,"ax",@progbits
	.globl __switch_to_asm
	.type __switch_to_asm,@function
__switch_to_asm:
	/* This layout matches struct inactive_task_frame. */
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	movq	%rdi, %rax
	movq	%rsp, TASK_threadsp(%rdi)
	movq	TASK_threadsp(%rsi), %rsp

	/* Unconditionally overwrite the return stack before changing tasks. */
	movl	$16, %r12d
.Lfill_rsb:
	call	.Lrsb_slot_1
	int3
.Lrsb_slot_1:
	call	.Lrsb_slot_2
	int3
.Lrsb_slot_2:
	addq	$16, %rsp
	decl	%r12d
	jnz	.Lfill_rsb
	lfence

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp

	ret
	.size __switch_to_asm, .-__switch_to_asm

	.section .text.kobox_ret_from_fork_asm,"ax",@progbits
	.globl ret_from_fork_asm
	.type ret_from_fork_asm,@function
ret_from_fork_asm:
	/* Provider tasks are kernel threads and never return to a user frame. */
	movq	%rax, %rdi
	call	schedule_tail
	testq	%rbx, %rbx
	jz	.Linvalid_fork_return
	movq	%r12, %rdi
	call	*%rbx
.Linvalid_fork_return:
	ud2
	.size ret_from_fork_asm, .-ret_from_fork_asm

	.section .text.kobox_kernel_entry_fault,"ax",@progbits
	.globl entry_SYSCALL_64
	.type entry_SYSCALL_64,@function
entry_SYSCALL_64:
	ud2
	.size entry_SYSCALL_64, .-entry_SYSCALL_64

	.globl entry_SYSCALL32_ignore
	.type entry_SYSCALL32_ignore,@function
entry_SYSCALL32_ignore:
	ud2
	.size entry_SYSCALL32_ignore, .-entry_SYSCALL32_ignore

	.globl x86_64_start_kernel
	.type x86_64_start_kernel,@function
x86_64_start_kernel:
	ud2
	.size x86_64_start_kernel, .-x86_64_start_kernel
	.section .note.GNU-stack,"",@progbits
