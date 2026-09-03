/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_PROVIDER_ARENA_H
#define KOBOX_PROVIDER_ARENA_H

#include <stddef.h>
#include <stdint.h>

#define KOBOX_PROVIDER_ARENA_PAGE_SIZE 4096u

struct kobox_provider_arena;

enum kobox_provider_arena_status {
	KOBOX_PROVIDER_ARENA_OK = 0,
	KOBOX_PROVIDER_ARENA_INVALID_ARGUMENT,
	KOBOX_PROVIDER_ARENA_TOO_SMALL,
	KOBOX_PROVIDER_ARENA_BUSY,
	KOBOX_PROVIDER_ARENA_INVALID_ALLOCATION,
	KOBOX_PROVIDER_ARENA_CORRUPT,
};

enum kobox_provider_arena_status kobox_provider_arena_init(
	void *memory, size_t length, struct kobox_provider_arena **arena_out);
enum kobox_provider_arena_status kobox_provider_arena_destroy(
	struct kobox_provider_arena **arena);
void *kobox_provider_arena_allocate(struct kobox_provider_arena *arena,
				    uint32_t order);
void *kobox_provider_arena_allocate_owned(struct kobox_provider_arena *arena,
					  uint32_t order, const void *owner,
					  uint32_t kind);
enum kobox_provider_arena_status kobox_provider_arena_release(
	struct kobox_provider_arena *arena, void *allocation, uint32_t order);
enum kobox_provider_arena_status kobox_provider_arena_release_owned(
	struct kobox_provider_arena *arena, void *allocation, uint32_t order,
	const void *owner, uint32_t kind);
int kobox_provider_arena_contains(const struct kobox_provider_arena *arena,
				  const void *address, size_t length);
int kobox_provider_arena_validate_owner(struct kobox_provider_arena *arena,
					void *allocation, uint32_t order,
					const void *owner, uint32_t kind);
void *kobox_provider_arena_page_base(struct kobox_provider_arena *arena);
size_t kobox_provider_arena_total_pages(
	const struct kobox_provider_arena *arena);
size_t kobox_provider_arena_free_pages(
	const struct kobox_provider_arena *arena);
uint32_t kobox_provider_arena_max_order(
	const struct kobox_provider_arena *arena);

#endif
