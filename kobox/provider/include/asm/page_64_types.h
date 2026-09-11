/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_ASM_PAGE_64_TYPES_H
#define KOBOX_PROVIDER_ASM_PAGE_64_TYPES_H

#include_next <asm/page_64_types.h>

#ifdef KOBOX_BOOT_RUNTIME
#include "../../../arch/x86_64/user_layout.h"
/* ELF, stack ASLR and both mmap layouts must use the same machine limit.
 * Lowering only task_size_max() leaves the upstream default stack above it.
 */
#undef DEFAULT_MAP_WINDOW
#define DEFAULT_MAP_WINDOW KOBOX_X86_USER_END
#endif

#endif
