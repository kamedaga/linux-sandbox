/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_ASM_SYNC_CORE_H
#define KOBOX_BOOT_ASM_SYNC_CORE_H

#include <linux/compiler.h>

static __always_inline void sync_core(void)
{
	unsigned int eax = 0, ebx, ecx = 0, edx;

	/* CPUID is serializing, available on every supported x86-64 CPU, and
	 * usable at CPL 3. Native IRET-to-self embeds kernel-model addresses.
	 * This is local serialization; upstream still owns cross-CPU rendezvous.
	 */
	asm volatile("cpuid"
		     : "+a" (eax), "=b" (ebx), "+c" (ecx), "=d" (edx)
		     : : "memory");
}

static __always_inline void sync_core_before_usermode(void)
{
	sync_core();
}

#endif
