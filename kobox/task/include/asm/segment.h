/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_ASM_SEGMENT_H
#define KOBOX_ASM_SEGMENT_H

#include_next <asm/segment.h>

#if defined(KOBOX_BOOT_RUNTIME) && !defined(__ASSEMBLER__)
enum kobox_user_segment {
	KOBOX_SEG_fs, KOBOX_SEG_gs, KOBOX_SEG_ds,
	KOBOX_SEG_es, KOBOX_SEG_cs, KOBOX_SEG_ss,
};

void kobox_user_load_segment(enum kobox_user_segment segment, unsigned short value);
unsigned short kobox_user_read_segment(enum kobox_user_segment segment);

/* Native FS belongs to the runtime pthread, never to its Linux current. */
#undef loadsegment
#undef savesegment
#define loadsegment(segment, value) kobox_user_load_segment(KOBOX_SEG_##segment, value)
#define savesegment(segment, value) ((value) = kobox_user_read_segment(KOBOX_SEG_##segment))
#endif

#endif
