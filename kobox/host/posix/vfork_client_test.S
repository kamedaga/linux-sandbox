/* SPDX-License-Identifier: GPL-2.0-only */
#include <asm/unistd.h>
.text
.globl kobox_elf_vfork
.type kobox_elf_vfork, @function
kobox_elf_vfork:
	pushq %r12
	movq %rdi, %r12
	movl $__NR_vfork, %eax
	syscall
	testq %rax, %rax
	jz .Lchild
	popq %r12
	ret
.Lchild:
	/* Child frames stay below the parent's suspended return address. */
	movq %r12, %rdi
	call kobox_elf_vfork_child
	ud2
.size kobox_elf_vfork, .-kobox_elf_vfork
.section .note.GNU-stack,"",@progbits
