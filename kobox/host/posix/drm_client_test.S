/* SPDX-License-Identifier: GPL-2.0-only */
#include <asm/unistd.h>
.text
.globl _start
.type _start, @function
_start:
	movq %rsp, %rdi
	andq $-16, %rsp
	xorl %ebp, %ebp
	call kobox_drm_client_test
	movl %eax, %edi
	movl $__NR_exit_group, %eax
	syscall
	ud2
.size _start, .-_start
#ifdef KOBOX_DRM_SYNC
.globl kobox_sync_sigreturn
.type kobox_sync_sigreturn, @function
kobox_sync_sigreturn:
	movl $__NR_rt_sigreturn, %eax
	syscall
	ud2
.size kobox_sync_sigreturn, .-kobox_sync_sigreturn
#endif
.section .note.GNU-stack,"",@progbits
