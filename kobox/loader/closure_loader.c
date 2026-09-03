// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "closure_loader.h"

#include "elf64_loader.h"

#include <kobox2/sha256.h>

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__clang__)
#define KOBOX_MANUAL_ELF_CALL __attribute__((no_sanitize("function")))
#else
#define KOBOX_MANUAL_ELF_CALL
#endif

struct closure_export {
	char name[128];
	uint32_t kind;
	uintptr_t address;
};

struct closure_import {
	char consumer_name[128];
	char provider_name[128];
	uint32_t provider_node_id;
	uint32_t kind;
	uint32_t flags;
	int used;
};

struct closure_node {
	uint32_t node_id;
	uint32_t kind;
	uint32_t flags;
	uint64_t content_size;
	uint8_t content_digest[KB2_SHA256_DIGEST_SIZE];
	char namespace_name[128];
	char init_name[128];
	char quiesce_name[128];
	char cleanup_name[128];
	struct closure_export *exports;
	size_t export_count;
	struct closure_import *imports;
	size_t import_count;
	void *shared_handle;
	struct kobox_elf64_module module;
	uintptr_t init_address;
	uintptr_t quiesce_address;
	uintptr_t cleanup_address;
	struct kobox_module_context context;
	int loaded;
	int initialized;
};

struct closure_dependency {
	size_t consumer;
	size_t provider;
};

enum closure_loader_state {
	CLOSURE_LOADING = 0,
	CLOSURE_READY,
	CLOSURE_QUIESCED,
	CLOSURE_FAULTED,
};

struct kobox_closure_loader {
	struct closure_node *nodes;
	size_t node_count;
	struct closure_dependency *dependencies;
	size_t dependency_count;
	size_t *topological_order;
	kobox_closure_runtime_resolve_fn resolve_runtime;
	void *resolve_runtime_context;
	struct kobox_resource_runtime *resource_runtime;
	enum closure_loader_state state;
};

struct module_resolver_context {
	struct kobox_closure_loader *loader;
	struct closure_node *consumer;
};

static int copy_name(char destination[128], kb2_closure_string_t source)
{
	if (!source.data || !source.length || source.length >= 128)
		return -1;
	memcpy(destination, source.data, source.length);
	destination[source.length] = '\0';
	return 0;
}

static int name_compare(const char *left, const char *right)
{
	return strcmp(left, right);
}

static int digest_is_zero(const uint8_t digest[KB2_SHA256_DIGEST_SIZE])
{
	uint8_t combined = 0;
	size_t index;

	for (index = 0; index < KB2_SHA256_DIGEST_SIZE; index++)
		combined |= digest[index];
	return combined == 0;
}

static size_t find_node_index(const struct kobox_closure_loader *loader,
			      uint32_t node_id)
{
	size_t left = 0;
	size_t right = loader->node_count;

	while (left < right) {
		size_t middle = left + (right - left) / 2;

		if (loader->nodes[middle].node_id == node_id)
			return middle;
		if (loader->nodes[middle].node_id < node_id)
			left = middle + 1;
		else
			right = middle;
	}
	return SIZE_MAX;
}

static struct closure_export *find_export(struct closure_node *node,
					  const char *name, uint32_t kind)
{
	size_t index;

	for (index = 0; index < node->export_count; index++) {
		if (node->exports[index].kind == kind &&
		    !strcmp(node->exports[index].name, name))
			return &node->exports[index];
	}
	return NULL;
}

static int has_dependency(const struct kobox_closure_loader *loader,
			  size_t consumer, size_t provider)
{
	size_t index;

	for (index = 0; index < loader->dependency_count; index++) {
		if (loader->dependencies[index].consumer == consumer &&
		    loader->dependencies[index].provider == provider)
			return 1;
	}
	return 0;
}

static void free_loader(struct kobox_closure_loader *loader)
{
	size_t index;

	if (!loader)
		return;
	for (index = 0; index < loader->node_count; index++) {
		free(loader->nodes[index].imports);
		free(loader->nodes[index].exports);
	}
	free(loader->topological_order);
	free(loader->dependencies);
	free(loader->nodes);
	kobox_resource_runtime_close(&loader->resource_runtime);
	free(loader);
}

