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
	const unsigned long reach = SZ_2G - PAGE_SIZE;
	unsigned long image_end = PAGE_ALIGN((unsigned long)__bss_stop);
	unsigned long reachable_start, reachable_end, start, end;

	/* Inline assembly and native module metadata use signed PC-relative
	 * relocations even with the large C code model. Keep the entire range
	 * reachable from every core section, not merely from its entry point.
	 * Saturate at the native address limits: a low fixed core must not turn
	 * the lower-bound subtraction into an unsigned high address.
	 */
	reachable_start = image_end > reach ? image_end - reach : 0;
	reachable_end = (unsigned long)_text > ULONG_MAX - reach ?
		ULONG_MAX : (unsigned long)_text + reach;
	start = max(VMALLOC_START, reachable_start);
	end = min(VMALLOC_END, reachable_end);
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
