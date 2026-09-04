// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "link_plan_loader.h"

#include "elf64_loader.h"

#include <kobox2/sha256.h>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct loaded_export {
	const char *name;
	uint32_t kind;
	uintptr_t address;
};

struct loaded_node {
	const struct kobox_link_plan_node *definition;
	struct loaded_export *exports;
	void *shared_handle;
	struct kobox_elf64_module module;
	int loaded;
	int initialized;
};

struct kobox_link_plan_loader {
	struct loaded_node *nodes;
	size_t node_count;
	size_t lifecycle_cursor;
};

struct module_resolver_context {
	struct kobox_link_plan_loader *loader;
	const struct kobox_link_plan_node *consumer;
	uint8_t *used_imports;
	struct kobox_link_plan_error *error;
	size_t node_index;
};

static void set_error(struct kobox_link_plan_error *error,
		      enum kobox_link_plan_phase phase, size_t node_index,
		      const char *node_name, const char *symbol_name)
{
	if (!error)
		return;
	error->phase = phase;
	error->node_index = node_index;
	error->node_name = node_name;
	error->symbol_name = symbol_name;
}

static int symbol_kind_valid(void *address, uint32_t kind)
{
#if defined(RTLD_DL_SYMENT)
	Dl_info information;
	const Elf64_Sym *symbol = NULL;
	unsigned int expected = kind == KOBOX_LINK_PLAN_SYMBOL_FUNCTION ?
				STT_FUNC : STT_OBJECT;

	return dladdr1(address, &information, (void **)&symbol,
		       RTLD_DL_SYMENT) && symbol &&
	       ELF64_ST_TYPE(symbol->st_info) == expected;
#else
	(void)address;
	(void)kind;
	return 1;
#endif
}

static int validate_node(const struct kobox_link_plan *plan, size_t index)
{
	const struct kobox_link_plan_node *node = &plan->nodes[index];
	uint8_t digest = 0;
	size_t item;

	if (!node->name || !node->name[0] ||
	    node->kind > KOBOX_LINK_PLAN_RELOCATABLE_MODULE ||
	    !node->content_size ||
	    (node->kind == KOBOX_LINK_PLAN_SHARED_PROVIDER &&
	     node->import_count) ||
	    (node->export_count && !node->exports) ||
	    (node->import_count && !node->imports))
		return -1;
	for (item = 0; item < sizeof(node->content_digest); item++)
		digest |= node->content_digest[item];
	if (!digest)
		return -1;
	for (item = 0; item < index; item++) {
		if (!strcmp(plan->nodes[item].name, node->name))
			return -1;
	}
	for (item = 0; item < node->export_count; item++) {
		size_t prior;

		if (!node->exports[item].name || !node->exports[item].name[0] ||
		    node->exports[item].kind > KOBOX_LINK_PLAN_SYMBOL_OBJECT)
			return -1;
		for (prior = 0; prior < item; prior++) {
			if (!strcmp(node->exports[prior].name,
				    node->exports[item].name))
				return -1;
		}
	}
	for (item = 0; item < node->import_count; item++) {
		size_t prior;

		if (!node->imports[item].name || !node->imports[item].name[0] ||
		    node->imports[item].optional > 1 ||
		    (node->imports[item].provider_index !=
			     KOBOX_LINK_PLAN_NO_PROVIDER &&
		     node->imports[item].provider_index >= index) ||
		    (!node->imports[item].optional &&
		     node->imports[item].provider_index ==
			     KOBOX_LINK_PLAN_NO_PROVIDER))
			return -1;
		for (prior = 0; prior < item; prior++) {
			if (!strcmp(node->imports[prior].name,
				    node->imports[item].name))
				return -1;
		}
	}
	if (node->kind == KOBOX_LINK_PLAN_SHARED_PROVIDER &&
	    (node->init_symbol || node->cleanup_symbol))
		return -1;
	if (!!node->init_symbol != !!node->cleanup_symbol)
		return -1;
	if (node->init_symbol) {
		int init_found = 0;
		int cleanup_found = 0;

		for (item = 0; item < node->export_count; item++) {
			if (!strcmp(node->exports[item].name, node->init_symbol) &&
			    node->exports[item].kind ==
				    KOBOX_LINK_PLAN_SYMBOL_FUNCTION)
				init_found = 1;
			if (!strcmp(node->exports[item].name,
				    node->cleanup_symbol) &&
			    node->exports[item].kind ==
				    KOBOX_LINK_PLAN_SYMBOL_FUNCTION)
				cleanup_found = 1;
		}
		if (!init_found || !cleanup_found)
			return -1;
	}
	return 0;
}

