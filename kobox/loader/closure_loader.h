/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_CLOSURE_LOADER_H
#define KOBOX_CLOSURE_LOADER_H

#include <kobox2/closure_manifest.h>

#include <stddef.h>
#include <stdint.h>

struct kobox_closure_loader;

enum kobox_closure_loader_status {
	KOBOX_CLOSURE_OK = 0,
	KOBOX_CLOSURE_INVALID_ARGUMENT,
	KOBOX_CLOSURE_NO_MEMORY,
	KOBOX_CLOSURE_MALFORMED,
	KOBOX_CLOSURE_ARTIFACT_FAILURE,
	KOBOX_CLOSURE_SYMBOL_FAILURE,
	KOBOX_CLOSURE_RESOURCE_FAILURE,
	KOBOX_CLOSURE_LIFECYCLE_FAILURE,
	KOBOX_CLOSURE_INVALID_STATE,
};

typedef int (*kobox_closure_runtime_resolve_fn)(
	void *context, const char *name, size_t name_length, uint32_t kind,
	uintptr_t *address_out);
typedef int (*kobox_closure_resource_bind_fn)(
	void *context, const kb2_closure_manifest_resource_t *resource,
	uint32_t node_id);
typedef int (*kobox_closure_shared_validate_fn)(
	void *context, int artifact_descriptor,
	const kb2_closure_manifest_artifact_t *artifact);

struct kobox_closure_loader_config {
	const kb2_closure_manifest_t *manifest;
	const int *artifact_descriptors;
	size_t artifact_count;
	kobox_closure_runtime_resolve_fn resolve_runtime;
	void *resolve_runtime_context;
	kobox_closure_resource_bind_fn bind_resource;
	void *bind_resource_context;
	kobox_closure_shared_validate_fn validate_shared;
	void *validate_shared_context;
};

enum kobox_closure_loader_status kobox_closure_loader_open(
	const struct kobox_closure_loader_config *config,
	struct kobox_closure_loader **loader_out);
enum kobox_closure_loader_status kobox_closure_loader_symbol(
	const struct kobox_closure_loader *loader, uint32_t node_id,
	const char *name, size_t name_length, uint32_t kind,
	uintptr_t *address_out);
enum kobox_closure_loader_status kobox_closure_loader_root_symbol(
	const struct kobox_closure_loader *loader, const char *name,
	size_t name_length, uint32_t kind, uint32_t *node_id_out,
	uintptr_t *address_out);
enum kobox_closure_loader_status
kobox_closure_loader_quiesce(struct kobox_closure_loader *loader);
enum kobox_closure_loader_status
kobox_closure_loader_close(struct kobox_closure_loader **loader);

#endif
