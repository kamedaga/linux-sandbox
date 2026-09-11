// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "../loader/closure_loader.h"
#include "../host/posix/memory_resource.h"
#include "core_lifecycle.h"

#include <kobox2/closure_layout.h>
#include <kobox2/memory_arena_layout.h>
#include <kobox2/sha256.h>

#include <errno.h>
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
#define CORE_NODE_ID 1u
#define ROOT_NODE_ID 2u
#define TEST_GENERATION UINT64_C(59)
#define TEST_OBJECT_ID UINT64_C(83)
#define TEST_ARENA_SIZE (16u * 1024u * 1024u)
#define TEST_RESULT UINT64_C(0x6b62326172656e61)

struct artifact {
	int descriptor;
	uint64_t size;
	uint8_t digest[KB2_SHA256_DIGEST_SIZE];
};

struct encoded_manifest {
	uint8_t *bytes;
	size_t size;
	kb2_closure_manifest_t decoded;
};

struct encoded_grant {
	uint8_t *bytes;
	size_t size;
	kb2_resource_grant_t decoded;
};

static int write_all(int descriptor, const void *bytes, size_t size)
{
	const uint8_t *cursor = bytes;

	while (size) {
		ssize_t written;

		do {
			written = write(descriptor, cursor, size);
		} while (written < 0 && errno == EINTR);
		if (written <= 0)
			return -1;
		cursor += written;
		size -= (size_t)written;
	}
	return 0;
}

static int make_artifact(const char *path, const char *name,
			 struct artifact *artifact)
{
	struct stat status;
	void *mapping = MAP_FAILED;
	int source = -1;
	int result = -1;