static int verify_artifact(int descriptor,
			   const struct kobox_link_plan_node *node)
{
	uint8_t digest[KOBOX_LINK_PLAN_DIGEST_SIZE];
	struct kobox_elf64_symbol *exports = NULL;
	struct stat status;
	void *mapping = MAP_FAILED;
	size_t index;
	int seals;
	int result = -1;

	if (descriptor < 0 || node->content_size > SIZE_MAX ||
	    fstat(descriptor, &status) || !S_ISREG(status.st_mode) ||
	    status.st_size < 0 ||
	    (uint64_t)status.st_size != node->content_size)
		return -1;
	seals = fcntl(descriptor, F_GET_SEALS);
	if (seals < 0 ||
	    (seals & (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW |
		      F_SEAL_WRITE)) !=
		    (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE))
		return -1;
	mapping = mmap(NULL, (size_t)node->content_size, PROT_READ, MAP_PRIVATE,
		       descriptor, 0);
	if (mapping == MAP_FAILED)
		return -1;
	kb2_sha256(mapping, (size_t)node->content_size, digest);
	if (memcmp(digest, node->content_digest, sizeof(digest)))
		goto out;
	if (node->export_count) {
		exports = calloc(node->export_count, sizeof(*exports));
		if (!exports)
			goto out;
	}
	for (index = 0; index < node->export_count; index++) {
		exports[index].name = node->exports[index].name;
		exports[index].kind = node->exports[index].kind;
	}
	if (kobox_elf64_validate_export_subset_fd(
		    descriptor, exports, node->export_count)) {
		goto out;
	}
	result = 0;

out:
	free(exports);
	munmap(mapping, (size_t)node->content_size);
	return result;
}

static struct loaded_export *find_export(struct loaded_node *node,
					 const char *name)
{
	size_t index;

	for (index = 0; index < node->definition->export_count; index++) {
		if (!strcmp(node->exports[index].name, name))
			return &node->exports[index];
	}
	return NULL;
}

static int resolve_module_import(void *opaque, const char *name,
				 uintptr_t *address_out)
{
	struct module_resolver_context *context = opaque;
	const struct kobox_link_plan_import *found = NULL;
	size_t found_index = 0;
	size_t index;

	for (index = 0; index < context->consumer->import_count; index++) {
		if (strcmp(context->consumer->imports[index].name, name))
			continue;
		if (found)
			return -1;
		found = &context->consumer->imports[index];
		found_index = index;
	}
	if (!found) {
		set_error(context->error, KOBOX_LINK_PLAN_PHASE_RESOLVE,
			  context->node_index, context->consumer->name, name);
		return -1;
	}
	context->used_imports[found_index] = 1;
	if (found->provider_index == KOBOX_LINK_PLAN_NO_PROVIDER) {
		*address_out = 0;
		return found->optional ? 0 : -1;
	}
	{
		struct loaded_export *provider_export = find_export(
			&context->loader->nodes[found->provider_index], name);

		if (!provider_export || !provider_export->address) {
			set_error(context->error, KOBOX_LINK_PLAN_PHASE_RESOLVE,
				  context->node_index, context->consumer->name,
				  name);
			return -1;
		}
		*address_out = provider_export->address;
	}
	return 0;
}

static enum kobox_link_plan_status load_shared(struct loaded_node *node,
					       int descriptor)
{
	char path[64];
	size_t index;
	int length;

	length = snprintf(path, sizeof(path), "/proc/self/fd/%d", descriptor);
	if (length <= 0 || (size_t)length >= sizeof(path))
		return KOBOX_LINK_PLAN_ARTIFACT_FAILURE;
	node->shared_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!node->shared_handle)
		return KOBOX_LINK_PLAN_ARTIFACT_FAILURE;
	for (index = 0; index < node->definition->export_count; index++) {
		void *symbol;

		dlerror();
		symbol = dlsym(node->shared_handle,
			       node->definition->exports[index].name);
		if (!symbol || !symbol_kind_valid(
				       symbol,
				       node->definition->exports[index].kind)) {
			dlclose(node->shared_handle);
			node->shared_handle = NULL;
			return KOBOX_LINK_PLAN_SYMBOL_FAILURE;
		}
		node->exports[index].address = (uintptr_t)symbol;
	}
	return KOBOX_LINK_PLAN_OK;
}

