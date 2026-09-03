// SPDX-License-Identifier: GPL-2.0-only

#include "arena.h"

#include <limits.h>

#define KOBOX_ARENA_MAGIC UINT64_C(0x6b62326172656e61)
#define KOBOX_ARENA_NO_PAGE UINT64_MAX
#define KOBOX_ARENA_ORDER_COUNT (sizeof(size_t) * CHAR_BIT)

enum arena_page_state {
	ARENA_PAGE_INTERIOR = 0,
	ARENA_PAGE_FREE,
	ARENA_PAGE_ALLOCATED,
};

struct arena_page {
	uint64_t next;
	uintptr_t owner;
	uint32_t order;
	uint32_t state;
	uint32_t kind;
	uint32_t reserved;
};

struct kobox_provider_arena {
	uint64_t magic;
	size_t length;
	size_t page_offset;
	size_t page_count;
	size_t free_page_count;
	uint32_t maximum_order;
	uint32_t lock;
	uint64_t free_heads[KOBOX_ARENA_ORDER_COUNT];
};

static int align_up(size_t value, size_t alignment, size_t *result_out)
{
	size_t mask = alignment - 1;

	if (value > SIZE_MAX - mask)
		return 0;
	*result_out = (value + mask) & ~mask;
	return 1;
}

static uint32_t floor_order(size_t value)
{
	uint32_t order = 0;

	while (value > 1) {
		value >>= 1;
		order++;
	}
	return order;
}

static size_t order_pages(uint32_t order)
{
	return (size_t)1 << order;
}

static struct arena_page *arena_pages(struct kobox_provider_arena *arena)
{
	size_t offset;

	(void)align_up(sizeof(*arena), _Alignof(struct arena_page), &offset);
	return (struct arena_page *)((unsigned char *)arena + offset);
}

static int layout_fits(size_t length, size_t page_count,
		       size_t *page_offset_out)
{
	size_t descriptor_offset;
	size_t metadata_size;
	size_t page_offset;

	if (!page_count ||
	    !align_up(sizeof(struct kobox_provider_arena),
		      _Alignof(struct arena_page), &descriptor_offset) ||
	    page_count >
		    (SIZE_MAX - descriptor_offset) / sizeof(struct arena_page))
		return 0;
	metadata_size = descriptor_offset + page_count * sizeof(struct arena_page);
	if (!align_up(metadata_size, KOBOX_PROVIDER_ARENA_PAGE_SIZE,
		      &page_offset) ||
	    page_offset > length ||
	    page_count >
		    (length - page_offset) / KOBOX_PROVIDER_ARENA_PAGE_SIZE)
		return 0;
	*page_offset_out = page_offset;
	return 1;
}

static size_t usable_page_count(size_t length, size_t *page_offset_out)
{
	size_t left = 1;
	size_t right = length / KOBOX_PROVIDER_ARENA_PAGE_SIZE;
	size_t best = 0;
	size_t best_offset = 0;

	while (left <= right) {
		size_t middle = left + (right - left) / 2;
		size_t offset;

		if (layout_fits(length, middle, &offset)) {
			best = middle;
			best_offset = offset;
			left = middle + 1;
		} else {
			if (!middle)
				break;
			right = middle - 1;
		}
	}
	*page_offset_out = best_offset;
	return best;
}

static int arena_valid(const struct kobox_provider_arena *arena)
{
	size_t descriptor_offset;

	if (!arena ||
	    __atomic_load_n(&arena->magic, __ATOMIC_ACQUIRE) !=
		    KOBOX_ARENA_MAGIC ||
	    !arena->page_count ||
	    arena->maximum_order >= KOBOX_ARENA_ORDER_COUNT ||
	    arena->page_offset % KOBOX_PROVIDER_ARENA_PAGE_SIZE ||
	    arena->page_offset > arena->length ||
	    arena->page_count >
		    (arena->length - arena->page_offset) /
			    KOBOX_PROVIDER_ARENA_PAGE_SIZE ||
	    !align_up(sizeof(*arena), _Alignof(struct arena_page),
		      &descriptor_offset) ||
	    descriptor_offset > arena->page_offset ||
	    arena->page_count >
		    (arena->page_offset - descriptor_offset) /
			    sizeof(struct arena_page))
		return 0;
	return 1;
}

