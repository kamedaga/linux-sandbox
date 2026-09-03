// SPDX-License-Identifier: GPL-2.0-only

#include "lifecycle.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression)                                                     \
	do {                                                                    \
		if (!(expression)) {                                              \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,    \
				__LINE__, #expression);                              \
			return -1;                                                  \
		}                                                               \
	} while (0)

static uint32_t events[32];
static size_t event_count;
static int failing_init;

static int record(uint32_t event)
{
	if (event_count >= sizeof(events) / sizeof(events[0]))
		return -1;
	events[event_count++] = event;
	return 0;
}

#define DEFINE_STEP(number)                                                   \
	static int init_##number(const struct kobox_module_context *context)     \
	{                                                                       \
		(void)context;                                                   \
		if (record(100u + number))                                      \
			return -1;                                              \
		return failing_init == number ? -number : 0;                    \
	}                                                                       \
	static int quiesce_##number(                                             \
		const struct kobox_module_context *context)                    \
	{                                                                       \
		(void)context;                                                   \
		return record(200u + number);                                   \
	}                                                                       \
	static int cleanup_##number(                                             \
		const struct kobox_module_context *context)                    \
	{                                                                       \
		(void)context;                                                   \
		return record(300u + number);                                   \
	}

DEFINE_STEP(1)
DEFINE_STEP(2)
DEFINE_STEP(3)

static const struct kobox_provider_init_entry entries[] = {
	{ 1, 0, "arena", init_1, quiesce_1, cleanup_1 },
	{ 2, 0, "percpu", init_2, quiesce_2, cleanup_2 },
	{ 2, 1, "thread", init_3, quiesce_3, cleanup_3 },
};

static int test_normal_lifecycle(void)
{
	struct kobox_provider_lifecycle lifecycle = { 0 };
	struct kobox_module_context context = { 0 };
	static const uint32_t expected[] = {
		101, 102, 103, 203, 202, 201, 303, 302, 301,
	};
	size_t index;

	event_count = 0;
	failing_init = 0;
	CHECK(kobox_provider_lifecycle_bind(&lifecycle, entries,
					    sizeof(entries) / sizeof(entries[0])) ==
	      KOBOX_PROVIDER_LIFECYCLE_OK);
	CHECK(kobox_provider_lifecycle_init(&lifecycle, &context) ==
	      KOBOX_PROVIDER_LIFECYCLE_OK);
	CHECK(lifecycle.state == KOBOX_PROVIDER_ACTIVE);
	CHECK(kobox_provider_lifecycle_quiesce(&lifecycle, &context) ==
	      KOBOX_PROVIDER_LIFECYCLE_OK);
	CHECK(kobox_provider_lifecycle_cleanup(&lifecycle, &context) ==
	      KOBOX_PROVIDER_LIFECYCLE_OK);
	CHECK(lifecycle.state == KOBOX_PROVIDER_CLEAN &&
	      event_count == sizeof(expected) / sizeof(expected[0]));
	for (index = 0; index < event_count; index++)
		CHECK(events[index] == expected[index]);
	return 0;
}

static int test_init_rollback(void)
{
	struct kobox_provider_lifecycle lifecycle = { 0 };
	struct kobox_module_context context = { 0 };
	static const uint32_t expected[] = { 101, 102, 201, 301 };
	size_t index;

	event_count = 0;
	failing_init = 2;
	CHECK(kobox_provider_lifecycle_bind(&lifecycle, entries,
					    sizeof(entries) / sizeof(entries[0])) ==
	      KOBOX_PROVIDER_LIFECYCLE_OK);
	CHECK(kobox_provider_lifecycle_init(&lifecycle, &context) ==
	      KOBOX_PROVIDER_LIFECYCLE_ENTRY_FAILURE);
	CHECK(lifecycle.state == KOBOX_PROVIDER_CLEAN &&
	      lifecycle.failure.phase == KOBOX_PROVIDER_PHASE_INIT &&
	      lifecycle.failure.entry_index == 1 &&
	      lifecycle.failure.entry_status == -2 &&
	      event_count == sizeof(expected) / sizeof(expected[0]));
	for (index = 0; index < event_count; index++)
		CHECK(events[index] == expected[index]);
	return 0;
}

static int test_table_validation(void)
{
	struct kobox_provider_lifecycle lifecycle = { 0 };
	const struct kobox_provider_init_entry unordered[] = {
		entries[1], entries[0],
	};

	CHECK(kobox_provider_lifecycle_bind(&lifecycle, NULL, 0) ==
	      KOBOX_PROVIDER_LIFECYCLE_INVALID_ARGUMENT);
	CHECK(kobox_provider_lifecycle_bind(
		      &lifecycle, unordered,
		      sizeof(unordered) / sizeof(unordered[0])) ==
	      KOBOX_PROVIDER_LIFECYCLE_INVALID_TABLE);
	return 0;
}

int main(void)
{
	return test_normal_lifecycle() || test_init_rollback() ||
		       test_table_validation() ?
		       EXIT_FAILURE :
		       EXIT_SUCCESS;
}
