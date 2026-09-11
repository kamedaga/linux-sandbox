/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux native execution ABI for the fixed x86-64 machine. */
#include <asm/unistd.h>

.text
.globl _start
.type _start, @function
_start:
	/* Do not retain the native ELF stack or its argv/auxv mappings. */
	leaq machine_stack_end(%rip), %rsp
	xorl %ebp, %ebp
	call kobox_vm_bootstrap
	ud2
.size _start, .-_start

.globl kobox_vm_syscall_entry
.type kobox_vm_syscall_entry, @function
kobox_vm_syscall_entry:
	syscall
	int3
	ret
.size kobox_vm_syscall_entry, .-kobox_vm_syscall_entry

.globl kobox_vm_native_sigreturn
.type kobox_vm_native_sigreturn, @function
kobox_vm_native_sigreturn:
	movq $__NR_rt_sigreturn, %rax
	syscall
	ud2
.size kobox_vm_native_sigreturn, .-kobox_vm_native_sigreturn

.bss
.balign 4096
machine_stack:
	.skip 65536
machine_stack_end:
.section .note.GNU-stack,"",@progbits
