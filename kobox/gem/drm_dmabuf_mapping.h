/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_DRM_DMABUF_MAPPING_H
#define KOBOX_DRM_DMABUF_MAPPING_H

#include "../boot/drm_file.h"
#include <linux/dma-buf.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <asm/pgtable.h>

struct kobox_drm_dmabuf_mapping {
	struct dma_buf *buffer;
	struct iosys_map map;
};

/* This exports pinned normal RAM, never a guessed linear device aperture.
 * A driver's requested WB vmap can be changed to WC for local memory. Check
 * the actual hosted page tables before publishing WB page-view metadata.
 * Hosted PAT slot zero is WB (boot/cpu_port.c). No native PAT state is read.
 */
static struct page *kobox_drm_wb_mapping_page(void *address)
{
	unsigned int level;
	pte_t *pte = lookup_address((unsigned long)address, &level);
	u64 mask;
	struct page *page;

	if (!pte || !pte_present(*pte))
		return NULL;
	if (level == PG_LEVEL_4K)
		mask = _PAGE_CACHE_MASK;
	else if (level == PG_LEVEL_2M || level == PG_LEVEL_1G)
		mask = _PAGE_PWT | _PAGE_PCD | _PAGE_PAT_LARGE;
	else
		return NULL;
	if (pte_val(*pte) & mask)
		return NULL;
	if (is_vmalloc_addr(address))
		page = vmalloc_to_page(address);
	else if (virt_addr_valid(address))
		page = virt_to_page(address);
	else
		return NULL;
	if (!page || !pfn_valid(page_to_pfn(page)) || PageReserved(page) ||
	    is_zone_device_page(page))
		return NULL;
	return page;
}

/* On success consumes the caller's DMA-BUF reference. CPU access is a
 * separate, explicit DMA_BUF_IOCTL_SYNC transaction; vmap is not a fence or
 * cache-coherency substitute. Both object and vmap survive GEM handle close.
 */
static int kobox_drm_dmabuf_map(struct dma_buf *buffer, u64 length,
	u64 *pages, size_t capacity, struct kobox_linux_drm_map_pages *result,
	struct kobox_drm_dmabuf_mapping *mapping)
{
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	size_t count, index;
	int error;

	if (!buffer || !length || length != buffer->size ||
	    length & (PAGE_SIZE - 1) || !pages || !result || !mapping ||
	    mapping->buffer)
		return -EINVAL;
	count = length >> PAGE_SHIFT;
	if (count > capacity)
		return -ENOSPC;
	error = dma_buf_vmap_unlocked(buffer, &map);
	if (error)
		return error;
	if (map.is_iomem || !map.vaddr ||
	    (unsigned long)map.vaddr & (PAGE_SIZE - 1) ||
	    length > ULONG_MAX - (unsigned long)map.vaddr) {
		error = -EOPNOTSUPP;
		goto unmap;
	}
	for (index = 0; index < count; index++) {
		struct page *page = kobox_drm_wb_mapping_page(
			map.vaddr + index * PAGE_SIZE);

		if (!page) {
			error = -EOPNOTSUPP;
			goto unmap;
		}
		pages[index] = page_to_pfn(page);
	}
	*mapping = (struct kobox_drm_dmabuf_mapping) {
		.buffer = buffer, .map = map,
	};
	*result = (struct kobox_linux_drm_map_pages) {
		.length = length, .page_count = count,
		.backing_kind = KOBOX_DRM_BACKING_RAM,
		.cache_policy = KOBOX_DRM_CACHE_WB,
	};
	return 0;
unmap:
	dma_buf_vunmap_unlocked(buffer, &map);
	return error;
}

static void kobox_drm_dmabuf_unmap(struct kobox_drm_dmabuf_mapping *mapping)
{
	if (!mapping->buffer)
		return;
	dma_buf_vunmap_unlocked(mapping->buffer, &mapping->map);
	dma_buf_put(mapping->buffer);
	mapping->buffer = NULL;
}
#endif