	memset(artifact, 0, sizeof(*artifact));
	artifact->descriptor = -1;
	source = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (source < 0 || fstat(source, &status) || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0 || (uint64_t)status.st_size > SIZE_MAX)
		goto out;
	mapping = mmap(NULL, (size_t)status.st_size, PROT_READ, MAP_PRIVATE,
		       source, 0);
	if (mapping == MAP_FAILED)
		goto out;
	artifact->descriptor = memfd_create(name,
					    MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (artifact->descriptor < 0 ||
	    ftruncate(artifact->descriptor, status.st_size) ||
	    write_all(artifact->descriptor, mapping, (size_t)status.st_size) ||
	    lseek(artifact->descriptor, 0, SEEK_SET) != 0 ||
	    fcntl(artifact->descriptor, F_ADD_SEALS,
		  F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE))
		goto out;
	artifact->size = (uint64_t)status.st_size;
	kb2_sha256(mapping, (size_t)status.st_size, artifact->digest);
	result = 0;

out:
	if (mapping != MAP_FAILED)
		munmap(mapping, (size_t)status.st_size);
	if (source >= 0)
		close(source);
	if (result && artifact->descriptor >= 0) {
		close(artifact->descriptor);
		artifact->descriptor = -1;
	}
	return result;
}

static int make_manifest(const struct artifact artifacts[2],
			 struct encoded_manifest *encoded)
{
	kb2_closure_manifest_artifact_t artifact_records[] = {
		{
			.node_id = CORE_NODE_ID,
			.kind = KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
			.namespace_name = TEST_STRING("linux_core"),
			.init_symbol = TEST_STRING("kobox_linux_core_init"),
			.quiesce_symbol = TEST_STRING("kobox_linux_core_quiesce"),
			.cleanup_symbol = TEST_STRING("kobox_linux_core_cleanup"),
		},
		{
			.node_id = ROOT_NODE_ID,
			.kind = KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE,
			.flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT,
			.namespace_name = TEST_STRING("arena_consumer"),
			.init_symbol = TEST_STRING("kobox_arena_fixture_init"),
			.quiesce_symbol = TEST_STRING("kobox_arena_fixture_quiesce"),
			.cleanup_symbol = TEST_STRING("kobox_arena_fixture_cleanup"),
		},
	};
	const kb2_closure_manifest_dependency_t dependencies[] = {
		{ ROOT_NODE_ID, CORE_NODE_ID },
	};
	const kb2_closure_manifest_symbol_t exports[] = {
		{ CORE_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_linux_core_cleanup") },
		{ CORE_NODE_ID, KB2_CLOSURE_SYMBOL_OBJECT,
		  TEST_STRING("kobox_linux_core_directory") },
		{ CORE_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_linux_core_init") },
		{ CORE_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_linux_core_quiesce") },
		{ ROOT_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_arena_fixture_cleanup") },
		{ ROOT_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_arena_fixture_init") },
		{ ROOT_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_arena_fixture_quiesce") },
		{ ROOT_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		  TEST_STRING("kobox_arena_fixture_run") },
	};
	kb2_closure_manifest_resource_t resource = {
		.slot_id = KOBOX_LINUX_CORE_MEMORY_SLOT_ID,
		.type = KB2_CLOSURE_RESOURCE_MEMORY,
		.minimum_count = 1,
		.maximum_count = 1,
		.required_rights = KB2_MEMORY_ARENA_REQUIRED_RIGHTS,
		.maximum_rights = KB2_MEMORY_ARENA_REQUIRED_RIGHTS,
		.flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED,
	};
	const kb2_closure_manifest_binding_t binding = {
		KOBOX_LINUX_CORE_MEMORY_SLOT_ID, CORE_NODE_ID
	};
	kb2_closure_manifest_source_t source;
	static const uint8_t interface_digest[
		KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE] =
			KB2_MEMORY_ARENA_SCHEMA_SHA256_BYTES;
	size_t index;

	memset(encoded, 0, sizeof(*encoded));
	for (index = 0; index < ARRAY_SIZE(artifact_records); index++) {
		artifact_records[index].content_size = artifacts[index].size;
		memcpy(artifact_records[index].content_digest,
		       artifacts[index].digest,
		       sizeof(artifact_records[index].content_digest));
	}
	memcpy(resource.interface_schema_digest, interface_digest,
	       sizeof(resource.interface_schema_digest));
	source = (kb2_closure_manifest_source_t){
		.artifacts = artifact_records,
		.artifact_count = ARRAY_SIZE(artifact_records),
		.dependencies = dependencies,
		.dependency_count = ARRAY_SIZE(dependencies),
		.exports = exports,
		.export_count = ARRAY_SIZE(exports),
		.resources = &resource,
		.resource_count = 1,
		.bindings = &binding,
		.binding_count = 1,
	};
	if (kb2_closure_manifest_encoded_size(&source, &encoded->size) !=
	    KB2_PROTOCOL_OK)
		return -1;
	encoded->bytes = malloc(encoded->size);
	if (!encoded->bytes ||
	    kb2_closure_manifest_encode(encoded->bytes, encoded->size,
				       &encoded->size, &source) != KB2_PROTOCOL_OK ||
	    kb2_closure_manifest_decode(encoded->bytes, encoded->size,
				       &encoded->decoded) != KB2_PROTOCOL_OK) {
		free(encoded->bytes);
		memset(encoded, 0, sizeof(*encoded));
		return -1;
	}
	return 0;
}

static int make_grant(const struct encoded_manifest *manifest,
		      struct encoded_grant *encoded)
{
	kb2_resource_grant_slot_source_t slot = {
		.slot_id = KOBOX_LINUX_CORE_MEMORY_SLOT_ID,
		.resource_type = KB2_CLOSURE_RESOURCE_MEMORY,
		.state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
	};
	const kb2_resource_grant_object_source_t object = {
		.slot_id = KOBOX_LINUX_CORE_MEMORY_SLOT_ID,
		.object_id = TEST_OBJECT_ID,
		.granted_rights = KB2_MEMORY_ARENA_REQUIRED_RIGHTS,
	};
	const kb2_resource_grant_handle_binding_t binding = {
		.object_id = TEST_OBJECT_ID,
		.role = KB2_MEMORY_ARENA_NATIVE_HANDLE_ROLE_MEMORY,
		.transfer_handle_index = 0,
	};
	kb2_resource_grant_source_t source = {
		.generation = TEST_GENERATION,
		.slots = &slot,
		.slot_count = 1,
		.objects = &object,
		.object_count = 1,
		.handle_bindings = &binding,
		.handle_binding_count = 1,
	};
	static const uint8_t interface_digest[
		KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE] =
			KB2_MEMORY_ARENA_SCHEMA_SHA256_BYTES;

	memset(encoded, 0, sizeof(*encoded));
	memcpy(slot.interface_schema_digest, interface_digest,
	       sizeof(slot.interface_schema_digest));
	kb2_sha256(manifest->bytes, manifest->size,
		   source.closure_manifest_digest);
	if (kb2_resource_grant_encoded_size(&source, &encoded->size) !=
	    KB2_PROTOCOL_OK)
		return -1;
	encoded->bytes = malloc(encoded->size);
	if (!encoded->bytes ||
	    kb2_resource_grant_encode(encoded->bytes, encoded->size,
				      &encoded->size, &source) != KB2_PROTOCOL_OK ||
	    kb2_resource_grant_decode(encoded->bytes, encoded->size,
				      &encoded->decoded) != KB2_PROTOCOL_OK) {
		free(encoded->bytes);
		memset(encoded, 0, sizeof(*encoded));
		return -1;
	}
	return 0;
}

static int validate_shared(void *context, int descriptor,
			   const kb2_closure_manifest_artifact_t *artifact)
{
	(void)context;
	return descriptor >= 0 && artifact && artifact->node_id == CORE_NODE_ID &&
		       artifact->kind == KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER ?
		       0 :
		       -1;
}

KOBOX_MANUAL_ELF_CALL static int call_run(uintptr_t address,
					 uint64_t *result_out)
{
	int (*function)(uint64_t *);

	if (sizeof(address) != sizeof(function))
		return -1;
	memcpy(&function, &address, sizeof(function));
	return function(result_out);
}

static int run_scenario(const char *core_path, const char *module_path,
			enum kobox_closure_loader_status close_expected)
{
	const int memory_seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW;
	struct kobox_closure_loader_config config = { 0 };
	struct kobox_closure_loader *loader = NULL;
	struct encoded_manifest manifest = { 0 };
	struct encoded_grant grant = { 0 };
	struct artifact artifacts[2];
	int artifact_descriptors[2];
	uint32_t root_node;
	uintptr_t run_address;
	uint64_t result;
	enum kobox_closure_loader_status closure_status = KOBOX_CLOSURE_OK;
	const char *stage = "artifact";
	int memory = -1;
	int status = -1;
	size_t index;

	for (index = 0; index < ARRAY_SIZE(artifacts); index++)
		artifacts[index].descriptor = -1;
	if (make_artifact(core_path, "core-arena-provider", &artifacts[0]) ||
	    make_artifact(module_path, "core-arena-module", &artifacts[1]) ||
	    make_manifest(artifacts, &manifest) || make_grant(&manifest, &grant))
		goto out;
	memory = memfd_create("core-arena-memory", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	stage = "memory";
	if (memory < 0 || ftruncate(memory, TEST_ARENA_SIZE) ||
	    fcntl(memory, F_ADD_SEALS, memory_seals))
		goto out;
	artifact_descriptors[0] = artifacts[0].descriptor;
	artifact_descriptors[1] = artifacts[1].descriptor;
	config = (struct kobox_closure_loader_config){
		.manifest = &manifest.decoded,
		.artifact_descriptors = artifact_descriptors,
		.artifact_count = ARRAY_SIZE(artifact_descriptors),
		.grant = &grant.decoded,
		.resource_handles = &memory,
		.resource_handle_count = 1,
		.import_resource = kobox_linux_memory_resource_import,
		.release_resource = kobox_linux_memory_resource_release,
		.validate_shared = validate_shared,
		.core_operations_node_id = CORE_NODE_ID,
		.core_operations_symbol = "kobox_linux_core_directory",
		.core_operations_symbol_length =
			 sizeof("kobox_linux_core_directory") - 1,
		.logical_cpu_count = 2,
	};
	stage = "open";
	closure_status = kobox_closure_loader_open(&config, &loader);
	if (closure_status != KOBOX_CLOSURE_OK)
		goto out;
	stage = "root-symbol";
	closure_status = kobox_closure_loader_root_symbol(
		loader, "kobox_arena_fixture_run",
		sizeof("kobox_arena_fixture_run") - 1,
		KB2_CLOSURE_SYMBOL_FUNCTION, &root_node, &run_address);
	if (closure_status != KOBOX_CLOSURE_OK || root_node != ROOT_NODE_ID)
		goto out;
	stage = "run";
	if (call_run(run_address, &result) || result != TEST_RESULT)
		goto out;
	stage = "quiesce";
	closure_status = kobox_closure_loader_quiesce(loader);
	if (closure_status != KOBOX_CLOSURE_OK)
		goto out;
	stage = "close";
	closure_status = kobox_closure_loader_close(&loader);
	if (closure_status != close_expected)
		goto out;
	status = 0;

out:
	if (status)
		fprintf(stderr, "core arena stage failed: %s (%d)\n", stage,
			closure_status);
	if (loader && kobox_closure_loader_quiesce(loader) == KOBOX_CLOSURE_OK)
		(void)kobox_closure_loader_close(&loader);
	if (memory >= 0)
		close(memory);
	for (index = 0; index < ARRAY_SIZE(artifacts); index++) {
		if (artifacts[index].descriptor >= 0)
			close(artifacts[index].descriptor);
	}
	free(grant.bytes);
	free(manifest.bytes);
	return status;
}

int main(int argument_count, char **arguments)
{
	if (argument_count != 4)
		return 2;
	if (run_scenario(arguments[1], arguments[2], KOBOX_CLOSURE_OK)) {
		fprintf(stderr, "core arena closure scenario failed\n");
		return 1;
	}
	if (run_scenario(arguments[1], arguments[3],
			 KOBOX_CLOSURE_LIFECYCLE_FAILURE)) {
		fprintf(stderr, "core arena leak gate scenario failed\n");
		return 1;
	}
	return 0;
}
