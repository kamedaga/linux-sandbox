// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "closure_loader.h"

#include "../fixture/fixture.h"

#include <kobox2/sha256.h>

#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
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

#define TEST_STRING(value) { (value), sizeof(value) - 1 }
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

struct artifact_object {
	int descriptor;
	uint64_t size;
	uint8_t digest[KB2_SHA256_DIGEST_SIZE];
};

struct manifest_object {
	uint8_t *bytes;
	size_t size;
	kb2_closure_manifest_t manifest;
};

static int write_all(int descriptor, const void *data, size_t size)
{
	const uint8_t *bytes = data;

	while (size) {
		ssize_t written;

		do {
			written = write(descriptor, bytes, size);
		} while (written < 0 && errno == EINTR);
		if (written <= 0)
			return -1;
		bytes += written;
		size -= written;
	}
	return 0;
}

static void store_u32(uint8_t destination[4], uint32_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
	destination[2] = (uint8_t)(value >> 16);
	destination[3] = (uint8_t)(value >> 24);
}

static int make_artifact(const char *path, const char *name,
			 struct artifact_object *artifact)
{
	struct stat status;
	void *bytes = MAP_FAILED;
	int source = -1;
	int result = -1;

	memset(artifact, 0, sizeof(*artifact));
	artifact->descriptor = -1;
	source = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (source < 0 || fstat(source, &status) || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0 || (uint64_t)status.st_size > SIZE_MAX)
		goto out;
	bytes = mmap(NULL, (size_t)status.st_size, PROT_READ, MAP_PRIVATE,
		     source, 0);
	if (bytes == MAP_FAILED)
		goto out;
	artifact->descriptor = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (artifact->descriptor < 0 ||
	    ftruncate(artifact->descriptor, status.st_size) ||
	    write_all(artifact->descriptor, bytes, (size_t)status.st_size) ||
	    lseek(artifact->descriptor, 0, SEEK_SET) != 0 ||
	    fcntl(artifact->descriptor, F_ADD_SEALS,
		  F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE))
		goto out;
	artifact->size = status.st_size;
	kb2_sha256(bytes, (size_t)status.st_size, artifact->digest);
	result = 0;

out:
	if (bytes != MAP_FAILED)
		munmap(bytes, (size_t)status.st_size);
	if (source >= 0)
		close(source);
	if (result && artifact->descriptor >= 0) {
		close(artifact->descriptor);
		artifact->descriptor = -1;
	}
	return result;
}

static int validate_shared(void *context, int descriptor,
			   const kb2_closure_manifest_artifact_t *artifact)
{
	(void)context;
	return descriptor >= 0 &&
	       artifact->kind == KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER
		       ? 0
		       : -1;
}

static int bind_resource(void *context,
			 const kb2_closure_manifest_resource_t *resource,
			 uint32_t node_id)
{
	size_t *count = context;

	if (resource->slot_id != 1 ||
	    resource->type != KB2_CLOSURE_RESOURCE_CHANNEL ||
	    (node_id != 2 && node_id != 3))
		return -1;
	(*count)++;
	return 0;
}

