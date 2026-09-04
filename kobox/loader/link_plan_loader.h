/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_LINK_PLAN_LOADER_H
#define KOBOX_LINK_PLAN_LOADER_H

#include "link_plan.h"

struct kobox_link_plan_loader;

enum kobox_link_plan_status {
	KOBOX_LINK_PLAN_OK = 0,
	KOBOX_LINK_PLAN_INVALID_ARGUMENT,
	KOBOX_LINK_PLAN_NO_MEMORY,
	KOBOX_LINK_PLAN_MALFORMED,
	KOBOX_LINK_PLAN_ARTIFACT_FAILURE,
	KOBOX_LINK_PLAN_SYMBOL_FAILURE,
	KOBOX_LINK_PLAN_INVALID_STATE,
	KOBOX_LINK_PLAN_ENTRY_FAILURE,
};

enum kobox_link_plan_phase {
	KOBOX_LINK_PLAN_PHASE_NONE = 0,
	KOBOX_LINK_PLAN_PHASE_VALIDATE,
	KOBOX_LINK_PLAN_PHASE_LOAD,
	KOBOX_LINK_PLAN_PHASE_RESOLVE,
};

struct kobox_link_plan_error {
	enum kobox_link_plan_phase phase;
	size_t node_index;
	const char *node_name;
	const char *symbol_name;
};

struct kobox_link_plan_loader_config {
	const struct kobox_link_plan *plan;
	const int *artifact_descriptors;
	size_t artifact_count;
	struct kobox_link_plan_error *error;
};

enum kobox_link_plan_status kobox_link_plan_loader_open(
	const struct kobox_link_plan_loader_config *config,
	struct kobox_link_plan_loader **loader_out);
enum kobox_link_plan_status
kobox_link_plan_loader_close(struct kobox_link_plan_loader **loader);
size_t kobox_link_plan_loader_count(
	const struct kobox_link_plan_loader *loader);
enum kobox_link_plan_status kobox_link_plan_loader_start_modules(
	struct kobox_link_plan_loader *loader, size_t *failed_node_out,
	int *entry_status_out);
enum kobox_link_plan_status kobox_link_plan_loader_start_modules_through(
	struct kobox_link_plan_loader *loader, size_t node_limit,
	size_t *failed_node_out, int *entry_status_out);
enum kobox_link_plan_status kobox_link_plan_loader_stop_modules(
	struct kobox_link_plan_loader *loader);
enum kobox_link_plan_status kobox_link_plan_loader_stop_modules_to(
	struct kobox_link_plan_loader *loader, size_t node_limit);
enum kobox_link_plan_status kobox_link_plan_loader_find_node(
	const struct kobox_link_plan_loader *loader, const char *name,
	size_t *node_index_out);
enum kobox_link_plan_status kobox_link_plan_loader_export(
	const struct kobox_link_plan_loader *loader, size_t node_index,
	const char *name, uint32_t kind, uintptr_t *address_out);
size_t kobox_link_plan_loader_lifecycle_cursor(
	const struct kobox_link_plan_loader *loader);

#endif
