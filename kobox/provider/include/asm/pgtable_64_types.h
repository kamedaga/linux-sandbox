/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_PGTABLE_64_TYPES_H
#define KOBOX_PROVIDER_PGTABLE_64_TYPES_H

#include_next <asm/pgtable_64_types.h>

#ifdef KOBOX_HOSTED_RAM
#ifdef CONFIG_KMSAN
#error "Hosted RAM does not provide the KMSAN virtual-address partitions"
#endif
#ifndef __ASSEMBLY__
extern unsigned long kobox_memory_vmalloc_end;
#endif

/* Linux owns vmalloc allocation and address classification. Both must use
 * the actual reserved host window, not native x86's 32-TiB region size.
 */
#undef VMEMORY_END
#define VMEMORY_END kobox_memory_vmalloc_end
#endif

#endif
