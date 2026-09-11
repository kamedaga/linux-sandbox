/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_ASM_FSGSBASE_H
#define KOBOX_ASM_FSGSBASE_H

#ifdef KOBOX_BOOT_RUNTIME
#define x86_fsbase_read_cpu kobox_native_fsbase_read_cpu
#define x86_fsbase_write_cpu kobox_native_fsbase_write_cpu
#endif
#include_next <asm/fsgsbase.h>
#ifdef KOBOX_BOOT_RUNTIME
#undef x86_fsbase_read_cpu
#undef x86_fsbase_write_cpu

unsigned long kobox_user_fsbase_read(void);
void kobox_user_fsbase_write(unsigned long base);

static inline unsigned long x86_fsbase_read_cpu(void)
{
	return kobox_user_fsbase_read();
}

static inline void x86_fsbase_write_cpu(unsigned long base)
{
	kobox_user_fsbase_write(base);
}
#endif

#endif