static enum kobox_link_plan_status load_module(
	struct kobox_link_plan_loader *loader, struct loaded_node *node,
	int descriptor, size_t node_index, struct kobox_link_plan_error *error)
{
	struct module_resolver_context context = {
		.loader = loader,
		.consumer = node->definition,
		.error = error,
		.node_index = node_index,
	};
	struct kobox_elf64_export *exports = NULL;
	size_t index;
	enum kobox_link_plan_status status = KOBOX_LINK_PLAN_NO_MEMORY;

	if (node->definition->import_count) {
		context.used_imports = calloc(node->definition->import_count, 1);
		if (!context.used_imports)
			goto out;
	}
	if (node->definition->export_count) {
		exports = calloc(node->definition->export_count, sizeof(*exports));
		if (!exports)
			goto out;
	}
	for (index = 0; index < node->definition->export_count; index++) {
		exports[index].name = node->definition->exports[index].name;
		exports[index].kind = node->definition->exports[index].kind;
	}
	if (kobox_elf64_module_load_fd(
		    descriptor, exports, node->definition->export_count,
		    resolve_module_import, &context, &node->module)) {
		status = KOBOX_LINK_PLAN_ARTIFACT_FAILURE;
		goto out;
	}
	for (index = 0; index < node->definition->export_count; index++)
		node->exports[index].address = exports[index].address;
	for (index = 0; index < node->definition->import_count; index++) {
		if (!context.used_imports[index] &&
		    !node->definition->imports[index].optional) {
			set_error(error, KOBOX_LINK_PLAN_PHASE_RESOLVE, node_index,
				  node->definition->name,
				  node->definition->imports[index].name);
			kobox_elf64_module_unload(&node->module);
			status = KOBOX_LINK_PLAN_SYMBOL_FAILURE;
			goto out;
		}
	}
	status = KOBOX_LINK_PLAN_OK;

out:
	free(exports);
	free(context.used_imports);
	return status;
}

static void unload_nodes(struct kobox_link_plan_loader *loader)
{
	size_t index;

	for (index = loader->node_count; index > 0; index--) {
		struct loaded_node *node = &loader->nodes[index - 1];

		if (!node->loaded)
			continue;
		if (node->definition->kind == KOBOX_LINK_PLAN_SHARED_PROVIDER)
			dlclose(node->shared_handle);
		else
			kobox_elf64_module_unload(&node->module);
		node->loaded = 0;
	}
}

static uintptr_t node_export_address(const struct loaded_node *node,
				     const char *name)
{
	size_t index;

	for (index = 0; index < node->definition->export_count; index++) {
		if (!strcmp(node->exports[index].name, name))
			return node->exports[index].address;
	}
	return 0;
}

/* Raw module entries do not carry Clang's function-type sanitizer prefix. */
#if defined(__clang__)
#define KOBOX_NO_SANITIZE_FUNCTION __attribute__((no_sanitize("function")))
#else
#define KOBOX_NO_SANITIZE_FUNCTION
#endif

static KOBOX_NO_SANITIZE_FUNCTION int invoke_module_init(uintptr_t address)
{
	return ((int (*)(void))address)();
}

static KOBOX_NO_SANITIZE_FUNCTION void invoke_module_cleanup(uintptr_t address)
{
	((void (*)(void))address)();
}

enum kobox_link_plan_status kobox_link_plan_loader_start_modules_through(
	struct kobox_link_plan_loader *loader, size_t node_limit,
	size_t *failed_node_out, int *entry_status_out)
{
	size_t starting_cursor;
	size_t index;

	if (!loader || node_limit > loader->node_count)
		return KOBOX_LINK_PLAN_INVALID_ARGUMENT;
	if (node_limit < loader->lifecycle_cursor)
		return KOBOX_LINK_PLAN_INVALID_STATE;
	starting_cursor = loader->lifecycle_cursor;
	if (failed_node_out)
		*failed_node_out = SIZE_MAX;
	if (entry_status_out)
		*entry_status_out = 0;
	for (index = loader->lifecycle_cursor; index < node_limit; index++) {
		struct loaded_node *node = &loader->nodes[index];
		uintptr_t init_entry;
		int status;

		if (node->initialized)
			return KOBOX_LINK_PLAN_INVALID_STATE;
		if (node->definition->kind != KOBOX_LINK_PLAN_RELOCATABLE_MODULE ||
		    !node->definition->init_symbol) {
			loader->lifecycle_cursor = index + 1;
			continue;
		}
		init_entry = node_export_address(
			node, node->definition->init_symbol);
		if (!init_entry)
			return KOBOX_LINK_PLAN_SYMBOL_FAILURE;
		status = invoke_module_init(init_entry);
		if (status) {
			if (failed_node_out)
				*failed_node_out = index;
			if (entry_status_out)
				*entry_status_out = status;
			while (index > starting_cursor) {
				uintptr_t cleanup_entry;

				node = &loader->nodes[--index];
				if (!node->initialized)
					continue;
				cleanup_entry = node_export_address(
					node, node->definition->cleanup_symbol);
				invoke_module_cleanup(cleanup_entry);
				node->initialized = 0;
			}
			loader->lifecycle_cursor = starting_cursor;
			return KOBOX_LINK_PLAN_ENTRY_FAILURE;
		}
		node->initialized = 1;
		loader->lifecycle_cursor = index + 1;
	}
	return KOBOX_LINK_PLAN_OK;
}

