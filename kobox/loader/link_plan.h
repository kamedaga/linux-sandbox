/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_LINK_PLAN_H
#define KOBOX_LINK_PLAN_H

#include <stddef.h>
#include <stdint.h>

#define KOBOX_LINK_PLAN_SHARED_PROVIDER 0u
#define KOBOX_LINK_PLAN_RELOCATABLE_MODULE 1u
#define KOBOX_LINK_PLAN_SYMBOL_FUNCTION 0u
#define KOBOX_LINK_PLAN_SYMBOL_OBJECT 1u
#define KOBOX_LINK_PLAN_NO_PROVIDER UINT32_MAX
#define KOBOX_LINK_PLAN_DIGEST_SIZE 32u

struct kobox_link_plan_export {
	const char *name;
	uint32_t kind;
};

struct kobox_link_plan_import {
	const char *name;
	uint32_t provider_index;
	uint32_t optional;
};

struct kobox_link_plan_node {
	const char *name;
	uint32_t kind;
	uint64_t content_size;
	uint8_t content_digest[KOBOX_LINK_PLAN_DIGEST_SIZE];
	const struct kobox_link_plan_export *exports;
	size_t export_count;
	const struct kobox_link_plan_import *imports;
	size_t import_count;
	const char *init_symbol;
	const char *cleanup_symbol;
};

struct kobox_link_plan {
	const char *identity;
	const struct kobox_link_plan_node *nodes;
	size_t node_count;
};

#endif