static int make_manifest(const struct artifact_object artifacts[3],
			 const kb2_closure_manifest_dependency_t *dependencies,
			 size_t dependency_count,
			 struct manifest_object *object)
{
	kb2_closure_manifest_artifact_t artifact_records[] = {
		{
			.node_id = 1,
			.kind = KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
			.namespace_name = TEST_STRING("fixture_core"),
			.init_symbol = TEST_STRING("kobox_fixture_core_init"),
			.quiesce_symbol = TEST_STRING("kobox_fixture_core_quiesce"),
			.cleanup_symbol = TEST_STRING("kobox_fixture_core_cleanup"),
		},
		{
			.node_id = 2,
			.kind = KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE,
			.flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT,
			.namespace_name = TEST_STRING("fixture_module_a"),
			.init_symbol = TEST_STRING("kobox_fixture_module_init"),
			.quiesce_symbol = TEST_STRING("kobox_fixture_module_quiesce"),
			.cleanup_symbol = TEST_STRING("kobox_fixture_module_cleanup"),
		},
		{
			.node_id = 3,
			.kind = KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE,
			.flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT,
			.namespace_name = TEST_STRING("fixture_module_b"),
			.init_symbol = TEST_STRING("kobox_fixture_module_init"),
			.quiesce_symbol = TEST_STRING("kobox_fixture_module_quiesce"),
			.cleanup_symbol = TEST_STRING("kobox_fixture_module_cleanup"),
		},
	};
	const kb2_closure_manifest_symbol_t exports[] = {
		{ 1, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_core_cleanup") },
		{ 1, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_core_get_ops") },
		{ 1, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_core_init") },
		{ 1, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_core_quiesce") },
		{ 2, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_module_cleanup") },
		{ 2, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_module_init") },
		{ 2, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_module_quiesce") },
		{ 2, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_module_run") },
		{ 3, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_module_cleanup") },
		{ 3, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_module_init") },
		{ 3, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_module_quiesce") },
		{ 3, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_fixture_module_run") },
	};
	const kb2_closure_manifest_import_t imports[] = {
		{ 2, 1, KB2_CLOSURE_SYMBOL_FUNCTION, 0,
		  TEST_STRING("kobox_fixture_core_get_ops"),
		  TEST_STRING("kobox_fixture_core_get_ops") },
		{ 3, 1, KB2_CLOSURE_SYMBOL_FUNCTION, 0,
		  TEST_STRING("kobox_fixture_core_get_ops"),
		  TEST_STRING("kobox_fixture_core_get_ops") },
	};
	const kb2_closure_manifest_resource_t resources[] = {
		{ 1, KB2_CLOSURE_RESOURCE_CHANNEL, 1, 1,
		  KB2_CLOSURE_CHANNEL_RIGHT_SEND |
			  KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE,
		  KB2_CLOSURE_CHANNEL_RIGHT_SEND |
			  KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE,
		  KB2_CLOSURE_RESOURCE_FLAG_REQUIRED |
			  KB2_CLOSURE_RESOURCE_FLAG_SHARED },
	};
	const kb2_closure_manifest_binding_t bindings[] = {
		{ 1, 2 },
		{ 1, 3 },
	};
	kb2_closure_manifest_source_t source = {
		.artifacts = artifact_records,
		.artifact_count = 3,
		.dependencies = dependencies,
		.dependency_count = dependency_count,
		.exports = exports,
		.export_count = ARRAY_SIZE(exports),
		.imports = imports,
		.import_count = ARRAY_SIZE(imports),
		.resources = resources,
		.resource_count = ARRAY_SIZE(resources),
		.bindings = bindings,
		.binding_count = ARRAY_SIZE(bindings),
	};
	size_t index;

	memset(object, 0, sizeof(*object));
	for (index = 0; index < 3; index++) {
		artifact_records[index].content_size = artifacts[index].size;
		memcpy(artifact_records[index].content_digest,
		       artifacts[index].digest,
		       sizeof(artifact_records[index].content_digest));
	}
	if (kb2_closure_manifest_encoded_size(&source, &object->size) !=
		    KB2_PROTOCOL_OK)
		return -1;
	object->bytes = malloc(object->size);
	if (!object->bytes ||
	    kb2_closure_manifest_encode(object->bytes, object->size,
					 &object->size, &source) != KB2_PROTOCOL_OK ||
	    kb2_closure_manifest_decode(object->bytes, object->size,
					 &object->manifest) != KB2_PROTOCOL_OK) {
		free(object->bytes);
		memset(object, 0, sizeof(*object));
		return -1;
	}
	return 0;
}

KOBOX_MANUAL_ELF_CALL static int call_module(uintptr_t address,
					     uint64_t *result_out)
{
	kobox_fixture_module_run_fn function;

	if (sizeof(address) != sizeof(function))
		return -1;
	memcpy(&function, &address, sizeof(function));
	return function(result_out);
}

static int run_valid_closure(const struct artifact_object artifacts[3],
			     const int descriptors[3])
{
	const kb2_closure_manifest_dependency_t dependencies[] = {
		{ 2, 1 },
		{ 3, 1 },
	};
	struct manifest_object manifest;
	struct kobox_closure_loader *loader = NULL;
	struct kobox_closure_loader_config config;
	uintptr_t first_run;
	uintptr_t second_run;
	uintptr_t ambiguous;
	uint64_t result;
	uint32_t root;
	size_t binding_count = 0;
	int status = -1;

	if (make_manifest(artifacts, dependencies,
			  ARRAY_SIZE(dependencies),
			  &manifest))
		return -1;
	memset(&config, 0, sizeof(config));
	config.manifest = &manifest.manifest;
	config.artifact_descriptors = descriptors;
	config.artifact_count = 3;
	config.bind_resource = bind_resource;
	config.bind_resource_context = &binding_count;
	config.validate_shared = validate_shared;
	if (kobox_closure_loader_open(&config, &loader) != KOBOX_CLOSURE_OK ||
	    binding_count != 2 ||
	    kobox_closure_loader_symbol(
		    loader, 2, "kobox_fixture_module_run",
		    sizeof("kobox_fixture_module_run") - 1,
		    KB2_CLOSURE_SYMBOL_FUNCTION, &first_run) != KOBOX_CLOSURE_OK ||
	    kobox_closure_loader_symbol(
		    loader, 3, "kobox_fixture_module_run",
		    sizeof("kobox_fixture_module_run") - 1,
		    KB2_CLOSURE_SYMBOL_FUNCTION, &second_run) != KOBOX_CLOSURE_OK ||
	    kobox_closure_loader_root_symbol(
		    loader, "kobox_fixture_module_run",
		    sizeof("kobox_fixture_module_run") - 1,
		    KB2_CLOSURE_SYMBOL_FUNCTION, &root, &ambiguous) !=
		    KOBOX_CLOSURE_SYMBOL_FAILURE ||
	    call_module(first_run, &result) || result != KOBOX_FIXTURE_RESULT ||
	    call_module(second_run, &result) || result != KOBOX_FIXTURE_RESULT ||
	    kobox_closure_loader_quiesce(loader) != KOBOX_CLOSURE_OK ||
	    kobox_closure_loader_close(&loader) != KOBOX_CLOSURE_OK)
		goto out;
	status = 0;

out:
	free(manifest.bytes);
	return status;
}

static int reject_cycle(const struct artifact_object artifacts[3],
			const int descriptors[3])
{
	const kb2_closure_manifest_dependency_t dependencies[] = {
		{ 2, 3 },
		{ 3, 2 },
	};
	struct manifest_object manifest;
	struct kobox_closure_loader *loader = NULL;
	struct kobox_closure_loader_config config;
	int result = -1;

	if (make_manifest(artifacts, dependencies,
			  ARRAY_SIZE(dependencies),
			  &manifest))
		return -1;
	memset(&config, 0, sizeof(config));
	config.manifest = &manifest.manifest;
	config.artifact_descriptors = descriptors;
	config.artifact_count = 3;
	config.bind_resource = bind_resource;
	config.validate_shared = validate_shared;
	if (kobox_closure_loader_open(&config, &loader) ==
		    KOBOX_CLOSURE_MALFORMED &&
	    !loader)
		result = 0;
	free(manifest.bytes);
	return result;
}

static int reject_missing_dependency(
	const struct artifact_object artifacts[3], const int descriptors[3])
{
	const kb2_closure_manifest_dependency_t dependencies[] = {
		{ 2, 1 },
	};
	struct manifest_object manifest;
	struct kobox_closure_loader *loader = NULL;
	struct kobox_closure_loader_config config;
	size_t binding_count = 0;
	int result = -1;

	if (make_manifest(artifacts, dependencies, ARRAY_SIZE(dependencies),
			  &manifest))
		return -1;
	memset(&config, 0, sizeof(config));
	config.manifest = &manifest.manifest;
	config.artifact_descriptors = descriptors;
	config.artifact_count = 3;
	config.bind_resource = bind_resource;
	config.bind_resource_context = &binding_count;
	config.validate_shared = validate_shared;
	if (kobox_closure_loader_open(&config, &loader) ==
		    KOBOX_CLOSURE_MALFORMED &&
	    !loader && !binding_count)
		result = 0;
	free(manifest.bytes);
	return result;
}

static int reject_digest_mismatch(
	const struct artifact_object artifacts[3], const int descriptors[3])
{
	const kb2_closure_manifest_dependency_t dependencies[] = {
		{ 2, 1 },
		{ 3, 1 },
	};
	struct artifact_object altered[3];
	struct manifest_object manifest;
	struct kobox_closure_loader *loader = NULL;
	struct kobox_closure_loader_config config;
	size_t binding_count = 0;
	int result = -1;

	memcpy(altered, artifacts, sizeof(altered));
	altered[0].digest[0] ^= 1;
	if (make_manifest(altered, dependencies, ARRAY_SIZE(dependencies),
			  &manifest))
		return -1;
	memset(&config, 0, sizeof(config));
	config.manifest = &manifest.manifest;
	config.artifact_descriptors = descriptors;
	config.artifact_count = 3;
	config.bind_resource = bind_resource;
	config.bind_resource_context = &binding_count;
	config.validate_shared = validate_shared;
	if (kobox_closure_loader_open(&config, &loader) ==
		    KOBOX_CLOSURE_ARTIFACT_FAILURE &&
	    !loader && !binding_count)
		result = 0;
	free(manifest.bytes);
	return result;
}

static int reject_unknown_binding_node(
	const struct artifact_object artifacts[3], const int descriptors[3])
{
	const kb2_closure_manifest_dependency_t dependencies[] = {
		{ 2, 1 },
		{ 3, 1 },
	};
	struct manifest_object manifest;
	struct kobox_closure_loader *loader = NULL;
	struct kobox_closure_loader_config config;
	uint8_t *binding;
	size_t binding_count = 0;
	int result = -1;

	if (make_manifest(artifacts, dependencies, ARRAY_SIZE(dependencies),
			  &manifest))
		return -1;
	binding = manifest.bytes + manifest.manifest.offsets[5];
	store_u32(binding + KB2_CLOSURE_RESOURCE_BINDING_NODE_ID_OFFSET, 99);
	memset(&config, 0, sizeof(config));
	config.manifest = &manifest.manifest;
	config.artifact_descriptors = descriptors;
	config.artifact_count = 3;
	config.bind_resource = bind_resource;
	config.bind_resource_context = &binding_count;
	config.validate_shared = validate_shared;
	if (kobox_closure_loader_open(&config, &loader) ==
		    KOBOX_CLOSURE_MALFORMED &&
	    !loader && !binding_count)
		result = 0;
	free(manifest.bytes);
	return result;
}

static int reject_noncanonical_artifact_order(
	const struct artifact_object artifacts[3], const int descriptors[3])
{
	const kb2_closure_manifest_dependency_t dependencies[] = {
		{ 2, 1 },
		{ 3, 1 },
	};
	struct manifest_object manifest;
	struct kobox_closure_loader *loader = NULL;
	struct kobox_closure_loader_config config;
	uint8_t *artifact;
	size_t binding_count = 0;
	int result = -1;

	if (make_manifest(artifacts, dependencies, ARRAY_SIZE(dependencies),
			  &manifest))
		return -1;
	artifact = manifest.bytes + manifest.manifest.offsets[0];
	store_u32(artifact + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_NODE_ID_OFFSET, 4);
	memset(&config, 0, sizeof(config));
	config.manifest = &manifest.manifest;
	config.artifact_descriptors = descriptors;
	config.artifact_count = 3;
	config.bind_resource = bind_resource;
	config.bind_resource_context = &binding_count;
	config.validate_shared = validate_shared;
	if (kobox_closure_loader_open(&config, &loader) ==
		    KOBOX_CLOSURE_MALFORMED &&
	    !loader && !binding_count)
		result = 0;
	free(manifest.bytes);
	return result;
}

static int verify_lifecycle_rollback(
	const struct artifact_object valid_artifacts[3],
	const int valid_descriptors[3],
	const struct artifact_object *failing_artifact)
{
	const kb2_closure_manifest_dependency_t dependencies[] = {
		{ 2, 1 },
		{ 3, 1 },
	};
	struct artifact_object artifacts[3] = {
		valid_artifacts[0],
		valid_artifacts[1],
		*failing_artifact,
	};
	int descriptors[3] = {
		artifacts[0].descriptor,
		artifacts[1].descriptor,
		artifacts[2].descriptor,
	};
	struct manifest_object manifest;
	struct kobox_closure_loader *loader = NULL;
	struct kobox_closure_loader_config config;
	char core_path[64];
	void *pinned_core = NULL;
	size_t binding_count = 0;
	int length;
	int result = -1;

	length = snprintf(core_path, sizeof(core_path), "/proc/self/fd/%d",
			  descriptors[0]);
	if (length <= 0 || (size_t)length >= sizeof(core_path))
		return -1;
	pinned_core = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
	if (!pinned_core ||
	    make_manifest(artifacts, dependencies, ARRAY_SIZE(dependencies),
			  &manifest))
		goto out;
	memset(&config, 0, sizeof(config));
	config.manifest = &manifest.manifest;
	config.artifact_descriptors = descriptors;
	config.artifact_count = 3;
	config.bind_resource = bind_resource;
	config.bind_resource_context = &binding_count;
	config.validate_shared = validate_shared;
	if (kobox_closure_loader_open(&config, &loader) !=
		    KOBOX_CLOSURE_LIFECYCLE_FAILURE ||
	    loader || binding_count != 2)
		goto free_manifest;
	if (run_valid_closure(valid_artifacts, valid_descriptors))
		goto free_manifest;
	result = 0;

free_manifest:
	free(manifest.bytes);
out:
	if (pinned_core)
		dlclose(pinned_core);
	return result;
}

int main(int argument_count, char **arguments)
{
	struct artifact_object artifacts[4];
	int descriptors[3];
	int result = 1;
	size_t index;

	if (argument_count != 4)
		return 2;
	for (index = 0; index < ARRAY_SIZE(artifacts); index++)
		artifacts[index].descriptor = -1;
	if (make_artifact(arguments[1], "closure-loader-core", &artifacts[0]) ||
	    make_artifact(arguments[2], "closure-loader-module-a",
			  &artifacts[1]) ||
	    make_artifact(arguments[2], "closure-loader-module-b",
			  &artifacts[2]) ||
	    make_artifact(arguments[3], "closure-loader-module-fail",
			  &artifacts[3]))
		goto out;
	for (index = 0; index < 3; index++)
		descriptors[index] = artifacts[index].descriptor;
	if (run_valid_closure(artifacts, descriptors) ||
	    reject_cycle(artifacts, descriptors) ||
	    reject_missing_dependency(artifacts, descriptors) ||
	    reject_digest_mismatch(artifacts, descriptors) ||
	    reject_unknown_binding_node(artifacts, descriptors) ||
	    reject_noncanonical_artifact_order(artifacts, descriptors) ||
	    verify_lifecycle_rollback(artifacts, descriptors, &artifacts[3]))
		goto out;
	result = 0;

out:
	for (index = 0; index < ARRAY_SIZE(artifacts); index++) {
		if (artifacts[index].descriptor >= 0)
			close(artifacts[index].descriptor);
	}
	if (result)
		fprintf(stderr, "generic closure loader test failed\n");
	return result;
}