enum kobox_link_plan_status kobox_link_plan_loader_start_modules(
	struct kobox_link_plan_loader *loader, size_t *failed_node_out,
	int *entry_status_out)
{
	if (!loader)
		return KOBOX_LINK_PLAN_INVALID_ARGUMENT;
	return kobox_link_plan_loader_start_modules_through(
		loader, loader->node_count, failed_node_out, entry_status_out);
}

enum kobox_link_plan_status kobox_link_plan_loader_stop_modules(
	struct kobox_link_plan_loader *loader)
{
	if (!loader)
		return KOBOX_LINK_PLAN_INVALID_ARGUMENT;
	return kobox_link_plan_loader_stop_modules_to(loader, 0);
}

enum kobox_link_plan_status kobox_link_plan_loader_stop_modules_to(
	struct kobox_link_plan_loader *loader, size_t node_limit)
{
	size_t index;

	if (!loader || node_limit > loader->lifecycle_cursor)
		return KOBOX_LINK_PLAN_INVALID_ARGUMENT;
	for (index = loader->lifecycle_cursor; index > node_limit; index--) {
		struct loaded_node *node = &loader->nodes[index - 1];
		uintptr_t cleanup_entry;

		if (!node->initialized)
			continue;
		cleanup_entry = node_export_address(
			node, node->definition->cleanup_symbol);
		if (!cleanup_entry)
			return KOBOX_LINK_PLAN_SYMBOL_FAILURE;
		invoke_module_cleanup(cleanup_entry);
		node->initialized = 0;
	}
	loader->lifecycle_cursor = node_limit;
	return KOBOX_LINK_PLAN_OK;
}

enum kobox_link_plan_status kobox_link_plan_loader_find_node(
	const struct kobox_link_plan_loader *loader, const char *name,
	size_t *node_index_out)
{
	size_t index;

	if (!loader || !name || !name[0] || !node_index_out)
		return KOBOX_LINK_PLAN_INVALID_ARGUMENT;
	for (index = 0; index < loader->node_count; index++) {
		if (!strcmp(loader->nodes[index].definition->name, name)) {
			*node_index_out = index;
			return KOBOX_LINK_PLAN_OK;
		}
	}
	return KOBOX_LINK_PLAN_SYMBOL_FAILURE;
}

enum kobox_link_plan_status kobox_link_plan_loader_export(
	const struct kobox_link_plan_loader *loader, size_t node_index,
	const char *name, uint32_t kind, uintptr_t *address_out)
{
	const struct loaded_node *node;
	size_t index;

	if (!loader || node_index >= loader->node_count || !name || !name[0] ||
	    kind > KOBOX_LINK_PLAN_SYMBOL_OBJECT || !address_out)
		return KOBOX_LINK_PLAN_INVALID_ARGUMENT;
	node = &loader->nodes[node_index];
	for (index = 0; index < node->definition->export_count; index++) {
		if (!strcmp(node->exports[index].name, name) &&
		    node->exports[index].kind == kind &&
		    node->exports[index].address) {
			*address_out = node->exports[index].address;
			return KOBOX_LINK_PLAN_OK;
		}
	}
	return KOBOX_LINK_PLAN_SYMBOL_FAILURE;
}

size_t kobox_link_plan_loader_lifecycle_cursor(
	const struct kobox_link_plan_loader *loader)
{
	return loader ? loader->lifecycle_cursor : 0;
}

enum kobox_link_plan_status kobox_link_plan_loader_open(
	const struct kobox_link_plan_loader_config *config,
	struct kobox_link_plan_loader **loader_out)
{
	struct kobox_link_plan_loader *loader;
	size_t index;

