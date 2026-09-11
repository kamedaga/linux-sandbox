/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_USER_LAYOUT_H
#define KOBOX_X86_USER_LAYOUT_H

/* Hosted x86 machine layout, not a wire ABI. The upper 4 GiB of the
 * four-level native user range hold only the machine entry and per-context
 * signal stacks. Guest ELF placement and mmap remain Linux decisions below
 * this boundary; no native libc, loader, or ordinary TLS occupies that MM.
 */
#define KOBOX_X86_USER_START 0x10000
#define KOBOX_X86_USER_END 0x7fff00000000
#define KOBOX_X86_NATIVE_USER_END 0x7ffffffff000
#define KOBOX_X86_MACHINE_CONTEXT (KOBOX_X86_USER_END + 0x100000)
#define KOBOX_X86_MACHINE_ALLOC (KOBOX_X86_USER_END + 0x200000)
#define KOBOX_X86_MACHINE_END 0x7ffffff00000

#endif
