/* SPDX-License-Identifier: GPL-2.0-only */
#include <asm/unistd.h>

/* Position-independent machine probe copied to a RAM page by the test.
 * There is no ELF parser here. The Linux ELF integration uses binfmt_elf.
 */
.section .rodata
.globl kobox_vm_probe_begin
kobox_vm_probe_begin:
	pushq %r12
	movq %fs:0, %rdi
	movq $__NR_getpid, %rax
	syscall
	movq %rax, (%r12)
	popq %r12
	movq $__NR_exit_group, %rax
	xorl %edi, %edi
	syscall
	ud2
.globl kobox_vm_probe_spin
kobox_vm_probe_spin:
	pxor %xmm0, %xmm0
	incq %r13
	jmp kobox_vm_probe_spin
.globl kobox_vm_probe_end
kobox_vm_probe_end:
.section .note.GNU-stack,"",@progbits