	if (!config || !config->plan || !config->artifact_descriptors ||
	    !loader_out || !config->plan->identity ||
	    strcmp(config->plan->identity, "dev") || !config->plan->nodes ||
	    !config->plan->node_count ||
	    config->plan->node_count > SIZE_MAX / sizeof(loader->nodes[0]) ||
	    config->artifact_count != config->plan->node_count)
		return KOBOX_LINK_PLAN_INVALID_ARGUMENT;
	*loader_out = NULL;
	if (config->error)
		*config->error = (struct kobox_link_plan_error){ 0 };
	loader = calloc(1, sizeof(*loader));
	if (!loader)
		return KOBOX_LINK_PLAN_NO_MEMORY;
	loader->node_count = config->plan->node_count;
	loader->nodes = calloc(loader->node_count, sizeof(loader->nodes[0]));
	if (!loader->nodes) {
		free(loader);
		return KOBOX_LINK_PLAN_NO_MEMORY;
	}
	for (index = 0; index < loader->node_count; index++) {
		const struct kobox_link_plan_node *definition =
			&config->plan->nodes[index];

		if (validate_node(config->plan, index)) {
			set_error(config->error, KOBOX_LINK_PLAN_PHASE_VALIDATE,
				  index, definition->name, NULL);
			unload_nodes(loader);
			free(loader->nodes);
			free(loader);
			return KOBOX_LINK_PLAN_MALFORMED;
		}
		loader->nodes[index].definition = definition;
		if (definition->export_count) {
			loader->nodes[index].exports = calloc(
				definition->export_count,
				sizeof(loader->nodes[index].exports[0]));
			if (!loader->nodes[index].exports)
				goto no_memory;
		}
		{
			size_t symbol;

			for (symbol = 0; symbol < definition->export_count;
			     symbol++) {
				loader->nodes[index].exports[symbol].name =
					definition->exports[symbol].name;
				loader->nodes[index].exports[symbol].kind =
					definition->exports[symbol].kind;
			}
		}
		if (verify_artifact(config->artifact_descriptors[index],
				    definition)) {
			set_error(config->error, KOBOX_LINK_PLAN_PHASE_VALIDATE,
				  index, definition->name, NULL);
			unload_nodes(loader);
			for (index = 0; index < loader->node_count; index++)
				free(loader->nodes[index].exports);
			free(loader->nodes);
			free(loader);
			return KOBOX_LINK_PLAN_ARTIFACT_FAILURE;
		}
	}
	for (index = 0; index < loader->node_count; index++) {
		struct loaded_node *node = &loader->nodes[index];
		enum kobox_link_plan_status status;

		set_error(config->error, KOBOX_LINK_PLAN_PHASE_LOAD, index,
			  node->definition->name, NULL);
		if (node->definition->kind == KOBOX_LINK_PLAN_SHARED_PROVIDER)
			status = load_shared(
				node, config->artifact_descriptors[index]);
		else
			status = load_module(
				loader, node, config->artifact_descriptors[index],
				index, config->error);
		if (status != KOBOX_LINK_PLAN_OK) {
			unload_nodes(loader);
			for (index = 0; index < loader->node_count; index++)
				free(loader->nodes[index].exports);
			free(loader->nodes);
			free(loader);
			return status;
		}
		node->loaded = 1;
	}
	if (config->error)
		*config->error = (struct kobox_link_plan_error){ 0 };
	*loader_out = loader;
	return KOBOX_LINK_PLAN_OK;

no_memory:
	unload_nodes(loader);
	for (index = 0; index < loader->node_count; index++)
		free(loader->nodes[index].exports);
	free(loader->nodes);
	free(loader);
	return KOBOX_LINK_PLAN_NO_MEMORY;
}

enum kobox_link_plan_status
kobox_link_plan_loader_close(struct kobox_link_plan_loader **loader_pointer)
{
	struct kobox_link_plan_loader *loader;
	size_t index;

	if (!loader_pointer || !*loader_pointer)
		return KOBOX_LINK_PLAN_INVALID_ARGUMENT;
	loader = *loader_pointer;
	if (loader->lifecycle_cursor)
		return KOBOX_LINK_PLAN_INVALID_STATE;
	unload_nodes(loader);
	for (index = 0; index < loader->node_count; index++)
		free(loader->nodes[index].exports);
	free(loader->nodes);
	free(loader);
	*loader_pointer = NULL;
	return KOBOX_LINK_PLAN_OK;
}

size_t kobox_link_plan_loader_count(
	const struct kobox_link_plan_loader *loader)
{
	return loader ? loader->node_count : 0;
}