static enum kobox_closure_loader_status copy_artifacts(
	struct kobox_closure_loader *loader,
	const kb2_closure_manifest_t *manifest)
{
	uint32_t prior_node_id = 0;
	size_t index;

	loader->node_count = kb2_closure_manifest_artifact_count(manifest);
	if (!loader->node_count ||
	    loader->node_count > SIZE_MAX / sizeof(loader->nodes[0]))
		return KOBOX_CLOSURE_MALFORMED;
	loader->nodes = calloc(loader->node_count, sizeof(loader->nodes[0]));
	if (!loader->nodes)
		return KOBOX_CLOSURE_NO_MEMORY;
	for (index = 0; index < loader->node_count; index++) {
		kb2_closure_manifest_artifact_t artifact;
		struct closure_node *node = &loader->nodes[index];
		size_t prior;

		if (kb2_closure_manifest_artifact(manifest, index, &artifact) !=
			    KB2_PROTOCOL_OK ||
		    copy_name(node->namespace_name, artifact.namespace_name) ||
		    copy_name(node->init_name, artifact.init_symbol) ||
		    copy_name(node->quiesce_name, artifact.quiesce_symbol) ||
		    copy_name(node->cleanup_name, artifact.cleanup_symbol))
			return KOBOX_CLOSURE_MALFORMED;
		node->node_id = artifact.node_id;
		node->kind = artifact.kind;
		node->flags = artifact.flags;
		node->content_size = artifact.content_size;
		memcpy(node->content_digest, artifact.content_digest,
		       sizeof(node->content_digest));
		if (!node->node_id || node->node_id <= prior_node_id ||
		    node->kind > KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE ||
		    (node->flags & ~KB2_CLOSURE_ARTIFACT_FLAG_ROOT) ||
		    !node->content_size || digest_is_zero(node->content_digest) ||
		    ((node->flags & KB2_CLOSURE_ARTIFACT_FLAG_ROOT) &&
		     node->kind != KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE))
			return KOBOX_CLOSURE_MALFORMED;
		for (prior = 0; prior < index; prior++) {
			if (!strcmp(loader->nodes[prior].namespace_name,
				    node->namespace_name))
				return KOBOX_CLOSURE_MALFORMED;
		}
		prior_node_id = node->node_id;
	}
	return KOBOX_CLOSURE_OK;
}

static enum kobox_closure_loader_status copy_dependencies(
	struct kobox_closure_loader *loader,
	const kb2_closure_manifest_t *manifest)
{
	uint32_t prior_consumer = 0;
	uint32_t prior_provider = 0;
	size_t index;

	loader->dependency_count = kb2_closure_manifest_dependency_count(manifest);
	if (loader->dependency_count >
	    SIZE_MAX / sizeof(loader->dependencies[0]))
		return KOBOX_CLOSURE_MALFORMED;
	if (loader->dependency_count) {
		loader->dependencies = calloc(loader->dependency_count,
					      sizeof(loader->dependencies[0]));
		if (!loader->dependencies)
			return KOBOX_CLOSURE_NO_MEMORY;
	}
	for (index = 0; index < loader->dependency_count; index++) {
		kb2_closure_manifest_dependency_t dependency;
		size_t consumer;
		size_t provider;

		if (kb2_closure_manifest_dependency(manifest, index, &dependency) !=
			    KB2_PROTOCOL_OK ||
		    (index &&
		     (dependency.consumer_node_id < prior_consumer ||
		      (dependency.consumer_node_id == prior_consumer &&
		       dependency.provider_node_id <= prior_provider))))
			return KOBOX_CLOSURE_MALFORMED;
		consumer = find_node_index(loader, dependency.consumer_node_id);
		provider = find_node_index(loader, dependency.provider_node_id);
		if (consumer == SIZE_MAX || provider == SIZE_MAX ||
		    (loader->nodes[consumer].kind ==
			     KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER &&
		     loader->nodes[provider].kind !=
			     KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER))
			return KOBOX_CLOSURE_MALFORMED;
		loader->dependencies[index].consumer = consumer;
		loader->dependencies[index].provider = provider;
		prior_consumer = dependency.consumer_node_id;
		prior_provider = dependency.provider_node_id;
	}
	return KOBOX_CLOSURE_OK;
}

static enum kobox_closure_loader_status build_topological_order(
	struct kobox_closure_loader *loader)
{
	size_t *remaining;
	uint8_t *selected;
	uint8_t *reachable;
	size_t index;
	size_t step;

