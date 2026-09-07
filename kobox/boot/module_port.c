// SPDX-License-Identifier: GPL-2.0-only

#include <linux/execmem.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>

#include <asm/sections.h>

static struct execmem_info hosted_execmem __ro_after_init;

struct execmem_info *__init execmem_arch_setup(void)
{
	unsigned long start, end;

	/* Inline assembly and native module metadata use signed PC-relative
	 * relocations even with the large C code model. Keep the entire range
	 * reachable from every core section, not merely from its entry point.
	 */
	start = max(VMALLOC_START,
		    PAGE_ALIGN((unsigned long)__bss_stop - SZ_2G + PAGE_SIZE));
	end = min(VMALLOC_END, (unsigned long)_text + SZ_2G - PAGE_SIZE);
	if (start >= end)
		panic("hosted vmalloc window is outside module relocation reach");
	/* This machine exposes 4K PTEs, not the huge-page ROX cache. Native
	 * execmem allocates RW/NX and strict_rwx publishes text RO/X before init.
	 */
	hosted_execmem.ranges[EXECMEM_DEFAULT] = (struct execmem_range) {
		.start = start,
		.end = end,
		.pgprot = PAGE_KERNEL,
		.alignment = MODULE_ALIGN,
	};
	return &hosted_execmem;
}
