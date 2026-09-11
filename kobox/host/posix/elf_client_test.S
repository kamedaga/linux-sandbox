/* SPDX-License-Identifier: GPL-2.0-only */
#include <asm/unistd.h>
.text
.globl _start
.type _start, @function
_start:
	movq %rsp, %rdi
	andq $-16, %rsp
	xorl %ebp, %ebp
	call kobox_elf_client_test
	movl %eax, %edi
	movl $__NR_exit_group, %eax
	syscall
	ud2
.size _start, .-_start

/* Ordinary libc-style clone wrapper, running entirely in Linux user memory. */
.globl kobox_elf_clone
.type kobox_elf_clone, @function
kobox_elf_clone:
	subq $16, %rsi
	movq %rdx, (%rsi)
	movq %rcx, 8(%rsi)
	xorl %edx, %edx
	xorl %r10d, %r10d
	xorl %r8d, %r8d
	movl $__NR_clone, %eax
	syscall
	testq %rax, %rax
	jz .Lpeer
	ret
.Lpeer:
	popq %rdi
	popq %rsi
	xorl %ebp, %ebp
	call kobox_elf_peer
	movl %eax, %edi
	movl $__NR_exit, %eax
	syscall
	ud2
.size kobox_elf_clone, .-kobox_elf_clone
.section .note.GNU-stack,"",@progbits