	remaining = calloc(loader->node_count, sizeof(*remaining));
	selected = calloc(loader->node_count, sizeof(*selected));
	reachable = calloc(loader->node_count, sizeof(*reachable));
	loader->topological_order = calloc(loader->node_count,
					   sizeof(loader->topological_order[0]));
	if (!remaining || !selected || !reachable || !loader->topological_order) {
		free(reachable);
		free(selected);
		free(remaining);
		return KOBOX_CLOSURE_NO_MEMORY;
	}
	for (index = 0; index < loader->dependency_count; index++)
		remaining[loader->dependencies[index].consumer]++;
	for (step = 0; step < loader->node_count; step++) {
		size_t chosen = SIZE_MAX;
		size_t edge;

		for (index = 0; index < loader->node_count; index++) {
			if (!selected[index] && !remaining[index]) {
				chosen = index;
				break;
			}
		}
		if (chosen == SIZE_MAX) {
			free(reachable);
			free(selected);
			free(remaining);
			return KOBOX_CLOSURE_MALFORMED;
		}
		selected[chosen] = 1;
		loader->topological_order[step] = chosen;
		for (edge = 0; edge < loader->dependency_count; edge++) {
			if (loader->dependencies[edge].provider == chosen)
				remaining[loader->dependencies[edge].consumer]--;
		}
	}
	for (index = 0; index < loader->node_count; index++) {
		if (loader->nodes[index].flags & KB2_CLOSURE_ARTIFACT_FLAG_ROOT)
			reachable[index] = 1;
	}
	for (step = loader->node_count; step > 0; step--) {
		size_t consumer = loader->topological_order[step - 1];
		size_t edge;

		if (!reachable[consumer])
			continue;
		for (edge = 0; edge < loader->dependency_count; edge++) {
			if (loader->dependencies[edge].consumer == consumer)
				reachable[loader->dependencies[edge].provider] = 1;
		}
	}
	for (index = 0; index < loader->node_count; index++) {
		if (!reachable[index]) {
			free(reachable);
			free(selected);
			free(remaining);
			return KOBOX_CLOSURE_MALFORMED;
		}
	}
	free(reachable);
	free(selected);
	free(remaining);
	return KOBOX_CLOSURE_OK;
}

static enum kobox_closure_loader_status copy_exports(
	struct kobox_closure_loader *loader,
	const kb2_closure_manifest_t *manifest)
{
	uint32_t prior_node_id = 0;
	char prior_name[128] = { 0 };
	size_t count = kb2_closure_manifest_export_count(manifest);
	size_t index;

	for (index = 0; index < count; index++) {
		kb2_closure_manifest_symbol_t symbol;
		size_t node_index;

		if (kb2_closure_manifest_export(manifest, index, &symbol) !=
			    KB2_PROTOCOL_OK)
			return KOBOX_CLOSURE_MALFORMED;
		node_index = find_node_index(loader, symbol.node_id);
		if (node_index == SIZE_MAX ||
		    loader->nodes[node_index].export_count == SIZE_MAX)
			return KOBOX_CLOSURE_MALFORMED;
		loader->nodes[node_index].export_count++;
	}
	for (index = 0; index < loader->node_count; index++) {
		struct closure_node *node = &loader->nodes[index];

		if (!node->export_count)
			return KOBOX_CLOSURE_MALFORMED;
		node->exports = calloc(node->export_count,
				       sizeof(node->exports[0]));
		if (!node->exports)
			return KOBOX_CLOSURE_NO_MEMORY;
		node->export_count = 0;
	}
	for (index = 0; index < count; index++) {
		kb2_closure_manifest_symbol_t symbol;
		struct closure_export *closure_export;
		struct closure_node *node;
		size_t node_index;
		char name[128];

		if (kb2_closure_manifest_export(manifest, index, &symbol) !=
			    KB2_PROTOCOL_OK ||
		    copy_name(name, symbol.name) ||
		    symbol.kind > KB2_CLOSURE_SYMBOL_OBJECT)
			return KOBOX_CLOSURE_MALFORMED;
		if (index &&
		    (symbol.node_id < prior_node_id ||
		     (symbol.node_id == prior_node_id &&
		      name_compare(name, prior_name) <= 0)))
			return KOBOX_CLOSURE_MALFORMED;
		node_index = find_node_index(loader, symbol.node_id);
		node = &loader->nodes[node_index];
		closure_export = &node->exports[node->export_count++];
		memcpy(closure_export->name, name, sizeof(name));
		closure_export->kind = symbol.kind;
		prior_node_id = symbol.node_id;
		memcpy(prior_name, name, sizeof(prior_name));
	}
	for (index = 0; index < loader->node_count; index++) {
		struct closure_node *node = &loader->nodes[index];
		struct closure_export *entry;

		entry = find_export(node, node->init_name,
				    KB2_CLOSURE_SYMBOL_FUNCTION);
		if (!entry)
			return KOBOX_CLOSURE_MALFORMED;
		entry = find_export(node, node->quiesce_name,
				    KB2_CLOSURE_SYMBOL_FUNCTION);
		if (!entry)
			return KOBOX_CLOSURE_MALFORMED;
		entry = find_export(node, node->cleanup_name,
				    KB2_CLOSURE_SYMBOL_FUNCTION);
		if (!entry)
			return KOBOX_CLOSURE_MALFORMED;
	}
	return KOBOX_CLOSURE_OK;
}

