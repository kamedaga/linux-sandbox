/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_LINUX_CALL_H
#define KOBOX_X86_LINUX_CALL_H

/* Native Linux syscall ABI, also usable by the freestanding bootstrap. */
static inline unsigned long kobox_x86_linux_call(unsigned long number,
	unsigned long a0, unsigned long a1, unsigned long a2,
	unsigned long a3, unsigned long a4, unsigned long a5)
{
	register unsigned long r10 __asm__("r10") = a3;
	register unsigned long r8 __asm__("r8") = a4;
	register unsigned long r9 __asm__("r9") = a5;

	__asm__ volatile("syscall" : "+a"(number) :
		"D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9) :
		"rcx", "r11", "memory");
	return number;
}

static inline void kobox_x86_linux_stop(void)
{
	__asm__ volatile("int3" : : : "memory");
}

static inline void kobox_x86_relax(void)
{
	__asm__ volatile("pause" : : : "memory");
}

#endif
