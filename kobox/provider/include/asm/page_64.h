/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_PAGE_64_H
#define _ASM_X86_PAGE_64_H

#include <asm/page_64_types.h>

#ifndef __ASSEMBLER__
#include <linux/kmsan-checks.h>

extern unsigned long max_pfn;
extern unsigned long phys_base;
extern unsigned long page_offset_base;
extern unsigned long vmalloc_base;
extern unsigned long vmemmap_base;
extern unsigned long direct_map_physmem_end;
unsigned long kobox_provider_get_task_size_limit(void);

static __always_inline unsigned long __phys_addr_nodebug(unsigned long address)
{
	unsigned long relative = address - __START_KERNEL_map;

	address = relative + ((address > relative) ? phys_base :
			      (__START_KERNEL_map - PAGE_OFFSET));
	return address;
}

#ifdef CONFIG_DEBUG_VIRTUAL
extern unsigned long __phys_addr(unsigned long address);
extern unsigned long __phys_addr_symbol(unsigned long address);
#else
#define __phys_addr(address) __phys_addr_nodebug(address)
#define __phys_addr_symbol(address) \
	((unsigned long)(address) - __START_KERNEL_map + phys_base)
#endif

#define __phys_reloc_hide(address) (address)

void clear_page_orig(void *page);
void copy_page(void *to, void *from);

static __always_inline void clear_page(void *page)
{
	kmsan_unpoison_memory(page, PAGE_SIZE);
	clear_page_orig(page);
}

static __always_inline unsigned long task_size_max(void)
{
	return kobox_provider_get_task_size_limit();
}

#endif /* !__ASSEMBLER__ */

#ifdef CONFIG_X86_VSYSCALL_EMULATION
#define __HAVE_ARCH_GATE_AREA 1
#endif

#endif /* _ASM_X86_PAGE_64_H */