static enum kobox_closure_loader_status copy_imports(
	struct kobox_closure_loader *loader,
	const kb2_closure_manifest_t *manifest)
{
	uint32_t prior_node_id = 0;
	char prior_name[128] = { 0 };
	size_t count = kb2_closure_manifest_import_count(manifest);
	size_t index;

	for (index = 0; index < count; index++) {
		kb2_closure_manifest_import_t import_record;
		size_t node_index;

		if (kb2_closure_manifest_import(manifest, index, &import_record) !=
			    KB2_PROTOCOL_OK)
			return KOBOX_CLOSURE_MALFORMED;
		node_index = find_node_index(loader, import_record.consumer_node_id);
		if (node_index == SIZE_MAX ||
		    loader->nodes[node_index].import_count == SIZE_MAX)
			return KOBOX_CLOSURE_MALFORMED;
		loader->nodes[node_index].import_count++;
	}
	for (index = 0; index < loader->node_count; index++) {
		struct closure_node *node = &loader->nodes[index];

		if (!node->import_count)
			continue;
		node->imports = calloc(node->import_count,
				       sizeof(node->imports[0]));
		if (!node->imports)
			return KOBOX_CLOSURE_NO_MEMORY;
		node->import_count = 0;
	}
	for (index = 0; index < count; index++) {
		kb2_closure_manifest_import_t import_record;
		struct closure_import *closure_import;
		struct closure_node *consumer;
		struct closure_node *provider;
		struct closure_export *provider_export;
		size_t consumer_index;
		size_t provider_index;
		char consumer_name[128];

		if (kb2_closure_manifest_import(manifest, index, &import_record) !=
			    KB2_PROTOCOL_OK ||
		    copy_name(consumer_name, import_record.consumer_name) ||
		    import_record.kind > KB2_CLOSURE_SYMBOL_OBJECT ||
		    (import_record.flags & ~KB2_CLOSURE_IMPORT_FLAG_OPTIONAL))
			return KOBOX_CLOSURE_MALFORMED;
		if (index &&
		    (import_record.consumer_node_id < prior_node_id ||
		     (import_record.consumer_node_id == prior_node_id &&
		      name_compare(consumer_name, prior_name) <= 0)))
			return KOBOX_CLOSURE_MALFORMED;
		consumer_index = find_node_index(loader,
					 import_record.consumer_node_id);
		consumer = &loader->nodes[consumer_index];
		closure_import = &consumer->imports[consumer->import_count++];
		memcpy(closure_import->consumer_name, consumer_name,
		       sizeof(consumer_name));
		if (copy_name(closure_import->provider_name,
			      import_record.provider_name))
			return KOBOX_CLOSURE_MALFORMED;
		closure_import->provider_node_id = import_record.provider_node_id;
		closure_import->kind = import_record.kind;
		closure_import->flags = import_record.flags;
		if (!import_record.provider_node_id) {
			if (!(import_record.flags &
			      KB2_CLOSURE_IMPORT_FLAG_OPTIONAL) &&
			    !loader->resolve_runtime)
				return KOBOX_CLOSURE_MALFORMED;
		} else {
			provider_index = find_node_index(
				loader, import_record.provider_node_id);
			if (provider_index == SIZE_MAX ||
			    !has_dependency(loader, consumer_index, provider_index))
				return KOBOX_CLOSURE_MALFORMED;
			provider = &loader->nodes[provider_index];
			provider_export = find_export(
				provider, closure_import->provider_name,
				closure_import->kind);
			if (!provider_export &&
			    !(closure_import->flags &
			      KB2_CLOSURE_IMPORT_FLAG_OPTIONAL))
				return KOBOX_CLOSURE_MALFORMED;
			if (consumer->kind ==
			    KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER)
				return KOBOX_CLOSURE_MALFORMED;
		}
		prior_node_id = import_record.consumer_node_id;
		memcpy(prior_name, consumer_name, sizeof(prior_name));
	}
	return KOBOX_CLOSURE_OK;
}

