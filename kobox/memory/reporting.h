/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_MEMORY_REPORTING_H
#define KOBOX_MEMORY_REPORTING_H
#include "host.h"
struct page;
/* Caller owns the allocation throughout this check and lease creation. This
 * asserts admission, never populates: buddy contains resident pages only. */
void kobox_linux_resident_assert_page(struct page *page);
int kobox_linux_reporting_start(const struct kobox_linux_memory_layout *memory);
struct kobox_linux_pressure_result {
 uint64_t requested_pages, linux_freed_pages, reported_ok_pages;
};
/* Explicit allocation-load test; never called by production adapters. */
int kobox_linux_resident_test_start(void);
int kobox_linux_pressure_start(int (*wake)(void *), void *context);
int kobox_linux_pressure_submit(uint64_t pages, uint64_t budget_ns);
int kobox_linux_pressure_collect(struct kobox_linux_pressure_result *result);
void kobox_linux_pressure_stop(void);
#endif
