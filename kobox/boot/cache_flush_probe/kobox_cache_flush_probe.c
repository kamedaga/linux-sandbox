// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <asm/cpufeature.h>
#include <drm/drm_cache.h>

static int __init cache_flush_probe(void)
{
	struct page *first, *pages[2];
	struct scatterlist sg[2];
	struct sg_table table = {.sgl = sg, .nents = 2, .orig_nents = 2};
	unsigned int eax, ebx, ecx, edx, line;
	unsigned char *bytes;
	int result = 0;

	cpuid(1, &eax, &ebx, &ecx, &edx);
	line = ((ebx >> 8) & 0xff) * 8;
	if (!(edx & BIT(X86_FEATURE_CLFLUSH % 32)) ||
	    !boot_cpu_has(X86_FEATURE_CLFLUSH) ||
	    !line || line > PAGE_SIZE || (line & (line - 1)) ||
	    boot_cpu_data.x86_clflush_size != line)
		return -EOPNOTSUPP;
	first = alloc_pages(GFP_KERNEL | __GFP_ZERO, 1);
	if (!first)
		return -ENOMEM;
	pages[0] = first;
	pages[1] = first + 1;
	bytes = page_address(first);
	bytes[0] = 0x17;
	bytes[PAGE_SIZE] = 0x42;
	bytes[2 * PAGE_SIZE - 1] = 0x89;
	sg_init_table(sg, 2);
	sg_set_page(&sg[0], pages[0], PAGE_SIZE, 0);
	sg_set_page(&sg[1], pages[1], PAGE_SIZE, 0);
	/* Execute all three unmodified DRM entry points, including an unaligned
	 * cross-page range. DRM's missing-CLFLUSH fallback is WBINVD; the feature
	 * precheck above rejects that old port before executing privileged code. */
	drm_clflush_pages(pages, ARRAY_SIZE(pages));
	drm_clflush_sg(&table);
	drm_clflush_virt_range(bytes + 3, PAGE_SIZE + 17);
	if (bytes[0] != 0x17 || bytes[PAGE_SIZE] != 0x42 ||
	    bytes[2 * PAGE_SIZE - 1] != 0x89)
		result = -EIO;
	__free_pages(first, 1);
	if (!result)
		pr_info("kobox-cache: DRM pages, sg and unaligned virtual flush PASS line=%u\n", line);
	return result;
}

static void __exit cache_flush_exit(void) { }
module_init(cache_flush_probe);
module_exit(cache_flush_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hosted DRM CLFLUSH capability and execution regression probe");