static enum kobox_closure_loader_status verify_artifacts(
	const struct kobox_closure_loader_config *config,
	const struct kobox_closure_loader *loader)
{
	uint8_t digest[KB2_SHA256_DIGEST_SIZE];
	size_t index;

	if (config->artifact_count != loader->node_count)
		return KOBOX_CLOSURE_MALFORMED;
	for (index = 0; index < loader->node_count; index++) {
		const struct closure_node *node = &loader->nodes[index];
		struct kobox_elf64_symbol *expected_exports;
		struct stat status;
		void *bytes;
		int descriptor = config->artifact_descriptors[index];
		int seals;

		if (descriptor < 0 || node->content_size > SIZE_MAX ||
		    fstat(descriptor, &status) || !S_ISREG(status.st_mode) ||
		    status.st_size < 0 ||
		    (uint64_t)status.st_size != node->content_size)
			return KOBOX_CLOSURE_ARTIFACT_FAILURE;
		seals = fcntl(descriptor, F_GET_SEALS);
		if (seals < 0 ||
		    (seals & (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW |
			      F_SEAL_WRITE)) !=
			    (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW |
			     F_SEAL_WRITE))
			return KOBOX_CLOSURE_ARTIFACT_FAILURE;
		bytes = mmap(NULL, (size_t)node->content_size, PROT_READ,
			     MAP_PRIVATE, descriptor, 0);
		if (bytes == MAP_FAILED)
			return KOBOX_CLOSURE_ARTIFACT_FAILURE;
		kb2_sha256(bytes, (size_t)node->content_size, digest);
		munmap(bytes, (size_t)node->content_size);
		if (memcmp(digest, node->content_digest, sizeof(digest)))
			return KOBOX_CLOSURE_ARTIFACT_FAILURE;
		expected_exports = calloc(node->export_count,
					  sizeof(expected_exports[0]));
		if (!expected_exports)
			return KOBOX_CLOSURE_NO_MEMORY;
		{
			size_t export_index;

			for (export_index = 0; export_index < node->export_count;
			     export_index++) {
				expected_exports[export_index].name =
					node->exports[export_index].name;
				expected_exports[export_index].kind =
					node->exports[export_index].kind;
			}
		}
		if (kobox_elf64_validate_export_set_fd(
			    descriptor, expected_exports, node->export_count)) {
			free(expected_exports);
			return KOBOX_CLOSURE_SYMBOL_FAILURE;
		}
		free(expected_exports);
		if (node->kind == KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER) {
			kb2_closure_manifest_artifact_t artifact = {
				.node_id = node->node_id,
				.kind = node->kind,
				.flags = node->flags,
				.content_size = node->content_size,
				.namespace_name = {
					.data = node->namespace_name,
					.length = strlen(node->namespace_name),
				},
				.init_symbol = {
					.data = node->init_name,
					.length = strlen(node->init_name),
				},
				.quiesce_symbol = {
					.data = node->quiesce_name,
					.length = strlen(node->quiesce_name),
				},
				.cleanup_symbol = {
					.data = node->cleanup_name,
					.length = strlen(node->cleanup_name),
				},
			};

			memcpy(artifact.content_digest, node->content_digest,
			       sizeof(artifact.content_digest));
			if (!config->validate_shared ||
			    config->validate_shared(config->validate_shared_context,
						    descriptor, &artifact))
				return KOBOX_CLOSURE_ARTIFACT_FAILURE;
		}
	}
	return KOBOX_CLOSURE_OK;
}

static int resolve_module_import(void *opaque_context, const char *name,
				 uintptr_t *address_out)
{
	struct module_resolver_context *context = opaque_context;
	struct closure_import *match = NULL;
	size_t index;

	for (index = 0; index < context->consumer->import_count; index++) {
		struct closure_import *candidate = &context->consumer->imports[index];

		if (strcmp(candidate->consumer_name, name))
			continue;
		if (match)
			return -1;
		match = candidate;
	}
	if (!match)
		return -1;
	if (!match->provider_node_id) {
		if (!context->loader->resolve_runtime ||
		    context->loader->resolve_runtime(
			    context->loader->resolve_runtime_context,
			    match->provider_name, strlen(match->provider_name),
			    match->kind, address_out)) {
			if (!(match->flags & KB2_CLOSURE_IMPORT_FLAG_OPTIONAL))
				return -1;
			*address_out = 0;
		}
	} else {
		size_t provider_index = find_node_index(
			context->loader, match->provider_node_id);
		struct closure_export *provider_export;

		if (provider_index == SIZE_MAX)
			return -1;
		provider_export = find_export(
			&context->loader->nodes[provider_index],
			match->provider_name, match->kind);
		if (!provider_export || !provider_export->address) {
			if (!(match->flags & KB2_CLOSURE_IMPORT_FLAG_OPTIONAL))
				return -1;
			*address_out = 0;
		} else {
			*address_out = provider_export->address;
		}
	}
	match->used = 1;
	return 0;
}

static int shared_symbol_kind_valid(void *address, uint32_t kind)
{
#if defined(RTLD_DL_SYMENT)
	Dl_info information;
	const Elf64_Sym *symbol = NULL;
	unsigned int expected = kind == KB2_CLOSURE_SYMBOL_FUNCTION ? STT_FUNC :
								       STT_OBJECT;

	return dladdr1(address, &information, (void **)&symbol,
		       RTLD_DL_SYMENT) && symbol &&
	       ELF64_ST_TYPE(symbol->st_info) == expected;
#else
	(void)address;
	(void)kind;
	return 1;
#endif
}

