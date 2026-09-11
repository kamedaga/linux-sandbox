/* SPDX-License-Identifier: GPL-2.0-only */
#include <asm/unistd.h>
.macro seed_fp
	subq $16, %rsp
	stmxcsr (%rsp)
	movl $0x7f80, 4(%rsp)
	ldmxcsr 4(%rsp)
	fld1
.endm
.text
.globl kobox_elf_sigreturn
.type kobox_elf_sigreturn, @function
kobox_elf_sigreturn:
	movl $__NR_rt_sigreturn, %eax
	syscall
	ud2
.size kobox_elf_sigreturn, .-kobox_elf_sigreturn

.globl kobox_elf_bad_sigreturn
.type kobox_elf_bad_sigreturn, @function
kobox_elf_bad_sigreturn:
	movl $1, %esp
	movl $__NR_rt_sigreturn, %eax
	syscall
	ud2
.size kobox_elf_bad_sigreturn, .-kobox_elf_bad_sigreturn

.globl kobox_elf_signal_call
.type kobox_elf_signal_call, @function
kobox_elf_signal_call:
	pushq %r12
	seed_fp
	movq %rdi, %rax
	movq %rsi, %rdi
	movq %rdx, %rsi
	movq %rcx, %rdx
	movq %rax, %r10
	movl $0x11223344, %r12d
	movl $0x12345678, %eax
	movd %eax, %xmm8
	movd %eax, %xmm0
	movq %r10, %rax
.globl kobox_elf_signal_call_begin
kobox_elf_signal_call_begin:
	syscall
.globl kobox_elf_signal_call_end
kobox_elf_signal_call_end:
	movq %rax, %r10
.Lcheck:
	stmxcsr 4(%rsp)
	cmpl $0x7f80, 4(%rsp)
	jne .Lfailed
	fstpl 8(%rsp)
	movabsq $0x3ff0000000000000, %rax
	cmpq %rax, 8(%rsp)
	jne .Lfailed
	cmpl $0x55667788, %r12d
	jne .Lfailed
	movd %xmm8, %eax
	cmpl $0x12345678, %eax
	jne .Lfailed
	movd %xmm0, %eax
	cmpl $0x12345678, %eax
	jne .Lfailed
	movq %r10, %rax
	jmp .Ldone
.Lfailed:
	movq $-4094, %rax
.Ldone:
	ldmxcsr (%rsp)
	addq $16, %rsp
	popq %r12
	ret
.size kobox_elf_signal_call, .-kobox_elf_signal_call

.globl kobox_elf_signal_spin
.type kobox_elf_signal_spin, @function
kobox_elf_signal_spin:
	pushq %r12
	seed_fp
	movl $0x11223344, %r12d
	movl $0x12345678, %eax
	movd %eax, %xmm0
	movd %eax, %xmm8
	movl $1, (%rdi)
.globl kobox_elf_signal_spin_begin
kobox_elf_signal_spin_begin:
	pause
	cmpl $0x11223344, %r12d
	je kobox_elf_signal_spin_begin
.globl kobox_elf_signal_spin_end
kobox_elf_signal_spin_end:
	xorl %r10d, %r10d
	jmp .Lcheck
.size kobox_elf_signal_spin, .-kobox_elf_signal_spin

.globl kobox_elf_signal_fault
.type kobox_elf_signal_fault, @function
kobox_elf_signal_fault:
	pushq %r12
	seed_fp
	movl $0x11223344, %r12d
	movl $0x12345678, %eax
	movd %eax, %xmm0
	movd %eax, %xmm8
.globl kobox_elf_signal_fault_begin
kobox_elf_signal_fault_begin:
	movq (%rdi), %rax
.globl kobox_elf_signal_fault_end
kobox_elf_signal_fault_end:
	xorl %r10d, %r10d
	jmp .Lcheck
.size kobox_elf_signal_fault, .-kobox_elf_signal_fault
.section .note.GNU-stack,"",@progbits
