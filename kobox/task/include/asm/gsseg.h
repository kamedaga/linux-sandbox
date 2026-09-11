/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_ASM_GSSEG_H
#define KOBOX_ASM_GSSEG_H

#ifdef KOBOX_BOOT_RUNTIME
#define load_gs_index kobox_native_load_gs_index
#endif
#include_next <asm/gsseg.h>
#ifdef KOBOX_BOOT_RUNTIME
#undef load_gs_index
#include <asm/segment.h>

static inline void load_gs_index(unsigned int selector)
{
	kobox_user_load_segment(KOBOX_SEG_gs, selector);
}
#endif

#endif