static enum kobox_closure_loader_status
load_shared(struct closure_node *node, int descriptor)
{
	char path[64];
	int length;
	size_t index;

	length = snprintf(path, sizeof(path), "/proc/self/fd/%d", descriptor);
	if (length <= 0 || (size_t)length >= sizeof(path))
		return KOBOX_CLOSURE_ARTIFACT_FAILURE;
	node->shared_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!node->shared_handle)
		return KOBOX_CLOSURE_ARTIFACT_FAILURE;
	for (index = 0; index < node->export_count; index++) {
		void *symbol;

		dlerror();
		symbol = dlsym(node->shared_handle, node->exports[index].name);
		if (!symbol ||
		    !shared_symbol_kind_valid(symbol, node->exports[index].kind)) {
			dlclose(node->shared_handle);
			node->shared_handle = NULL;
			while (index > 0)
				node->exports[--index].address = 0;
			return KOBOX_CLOSURE_SYMBOL_FAILURE;
		}
		node->exports[index].address = (uintptr_t)symbol;
	}
	return KOBOX_CLOSURE_OK;
}

static enum kobox_closure_loader_status load_module(
	struct kobox_closure_loader *loader, struct closure_node *node,
	int descriptor)
{
	struct module_resolver_context resolver_context = {
		.loader = loader,
		.consumer = node,
	};
	struct kobox_elf64_export *exports;
	size_t index;
	size_t export_index;

	exports = calloc(node->export_count, sizeof(*exports));
	if (!exports)
		return KOBOX_CLOSURE_NO_MEMORY;
	for (index = 0; index < node->export_count; index++) {
		exports[index].name = node->exports[index].name;
		exports[index].kind = node->exports[index].kind;
	}
	if (kobox_elf64_module_load_fd(descriptor, exports,
				       node->export_count,
				       resolve_module_import,
				       &resolver_context, &node->module)) {
		free(exports);
		return KOBOX_CLOSURE_ARTIFACT_FAILURE;
	}
	for (index = 0; index < node->export_count; index++)
		node->exports[index].address = exports[index].address;
	free(exports);
	for (index = 0; index < node->import_count; index++) {
		if (!node->imports[index].used &&
		    !(node->imports[index].flags &
		      KB2_CLOSURE_IMPORT_FLAG_OPTIONAL)) {
			kobox_elf64_module_unload(&node->module);
			for (export_index = 0; export_index < node->export_count;
			     export_index++)
				node->exports[export_index].address = 0;
			return KOBOX_CLOSURE_SYMBOL_FAILURE;
		}
	}
	return KOBOX_CLOSURE_OK;
}

static enum kobox_closure_loader_status map_artifacts(
	const struct kobox_closure_loader_config *config,
	struct kobox_closure_loader *loader)
{
	size_t order;

	for (order = 0; order < loader->node_count; order++) {
		size_t index = loader->topological_order[order];
		struct closure_node *node = &loader->nodes[index];
		enum kobox_closure_loader_status status;

		if (node->kind == KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER)
			status = load_shared(node,
					     config->artifact_descriptors[index]);
		else
			status = load_module(loader, node,
					     config->artifact_descriptors[index]);
		if (status != KOBOX_CLOSURE_OK)
			return status;
		node->loaded = 1;
		node->init_address =
			find_export(node, node->init_name,
				    KB2_CLOSURE_SYMBOL_FUNCTION)->address;
		node->quiesce_address =
			find_export(node, node->quiesce_name,
				    KB2_CLOSURE_SYMBOL_FUNCTION)->address;
		node->cleanup_address =
			find_export(node, node->cleanup_name,
				    KB2_CLOSURE_SYMBOL_FUNCTION)->address;
		if (!node->init_address || !node->quiesce_address ||
		    !node->cleanup_address)
			return KOBOX_CLOSURE_SYMBOL_FAILURE;
	}
	return KOBOX_CLOSURE_OK;
}

KOBOX_MANUAL_ELF_CALL static int call_entry(
	uintptr_t address, const struct kobox_module_context *context)
{
	kobox_module_lifecycle_fn function;

	if (sizeof(address) != sizeof(function))
		return -1;
	memcpy(&function, &address, sizeof(function));
	return function(context);
}

static enum kobox_closure_loader_status build_module_contexts(
	const struct kobox_closure_loader_config *config,
	struct kobox_closure_loader *loader)
{
	static const uint8_t identity[KOBOX_MODULE_INTERFACE_IDENTITY_SIZE] =
		KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER;
	struct closure_export *core_export;
	struct closure_node *core_node;
	char core_name[128];
	size_t core_index;
	size_t index;

