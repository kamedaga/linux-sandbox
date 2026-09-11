/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared conformance access: the OS supplies the real user fault return.
 * Keep every operand in registers while the guest stack is inaccessible. */
.text
.globl kobox_vm_stack_access
.type kobox_vm_stack_access, @function
kobox_vm_stack_access:
	movq %rsp, %rcx
	leaq 8(%rdi), %rsp
	pushq %rsi
	popq %rax
	movq %rcx, %rsp
	ret
.size kobox_vm_stack_access, .-kobox_vm_stack_access
.section .note.GNU-stack,"",@progbits
