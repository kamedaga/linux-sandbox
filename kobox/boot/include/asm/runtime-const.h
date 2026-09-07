/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_RUNTIME_CONST_H
#define KOBOX_BOOT_RUNTIME_CONST_H

#ifdef __ASSEMBLY__
#include_next <asm/runtime-const.h>
#else

/* Preserve Linux's emitted sites, values and initialization traversal. Only
 * the destination mapping for the instruction bytes belongs to the host.
 */
#define __runtime_fixup_ptr kobox_native_runtime_fixup_ptr
#define __runtime_fixup_shift kobox_native_runtime_fixup_shift
#include_next <asm/runtime-const.h>
#undef __runtime_fixup_ptr
#undef __runtime_fixup_shift

#include <asm/sync_core.h>

void *kobox_linux_boot_text_alias(void *where, unsigned long size);

static inline void __runtime_fixup_ptr(void *where, unsigned long value)
{
	unsigned long *alias = kobox_linux_boot_text_alias(where, sizeof(value));

	*alias = value;
	sync_core();
}

static inline void __runtime_fixup_shift(void *where, unsigned long value)
{
	unsigned char *alias = kobox_linux_boot_text_alias(where, sizeof(*alias));

	*alias = value;
	sync_core();
}

#endif /* __ASSEMBLY__ */
#endif /* KOBOX_BOOT_RUNTIME_CONST_H */
