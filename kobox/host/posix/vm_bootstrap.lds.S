/* SPDX-License-Identifier: GPL-2.0-only */
#include "../../arch/x86_64/user_layout.h"

ENTRY(_start)
PHDRS
{
	text PT_LOAD FILEHDR PHDRS FLAGS(5);
	data PT_LOAD FLAGS(6);
	stack PT_GNU_STACK FLAGS(6);
}
SECTIONS
{
	. = KOBOX_X86_USER_END + SIZEOF_HEADERS;
	.text : { *(.text .text.* .ltext .ltext.*) } :text
	.rodata : { *(.rodata .rodata.* .lrodata .lrodata.*) } :text
	. = ALIGN(4096);
	.data : { *(.data .data.* .ldata .ldata.*) } :data
	.bss : { *(.bss .bss.* .lbss .lbss.*) *(COMMON) } :data
	ASSERT(. < KOBOX_X86_MACHINE_CONTEXT, "machine image exceeds reservation")
	/DISCARD/ : { *(.eh_frame .eh_frame_hdr .comment .note.*) }
}
