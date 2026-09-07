/* SPDX-License-Identifier: GPL-2.0-only */
.text
.globl kobox_vm_syscall_entry
.type kobox_vm_syscall_entry, @function
kobox_vm_syscall_entry:
	syscall
	int3
	ret
.size kobox_vm_syscall_entry, .-kobox_vm_syscall_entry
.section .note.GNU-stack,"",@progbits