	if (!config->core_operations_node_id || !config->core_operations_symbol ||
	    !config->core_operations_symbol_length ||
	    config->core_operations_symbol_length >= sizeof(core_name))
		return KOBOX_CLOSURE_INVALID_ARGUMENT;
	memcpy(core_name, config->core_operations_symbol,
	       config->core_operations_symbol_length);
	core_name[config->core_operations_symbol_length] = '\0';
	core_index = find_node_index(loader, config->core_operations_node_id);
	if (core_index == SIZE_MAX)
		return KOBOX_CLOSURE_SYMBOL_FAILURE;
	core_node = &loader->nodes[core_index];
	core_export = find_export(core_node, core_name,
				  KB2_CLOSURE_SYMBOL_OBJECT);
	if (!core_export || !core_export->address)
		return KOBOX_CLOSURE_SYMBOL_FAILURE;
	for (index = 0; index < loader->node_count; index++) {
		struct closure_node *node = &loader->nodes[index];
		const void *view = kobox_resource_runtime_view(
			loader->resource_runtime, node->node_id);

		if (!view)
			return KOBOX_CLOSURE_RESOURCE_FAILURE;
		node->context.size = sizeof(node->context);
		memcpy(node->context.identity, identity, sizeof(identity));
		node->context.generation = kobox_resource_runtime_generation(
			loader->resource_runtime);
		node->context.node_id = node->node_id;
		node->context.resource_view = view;
		node->context.runtime_operations =
			kobox_resource_runtime_operations();
		node->context.core_operations =
			(const void *)(uintptr_t)core_export->address;
		node->context.logical_cpu_count = config->logical_cpu_count;
	}
	return KOBOX_CLOSURE_OK;
}

static void unload_artifacts(struct kobox_closure_loader *loader)
{
	size_t pass;
	size_t order;

	if (!loader->topological_order)
		return;
	for (pass = 0; pass < 2; pass++) {
		uint32_t kind = pass == 0 ?
				KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE :
				KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER;

		for (order = loader->node_count; order > 0; order--) {
			struct closure_node *node =
				&loader->nodes[loader->topological_order[order - 1]];

			if (!node->loaded || node->kind != kind)
				continue;
			if (kind == KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE)
				kobox_elf64_module_unload(&node->module);
			else if (node->shared_handle)
				dlclose(node->shared_handle);
			node->shared_handle = NULL;
			node->loaded = 0;
		}
	}
}

static enum kobox_closure_loader_status initialize_nodes(
	struct kobox_closure_loader *loader)
{
	size_t order;

	for (order = 0; order < loader->node_count; order++) {
		struct closure_node *node =
			&loader->nodes[loader->topological_order[order]];

		if (call_entry(node->init_address, &node->context)) {
			while (order > 0) {
				node = &loader->nodes[
					loader->topological_order[--order]];
				if (node->initialized)
					call_entry(node->cleanup_address,
						   &node->context);
				node->initialized = 0;
			}
			return KOBOX_CLOSURE_LIFECYCLE_FAILURE;
		}
		node->initialized = 1;
	}
	return KOBOX_CLOSURE_OK;
}

enum kobox_closure_loader_status kobox_closure_loader_open(
	const struct kobox_closure_loader_config *config,
	struct kobox_closure_loader **loader_out)
{
	struct kobox_closure_loader *loader;
	enum kobox_closure_loader_status status;

	if (!config || !config->manifest || !config->artifact_descriptors ||
	    !config->logical_cpu_count || !loader_out)
		return KOBOX_CLOSURE_INVALID_ARGUMENT;
	*loader_out = NULL;
	loader = calloc(1, sizeof(*loader));
	if (!loader)
		return KOBOX_CLOSURE_NO_MEMORY;
	loader->resolve_runtime = config->resolve_runtime;
	loader->resolve_runtime_context = config->resolve_runtime_context;
	status = copy_artifacts(loader, config->manifest);
	if (status == KOBOX_CLOSURE_OK)
		status = copy_dependencies(loader, config->manifest);
	if (status == KOBOX_CLOSURE_OK)
		status = build_topological_order(loader);
	if (status == KOBOX_CLOSURE_OK)
		status = copy_exports(loader, config->manifest);
	if (status == KOBOX_CLOSURE_OK)
		status = copy_imports(loader, config->manifest);
	if (status == KOBOX_CLOSURE_OK)
		status = verify_artifacts(config, loader);
	if (status == KOBOX_CLOSURE_OK) {
		struct kobox_resource_runtime_config resource_config = {
			.manifest = config->manifest,
			.grant = config->grant,
			.native_handles = config->resource_handles,
			.native_handle_count = config->resource_handle_count,
			.import_object = config->import_resource,
			.release_object = config->release_resource,
			.object_context = config->resource_context,
		};
		enum kobox_resource_runtime_status resource_status =
			kobox_resource_runtime_open(&resource_config,
						    &loader->resource_runtime);

		if (resource_status == KOBOX_RESOURCE_RUNTIME_NO_MEMORY)
			status = KOBOX_CLOSURE_NO_MEMORY;
		else if (resource_status == KOBOX_RESOURCE_RUNTIME_IMPORT_FAILURE)
			status = KOBOX_CLOSURE_RESOURCE_FAILURE;
		else if (resource_status != KOBOX_RESOURCE_RUNTIME_OK)
			status = KOBOX_CLOSURE_MALFORMED;
	}
	if (status == KOBOX_CLOSURE_OK)
		status = map_artifacts(config, loader);
	if (status == KOBOX_CLOSURE_OK)
		status = build_module_contexts(config, loader);
	if (status == KOBOX_CLOSURE_OK)
		status = initialize_nodes(loader);
	if (status != KOBOX_CLOSURE_OK) {
		unload_artifacts(loader);
		free_loader(loader);
		return status;
	}
	loader->state = CLOSURE_READY;
	*loader_out = loader;
	return KOBOX_CLOSURE_OK;
}

