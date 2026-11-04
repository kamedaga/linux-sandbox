/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_REPORTING_RANGES_H
#define KOBOX_REPORTING_RANGES_H
#include "host.h"

#define KOBOX_REPORTING_RANGES 32U
#define KOBOX_REPORTING_PAGES 1024U

/* Linux PFNs are arena indices, never native physical addresses. */
struct kobox_reporting_segment {
	uint64_t pfn;
	uint64_t offset;
	uint64_t length;
};

static inline int kobox_reporting_convert(struct kobox_reporting_segment input,
	uint64_t arena_pages, struct kobox_linux_memory_range *out)
{
	if (input.offset % KOBOX_LINUX_MEMORY_PAGE_SIZE || !input.length ||
	    input.length % KOBOX_LINUX_MEMORY_PAGE_SIZE || input.pfn >= arena_pages)
		return -1;
	uint64_t offset_pages = input.offset / KOBOX_LINUX_MEMORY_PAGE_SIZE;
	if (offset_pages >= arena_pages - input.pfn)
		return -1;
	out->first_page = input.pfn + offset_pages;
	out->page_count = input.length / KOBOX_LINUX_MEMORY_PAGE_SIZE;
	return out->page_count <= arena_pages - out->first_page ? 0 : -1;
}

/* Preflight the entire SG before making any host calls. SG order is arbitrary.
 * Gaps stay gaps: do not discard allocated PFNs between isolated blocks.
 * Input and batch arrays are on the allocated worker stack, outside the SG.
 */
static inline int kobox_reporting_submit(struct kobox_linux_memory_range *ranges,
	size_t count, int (*discard)(void *, const struct kobox_linux_memory_range *, size_t),
	void *backing)
{
	struct kobox_linux_memory_range batch[KOBOX_REPORTING_RANGES];
	size_t i, j, used = 0;
	uint64_t pages = 0;
	if (!count || count > KOBOX_REPORTING_RANGES || !discard)
		return -1;
	for (i = 1; i < count; i++) {
		struct kobox_linux_memory_range item = ranges[i];
		for (j = i; j && ranges[j - 1].first_page > item.first_page; j--)
			ranges[j] = ranges[j - 1];
		ranges[j] = item;
	}
	for (i = 0; i < count; i++) {
		if (!ranges[i].page_count ||
		    ranges[i].page_count > ~(uint64_t)0 - ranges[i].first_page ||
		    (i && ranges[i - 1].first_page + ranges[i - 1].page_count > ranges[i].first_page))
			return -1;
	}
	for (i = 0; i < count; i++) {
		uint64_t first = ranges[i].first_page, left = ranges[i].page_count;
		while (left) {
			uint64_t take = left < KOBOX_REPORTING_PAGES - pages ? left : KOBOX_REPORTING_PAGES - pages;
			batch[used++] = (struct kobox_linux_memory_range){first, take};
			pages += take;
			first += take;
			left -= take;
			if (used == KOBOX_REPORTING_RANGES || pages == KOBOX_REPORTING_PAGES) {
				int status = discard(backing, batch, used);
				if (status)
					return status;
				used = 0;
				pages = 0;
			}
		}
	}
	return used ? discard(backing, batch, used) : 0;
}
#endif