static void arena_lock(struct kobox_provider_arena *arena)
{
	while (__atomic_exchange_n(&arena->lock, 1, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#endif
	}
}

static void arena_unlock(struct kobox_provider_arena *arena)
{
	__atomic_store_n(&arena->lock, 0, __ATOMIC_RELEASE);
}

static void free_insert(struct kobox_provider_arena *arena, size_t page_index,
			uint32_t order)
{
	struct arena_page *pages = arena_pages(arena);

	pages[page_index].state = ARENA_PAGE_FREE;
	pages[page_index].order = order;
	pages[page_index].owner = 0;
	pages[page_index].kind = 0;
	pages[page_index].next = arena->free_heads[order];
	arena->free_heads[order] = page_index;
}

static int free_remove(struct kobox_provider_arena *arena, size_t page_index,
		       uint32_t order)
{
	struct arena_page *pages = arena_pages(arena);
	uint64_t *link = &arena->free_heads[order];

	while (*link != KOBOX_ARENA_NO_PAGE) {
		if (*link >= arena->page_count)
			return 0;
		if (*link == page_index) {
			*link = pages[page_index].next;
			return 1;
		}
		link = &pages[*link].next;
	}
	return 0;
}

enum kobox_provider_arena_status kobox_provider_arena_init(
	void *memory, size_t length, struct kobox_provider_arena **arena_out)
{
	struct kobox_provider_arena *arena = memory;
	struct arena_page *pages;
	size_t page_offset;
	size_t page_count;
	size_t index;
	size_t remaining;

	if (!arena_out)
		return KOBOX_PROVIDER_ARENA_INVALID_ARGUMENT;
	*arena_out = NULL;
	if (!memory || (uintptr_t)memory % KOBOX_PROVIDER_ARENA_PAGE_SIZE)
		return KOBOX_PROVIDER_ARENA_INVALID_ARGUMENT;
	page_count = usable_page_count(length, &page_offset);
	if (!page_count)
		return KOBOX_PROVIDER_ARENA_TOO_SMALL;
	for (index = 0; index < sizeof(*arena); index++)
		((unsigned char *)arena)[index] = 0;
	arena->length = length;
	arena->page_offset = page_offset;
	arena->page_count = page_count;
	arena->free_page_count = page_count;
	arena->maximum_order = floor_order(page_count);
	for (index = 0; index < KOBOX_ARENA_ORDER_COUNT; index++)
		arena->free_heads[index] = KOBOX_ARENA_NO_PAGE;
	pages = arena_pages(arena);
	for (index = 0; index < page_count; index++) {
		pages[index].next = KOBOX_ARENA_NO_PAGE;
		pages[index].state = ARENA_PAGE_INTERIOR;
	}
	index = 0;
	remaining = page_count;
	while (remaining) {
		uint32_t order = floor_order(remaining);
		size_t block_pages = order_pages(order);

		free_insert(arena, index, order);
		index += block_pages;
		remaining -= block_pages;
	}
	__atomic_store_n(&arena->magic, KOBOX_ARENA_MAGIC, __ATOMIC_RELEASE);
	*arena_out = arena;
	return KOBOX_PROVIDER_ARENA_OK;
}

enum kobox_provider_arena_status kobox_provider_arena_destroy(
	struct kobox_provider_arena **arena_pointer)
{
	struct kobox_provider_arena *arena;

	if (!arena_pointer || !arena_valid(*arena_pointer))
		return KOBOX_PROVIDER_ARENA_INVALID_ARGUMENT;
	arena = *arena_pointer;
	arena_lock(arena);
	if (!arena_valid(arena)) {
		arena_unlock(arena);
		return KOBOX_PROVIDER_ARENA_INVALID_ARGUMENT;
	}
	if (__atomic_load_n(&arena->free_page_count, __ATOMIC_ACQUIRE) !=
	    arena->page_count) {
		arena_unlock(arena);
		return KOBOX_PROVIDER_ARENA_BUSY;
	}
	__atomic_store_n(&arena->magic, 0, __ATOMIC_RELEASE);
	arena_unlock(arena);
	*arena_pointer = NULL;
	return KOBOX_PROVIDER_ARENA_OK;
}

void *kobox_provider_arena_allocate_owned(struct kobox_provider_arena *arena,
					  uint32_t order, const void *owner,
					  uint32_t kind)
{
	struct arena_page *pages;
	size_t page_index;
	uint32_t available_order;

	if (!arena_valid(arena) || order > arena->maximum_order)
		return NULL;
	arena_lock(arena);
	if (!arena_valid(arena)) {
		arena_unlock(arena);
		return NULL;
	}
	for (available_order = order;
	     available_order <= arena->maximum_order; available_order++) {
		if (arena->free_heads[available_order] != KOBOX_ARENA_NO_PAGE)
			break;
	}
	if (available_order > arena->maximum_order) {
		arena_unlock(arena);
		return NULL;
	}
	pages = arena_pages(arena);
	page_index = arena->free_heads[available_order];
	if (page_index >= arena->page_count ||
	    pages[page_index].state != ARENA_PAGE_FREE ||
	    pages[page_index].order != available_order) {
		arena_unlock(arena);
		return NULL;
	}
	arena->free_heads[available_order] = pages[page_index].next;
	while (available_order > order) {
		size_t buddy;

		available_order--;
		buddy = page_index + order_pages(available_order);
		pages[page_index].state = ARENA_PAGE_FREE;
		pages[page_index].order = available_order;
		free_insert(arena, buddy, available_order);
	}
	pages[page_index].state = ARENA_PAGE_ALLOCATED;
	pages[page_index].order = order;
	pages[page_index].next = KOBOX_ARENA_NO_PAGE;
	pages[page_index].owner = (uintptr_t)owner;
	pages[page_index].kind = kind;
	(void)__atomic_sub_fetch(&arena->free_page_count, order_pages(order),
				 __ATOMIC_RELEASE);
	arena_unlock(arena);
	return (unsigned char *)arena + arena->page_offset +
	       page_index * KOBOX_PROVIDER_ARENA_PAGE_SIZE;
}

void *kobox_provider_arena_allocate(struct kobox_provider_arena *arena,
				    uint32_t order)
{
	return kobox_provider_arena_allocate_owned(arena, order, NULL, 0);
}

enum kobox_provider_arena_status kobox_provider_arena_release_owned(
	struct kobox_provider_arena *arena, void *allocation, uint32_t order,
	const void *owner, uint32_t kind)
{
	struct arena_page *pages;
	uintptr_t page_base;
	uintptr_t address;
	size_t page_index;
	size_t block_pages;

	if (!arena_valid(arena) || !allocation ||
	    order > arena->maximum_order)
		return KOBOX_PROVIDER_ARENA_INVALID_ARGUMENT;
	page_base = (uintptr_t)arena + arena->page_offset;
	address = (uintptr_t)allocation;
	if (address < page_base ||
	    address - page_base >=
		    arena->page_count * KOBOX_PROVIDER_ARENA_PAGE_SIZE ||
	    (address - page_base) % KOBOX_PROVIDER_ARENA_PAGE_SIZE)
		return KOBOX_PROVIDER_ARENA_INVALID_ALLOCATION;
	page_index = (address - page_base) / KOBOX_PROVIDER_ARENA_PAGE_SIZE;
	block_pages = order_pages(order);
	if (page_index % block_pages ||
	    block_pages > arena->page_count - page_index)
		return KOBOX_PROVIDER_ARENA_INVALID_ALLOCATION;
	arena_lock(arena);
	if (!arena_valid(arena)) {
		arena_unlock(arena);
		return KOBOX_PROVIDER_ARENA_INVALID_ARGUMENT;
	}
	pages = arena_pages(arena);
	if (pages[page_index].state != ARENA_PAGE_ALLOCATED ||
	    pages[page_index].order != order ||
	    pages[page_index].owner != (uintptr_t)owner ||
	    pages[page_index].kind != kind) {
		arena_unlock(arena);
		return KOBOX_PROVIDER_ARENA_INVALID_ALLOCATION;
	}
	(void)__atomic_add_fetch(&arena->free_page_count, block_pages,
				 __ATOMIC_RELEASE);
	pages[page_index].state = ARENA_PAGE_FREE;
	pages[page_index].order = order;
	while (order < arena->maximum_order) {
		size_t buddy = page_index ^ order_pages(order);
		size_t merged;

		if (buddy >= arena->page_count ||
		    pages[buddy].state != ARENA_PAGE_FREE ||
		    pages[buddy].order != order)
			break;
		if (!free_remove(arena, buddy, order)) {
			arena_unlock(arena);
			return KOBOX_PROVIDER_ARENA_CORRUPT;
		}
		pages[page_index].state = ARENA_PAGE_INTERIOR;
		pages[buddy].state = ARENA_PAGE_INTERIOR;
		merged = page_index < buddy ? page_index : buddy;
		order++;
		page_index = merged;
		pages[page_index].state = ARENA_PAGE_FREE;
		pages[page_index].order = order;
	}
	free_insert(arena, page_index, order);
	arena_unlock(arena);
	return KOBOX_PROVIDER_ARENA_OK;
}

enum kobox_provider_arena_status kobox_provider_arena_release(
	struct kobox_provider_arena *arena, void *allocation, uint32_t order)
{
	return kobox_provider_arena_release_owned(arena, allocation, order, NULL,
						 0);
}

int kobox_provider_arena_contains(const struct kobox_provider_arena *arena,
				  const void *address, size_t length)
{
	uintptr_t base;
	uintptr_t value;
	size_t span;

	if (!arena_valid(arena) || !address)
		return 0;
	base = (uintptr_t)arena + arena->page_offset;
	value = (uintptr_t)address;
	span = arena->page_count * KOBOX_PROVIDER_ARENA_PAGE_SIZE;
	return value >= base && value - base <= span &&
	       length <= span - (value - base);
}

int kobox_provider_arena_validate_owner(struct kobox_provider_arena *arena,
					void *allocation, uint32_t order,
					const void *owner, uint32_t kind)
{
	struct arena_page *pages;
	uintptr_t page_base;
	uintptr_t address;
	size_t page_index;
	size_t block_pages;
	int valid;

	if (!arena_valid(arena) || !allocation ||
	    order > arena->maximum_order)
		return 0;
	page_base = (uintptr_t)arena + arena->page_offset;
	address = (uintptr_t)allocation;
	if (address < page_base ||
	    address - page_base >=
		    arena->page_count * KOBOX_PROVIDER_ARENA_PAGE_SIZE ||
	    (address - page_base) % KOBOX_PROVIDER_ARENA_PAGE_SIZE)
		return 0;
	page_index = (address - page_base) / KOBOX_PROVIDER_ARENA_PAGE_SIZE;
	block_pages = order_pages(order);
	if (page_index % block_pages ||
	    block_pages > arena->page_count - page_index)
		return 0;
	arena_lock(arena);
	pages = arena_pages(arena);
	valid = arena_valid(arena) &&
		pages[page_index].state == ARENA_PAGE_ALLOCATED &&
		pages[page_index].order == order &&
		pages[page_index].owner == (uintptr_t)owner &&
		pages[page_index].kind == kind;
	arena_unlock(arena);
	return valid;
}

void *kobox_provider_arena_page_base(struct kobox_provider_arena *arena)
{
	if (!arena_valid(arena))
		return NULL;
	return (unsigned char *)arena + arena->page_offset;
}

size_t kobox_provider_arena_total_pages(
	const struct kobox_provider_arena *arena)
{
	return arena_valid(arena) ? arena->page_count : 0;
}

size_t kobox_provider_arena_free_pages(
	const struct kobox_provider_arena *arena)
{
	return arena_valid(arena) ?
		       __atomic_load_n(&arena->free_page_count, __ATOMIC_ACQUIRE) :
		       0;
}

uint32_t kobox_provider_arena_max_order(
	const struct kobox_provider_arena *arena)
{
	return arena_valid(arena) ? arena->maximum_order : 0;
}