enum kobox_closure_loader_status kobox_closure_loader_symbol(
	const struct kobox_closure_loader *loader, uint32_t node_id,
	const char *name, size_t name_length, uint32_t kind,
	uintptr_t *address_out)
{
	struct closure_export *closure_export;
	size_t node_index;
	char local_name[128];

	if (!loader || !name || !name_length || name_length >= sizeof(local_name) ||
	    kind > KB2_CLOSURE_SYMBOL_OBJECT || !address_out)
		return KOBOX_CLOSURE_INVALID_ARGUMENT;
	if (loader->state != CLOSURE_READY &&
	    loader->state != CLOSURE_QUIESCED)
		return KOBOX_CLOSURE_INVALID_STATE;
	memcpy(local_name, name, name_length);
	local_name[name_length] = '\0';
	node_index = find_node_index(loader, node_id);
	if (node_index == SIZE_MAX)
		return KOBOX_CLOSURE_SYMBOL_FAILURE;
	closure_export = find_export(&loader->nodes[node_index], local_name, kind);
	if (!closure_export || !closure_export->address)
		return KOBOX_CLOSURE_SYMBOL_FAILURE;
	*address_out = closure_export->address;
	return KOBOX_CLOSURE_OK;
}

enum kobox_closure_loader_status kobox_closure_loader_root_symbol(
	const struct kobox_closure_loader *loader, const char *name,
	size_t name_length, uint32_t kind, uint32_t *node_id_out,
	uintptr_t *address_out)
{
	uintptr_t found_address = 0;
	uint32_t found_node = 0;
	size_t index;

	if (!loader || !node_id_out || !address_out)
		return KOBOX_CLOSURE_INVALID_ARGUMENT;
	for (index = 0; index < loader->node_count; index++) {
		uintptr_t address;

		if (!(loader->nodes[index].flags &
		      KB2_CLOSURE_ARTIFACT_FLAG_ROOT))
			continue;
		if (kobox_closure_loader_symbol(loader,
						loader->nodes[index].node_id,
						name, name_length, kind,
						&address) != KOBOX_CLOSURE_OK)
			continue;
		if (found_address)
			return KOBOX_CLOSURE_SYMBOL_FAILURE;
		found_address = address;
		found_node = loader->nodes[index].node_id;
	}
	if (!found_address)
		return KOBOX_CLOSURE_SYMBOL_FAILURE;
	*node_id_out = found_node;
	*address_out = found_address;
	return KOBOX_CLOSURE_OK;
}

enum kobox_closure_loader_status
kobox_closure_loader_quiesce(struct kobox_closure_loader *loader)
{
	size_t order;

	if (!loader)
		return KOBOX_CLOSURE_INVALID_ARGUMENT;
	if (loader->state != CLOSURE_READY)
		return KOBOX_CLOSURE_INVALID_STATE;
	for (order = loader->node_count; order > 0; order--) {
		struct closure_node *node =
			&loader->nodes[loader->topological_order[order - 1]];

		if (call_entry(node->quiesce_address, &node->context)) {
			loader->state = CLOSURE_FAULTED;
			return KOBOX_CLOSURE_LIFECYCLE_FAILURE;
		}
	}
	loader->state = CLOSURE_QUIESCED;
	return KOBOX_CLOSURE_OK;
}

enum kobox_closure_loader_status
kobox_closure_loader_close(struct kobox_closure_loader **loader_pointer)
{
	struct kobox_closure_loader *loader;
	enum kobox_closure_loader_status status = KOBOX_CLOSURE_OK;
	size_t order;

	if (!loader_pointer || !*loader_pointer)
		return KOBOX_CLOSURE_INVALID_ARGUMENT;
	loader = *loader_pointer;
	if (loader->state != CLOSURE_QUIESCED)
		return KOBOX_CLOSURE_INVALID_STATE;
	for (order = loader->node_count; order > 0; order--) {
		struct closure_node *node =
			&loader->nodes[loader->topological_order[order - 1]];

		if (node->initialized &&
		    call_entry(node->cleanup_address, &node->context))
			status = KOBOX_CLOSURE_LIFECYCLE_FAILURE;
		node->initialized = 0;
	}
	unload_artifacts(loader);
	free_loader(loader);
	*loader_pointer = NULL;
	return status;
}
