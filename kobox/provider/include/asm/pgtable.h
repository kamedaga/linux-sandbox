/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_ASM_PGTABLE_H
#define KOBOX_PROVIDER_ASM_PGTABLE_H

#include_next <asm/pgtable.h>

#ifdef KOBOX_BOOT_RUNTIME
/*
 * PAGE_OFFSET locates the host's RAM alias, not the guest kernel half.
 * Copying init_mm's low host mappings into a guest PGD would also share
 * user page-table descendants between otherwise independent Linux mms.
 * Keep upstream pgd allocation, but retain the x86 kernel-half boundary.
 */
#undef KERNEL_PGD_BOUNDARY
#define KERNEL_PGD_BOUNDARY PGD_KERNEL_START
#endif

#endif
