/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux native execution ABI for the fixed x86-64 machine. */
.text
.globl kobox_vm_syscall_entry
.type kobox_vm_syscall_entry, @function
kobox_vm_syscall_entry:
	syscall
	int3
	ret
.size kobox_vm_syscall_entry, .-kobox_vm_syscall_entry

/* The diagnostic user program crosses the real clone instruction. Child
 * registers/stack come back from Linux copy_thread through the machine port.
 */
#include <asm/unistd.h>
.globl kobox_vm_clone_entry
.type kobox_vm_clone_entry, @function
kobox_vm_clone_entry:
	pushq %r12
	movq %rdx, %r12
	movq %rcx, %rdx
	movq %rcx, %r10
	xorl %r8d, %r8d
	xorl %r9d, %r9d
	movq $__NR_clone, %rax
	syscall
	testq %rax, %rax
	jnz 1f
	movq %r12, %rdi
	call kobox_vm_child_program
	ud2
1:
	popq %r12
	ret
.size kobox_vm_clone_entry, .-kobox_vm_clone_entry

/* The Linux-created stack can initially be inaccessible on the host.
 * The separate native signal stack reports the real first push/store fault.
 */
.globl kobox_vm_root_entry
.type kobox_vm_root_entry, @function
kobox_vm_root_entry:
	movq %rdx, %rsp
	andq $-16, %rsp
	xorl %ebp, %ebp
	call kobox_vm_root_program
	ud2
.size kobox_vm_root_entry, .-kobox_vm_root_entry
.section .note.GNU-stack,"",@progbits
