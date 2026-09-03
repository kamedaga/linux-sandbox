// SPDX-License-Identifier: GPL-2.0-only

#include "arena.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_ARENA_SIZE (16u * 1024u * 1024u)
#define TEST_THREAD_COUNT 4u
#define TEST_ITERATIONS 10000u

#define CHECK(expression)                                                     \
	do {                                                                    \
		if (!(expression)) {                                              \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,    \
				__LINE__, #expression);                              \
			return -1;                                                  \
		}                                                               \
	} while (0)

struct thread_argument {
	struct kobox_provider_arena *arena;
	uint32_t seed;
};

static void *exercise_arena(void *opaque)
{
	struct thread_argument *argument = opaque;
	uint32_t value = argument->seed;
	size_t iteration;

	for (iteration = 0; iteration < TEST_ITERATIONS; iteration++) {
		uint32_t order;
		void *allocation;

		value = value * UINT32_C(1664525) + UINT32_C(1013904223);
		order = value % 4u;
		allocation = kobox_provider_arena_allocate(argument->arena, order);
		if (!allocation ||
		    kobox_provider_arena_release(argument->arena, allocation,
					 order) != KOBOX_PROVIDER_ARENA_OK)
			return (void *)(uintptr_t)1;
	}
	return NULL;
}

static int test_arena(void)
{
	struct kobox_provider_arena *arena;
	struct thread_argument arguments[TEST_THREAD_COUNT];
	pthread_t threads[TEST_THREAD_COUNT];
	void **allocations;
	void *memory;
	void *allocation;
	void *page_base;
	size_t total_pages;
	size_t index;

	CHECK(!posix_memalign(&memory, KOBOX_PROVIDER_ARENA_PAGE_SIZE,
			      TEST_ARENA_SIZE));
	CHECK(kobox_provider_arena_init(memory, TEST_ARENA_SIZE, &arena) ==
	      KOBOX_PROVIDER_ARENA_OK);
	page_base = kobox_provider_arena_page_base(arena);
	total_pages = kobox_provider_arena_total_pages(arena);
	CHECK(page_base && total_pages > 1 &&
	      kobox_provider_arena_free_pages(arena) == total_pages);
	CHECK(!kobox_provider_arena_allocate(
		arena, kobox_provider_arena_max_order(arena) + 1));

	allocation = kobox_provider_arena_allocate(arena, 3);
	CHECK(allocation &&
	      ((uintptr_t)allocation - (uintptr_t)page_base) %
		      (8u * KOBOX_PROVIDER_ARENA_PAGE_SIZE) ==
		      0);
	CHECK(kobox_provider_arena_destroy(&arena) == KOBOX_PROVIDER_ARENA_BUSY &&
	      arena);
	CHECK(kobox_provider_arena_release(arena, allocation, 2) ==
	      KOBOX_PROVIDER_ARENA_INVALID_ALLOCATION);
	CHECK(kobox_provider_arena_release(arena, allocation, 3) ==
	      KOBOX_PROVIDER_ARENA_OK);
	CHECK(kobox_provider_arena_release(arena, allocation, 3) ==
	      KOBOX_PROVIDER_ARENA_INVALID_ALLOCATION);

	allocations = calloc(total_pages, sizeof(allocations[0]));
	CHECK(allocations);
	for (index = 0; index < total_pages; index++) {
		allocations[index] = kobox_provider_arena_allocate(arena, 0);
		CHECK(allocations[index]);
	}
	CHECK(!kobox_provider_arena_allocate(arena, 0));
	CHECK(kobox_provider_arena_free_pages(arena) == 0);
	for (index = total_pages; index > 0; index--)
		CHECK(kobox_provider_arena_release(arena, allocations[index - 1],
						   0) ==
		      KOBOX_PROVIDER_ARENA_OK);
	free(allocations);
	CHECK(kobox_provider_arena_free_pages(arena) == total_pages);

	for (index = 0; index < TEST_THREAD_COUNT; index++) {
		arguments[index] = (struct thread_argument){
			.arena = arena,
			.seed = (uint32_t)index + 1,
		};
		CHECK(!pthread_create(&threads[index], NULL, exercise_arena,
				      &arguments[index]));
	}
	for (index = 0; index < TEST_THREAD_COUNT; index++) {
		void *result;

		CHECK(!pthread_join(threads[index], &result) && !result);
	}
	CHECK(kobox_provider_arena_free_pages(arena) == total_pages);
	CHECK(kobox_provider_arena_destroy(&arena) == KOBOX_PROVIDER_ARENA_OK &&
	      !arena);
	free(memory);
	return 0;
}

int main(void)
{
	return test_arena() ? EXIT_FAILURE : EXIT_SUCCESS;
}
