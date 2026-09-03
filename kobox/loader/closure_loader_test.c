// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "closure_loader.h"

#include "../fixture/fixture.h"

#include <kobox2/sha256.h>

#include <dlfcn.h>
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

struct grant_object {
	uint8_t *bytes;
	size_t size;
	kb2_resource_grant_t grant;
};

struct import_tracker {
	size_t imports;
	size_t releases;
	int fail;
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
		size -= (size_t)written;
	}
	return 0;
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
	artifact->size = (uint64_t)status.st_size;
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

static const struct kobox_resource_interface_operations
	test_resource_operations = {
		.size = sizeof(test_resource_operations),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
	};

static int import_resource(
	void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles, size_t handle_count,
	void **native_object_out,
	const struct kobox_resource_interface_operations **operations_out)
{
	struct import_tracker *tracker = context;
	int *native_object;

	if (!tracker || !slot || !object || !handles || !native_object_out ||
	    !operations_out ||
	    tracker->fail || slot->slot_id != KOBOX_FIXTURE_RESOURCE_SLOT_ID ||
	    slot->resource_type != KB2_CLOSURE_RESOURCE_CHANNEL ||
	    object->slot_id != slot->slot_id || handle_count != 1 ||
	    handles[0].handle < 0)
		return -1;
	native_object = malloc(sizeof(*native_object));
	if (!native_object)
		return -1;
	*native_object = fcntl(handles[0].handle, F_DUPFD_CLOEXEC, 0);
	if (*native_object < 0) {
		free(native_object);
		return -1;
	}
	tracker->imports++;
	*native_object_out = native_object;
	*operations_out = &test_resource_operations;
	return 0;
}

static void release_resource(void *context, void *opaque_object)
{
	struct import_tracker *tracker = context;
	int *native_object = opaque_object;

	if (!tracker || !native_object)
		return;
	close(*native_object);
	free(native_object);
	tracker->releases++;
}

static int make_manifest(const struct artifact_object artifacts[3],
			 int include_complete_export_set,
			 struct manifest_object *object)
{
	kb2_closure_manifest_artifact_t artifact_records[] = {
		{
			.node_id = KOBOX_FIXTURE_CORE_NODE_ID,
			.kind = KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
			.namespace_name = TEST_STRING("fixture_core"),
			.init_symbol = TEST_STRING("kobox_fixture_core_init"),
			.quiesce_symbol =
				TEST_STRING("kobox_fixture_core_quiesce"),
			.cleanup_symbol =
				TEST_STRING("kobox_fixture_core_cleanup"),
		},
		{
			.node_id = KOBOX_FIXTURE_PROVIDER_NODE_ID,
			.kind = KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE,
			.namespace_name = TEST_STRING("fixture_provider"),
			.init_symbol = TEST_STRING("kobox_fixture_provider_init"),
			.quiesce_symbol =
				TEST_STRING("kobox_fixture_provider_quiesce"),
			.cleanup_symbol =
				TEST_STRING("kobox_fixture_provider_cleanup"),
		},
		{
			.node_id = KOBOX_FIXTURE_CONSUMER_NODE_ID,
			.kind = KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE,
			.flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT,
			.namespace_name = TEST_STRING("fixture_consumer"),
			.init_symbol = TEST_STRING("kobox_fixture_consumer_init"),
			.quiesce_symbol =
				TEST_STRING("kobox_fixture_consumer_quiesce"),
			.cleanup_symbol =
				TEST_STRING("kobox_fixture_consumer_cleanup"),
		},
	};
	const kb2_closure_manifest_dependency_t dependencies[] = {
		{ KOBOX_FIXTURE_PROVIDER_NODE_ID, KOBOX_FIXTURE_CORE_NODE_ID },
		{ KOBOX_FIXTURE_CONSUMER_NODE_ID,
		  KOBOX_FIXTURE_PROVIDER_NODE_ID },
	};
	kb2_closure_manifest_symbol_t exports[14];
	size_t export_count = 0;
	const kb2_closure_manifest_import_t imports[] = {
		{
			KOBOX_FIXTURE_CONSUMER_NODE_ID,
			KOBOX_FIXTURE_PROVIDER_NODE_ID,
			KB2_CLOSURE_SYMBOL_FUNCTION,
			0,
			TEST_STRING("kobox_fixture_provider_add"),
			TEST_STRING("kobox_fixture_provider_add"),
		},
	};
	kb2_closure_manifest_resource_t resources[] = {
		{
			.slot_id = KOBOX_FIXTURE_RESOURCE_SLOT_ID,
			.type = KB2_CLOSURE_RESOURCE_CHANNEL,
			.minimum_count = 1,
			.maximum_count = 1,
			.required_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND |
					   KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE,
			.maximum_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND |
					  KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE,
			.flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED,
		},
	};
	const kb2_closure_manifest_binding_t bindings[] = {
		{ KOBOX_FIXTURE_RESOURCE_SLOT_ID,
		  KOBOX_FIXTURE_CONSUMER_NODE_ID },
	};
	kb2_closure_manifest_source_t source;
	size_t index;

#define ADD_EXPORT(node, symbol_kind, value)                                    \
	exports[export_count++] = (kb2_closure_manifest_symbol_t){                \
		(node), (symbol_kind), TEST_STRING(value)                           \
	}
	ADD_EXPORT(KOBOX_FIXTURE_CORE_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_core_cleanup");
	ADD_EXPORT(KOBOX_FIXTURE_CORE_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_core_init");
	ADD_EXPORT(KOBOX_FIXTURE_CORE_NODE_ID, KB2_CLOSURE_SYMBOL_OBJECT,
		   "kobox_fixture_core_operations");
	ADD_EXPORT(KOBOX_FIXTURE_CORE_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_core_quiesce");
	if (include_complete_export_set)
		ADD_EXPORT(KOBOX_FIXTURE_CORE_NODE_ID,
			   KB2_CLOSURE_SYMBOL_FUNCTION,
			   "kobox_fixture_lifecycle_snapshot");
	ADD_EXPORT(KOBOX_FIXTURE_PROVIDER_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_provider_add");
	ADD_EXPORT(KOBOX_FIXTURE_PROVIDER_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_provider_cleanup");
	ADD_EXPORT(KOBOX_FIXTURE_PROVIDER_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_provider_init");
	ADD_EXPORT(KOBOX_FIXTURE_PROVIDER_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_provider_quiesce");
	ADD_EXPORT(KOBOX_FIXTURE_CONSUMER_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_consumer_cleanup");
	ADD_EXPORT(KOBOX_FIXTURE_CONSUMER_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_consumer_init");
	ADD_EXPORT(KOBOX_FIXTURE_CONSUMER_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_consumer_quiesce");
	ADD_EXPORT(KOBOX_FIXTURE_CONSUMER_NODE_ID, KB2_CLOSURE_SYMBOL_FUNCTION,
		   "kobox_fixture_consumer_run");
#undef ADD_EXPORT

	memset(object, 0, sizeof(*object));
	for (index = 0; index < ARRAY_SIZE(artifact_records); index++) {
		artifact_records[index].content_size = artifacts[index].size;
		memcpy(artifact_records[index].content_digest,
		       artifacts[index].digest,
		       sizeof(artifact_records[index].content_digest));
	}
	if (kb2_protocol_copy_schema_digest(
		    resources[0].interface_schema_digest,
		    sizeof(resources[0].interface_schema_digest)) != KB2_PROTOCOL_OK)
		return -1;
	source = (kb2_closure_manifest_source_t){
		.artifacts = artifact_records,
		.artifact_count = ARRAY_SIZE(artifact_records),
		.dependencies = dependencies,
		.dependency_count = ARRAY_SIZE(dependencies),
		.exports = exports,
		.export_count = export_count,
		.imports = imports,
		.import_count = ARRAY_SIZE(imports),
		.resources = resources,
		.resource_count = ARRAY_SIZE(resources),
		.bindings = bindings,
		.binding_count = ARRAY_SIZE(bindings),
	};
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

static int make_grant(const struct manifest_object *manifest,
		      uint64_t generation, struct grant_object *object)
{
	kb2_resource_grant_slot_source_t slot = {
		.slot_id = KOBOX_FIXTURE_RESOURCE_SLOT_ID,
		.resource_type = KB2_CLOSURE_RESOURCE_CHANNEL,
		.state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
	};
	kb2_resource_grant_object_source_t resource = {
		.slot_id = KOBOX_FIXTURE_RESOURCE_SLOT_ID,
		.object_id = (generation << 32) | 1,
		.granted_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND |
				  KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE,
	};
	kb2_resource_grant_handle_binding_t binding = {
		.object_id = resource.object_id,
		.role = 1,
		.transfer_handle_index = 0,
	};
	kb2_resource_grant_source_t source = {
		.generation = generation,
		.slots = &slot,
		.slot_count = 1,
		.objects = &resource,
		.object_count = 1,
		.handle_bindings = &binding,
		.handle_binding_count = 1,
	};

	memset(object, 0, sizeof(*object));
	kb2_sha256(manifest->bytes, manifest->size,
		   source.closure_manifest_digest);
	if (kb2_protocol_copy_schema_digest(slot.interface_schema_digest,
					    sizeof(slot.interface_schema_digest)) !=
		    KB2_PROTOCOL_OK ||
	    kb2_resource_grant_encoded_size(&source, &object->size) !=
		    KB2_PROTOCOL_OK)
		return -1;
	object->bytes = malloc(object->size);
	if (!object->bytes ||
	    kb2_resource_grant_encode(object->bytes, object->size,
				      &object->size, &source) != KB2_PROTOCOL_OK ||
	    kb2_resource_grant_decode(object->bytes, object->size,
				      &object->grant) != KB2_PROTOCOL_OK) {
		free(object->bytes);
		memset(object, 0, sizeof(*object));
		return -1;
	}
	return 0;
}

static void configure_loader(struct kobox_closure_loader_config *config,
			     const struct manifest_object *manifest,
			     const struct grant_object *grant,
			     const int descriptors[3],
			     const int *resource_descriptor,
			     struct import_tracker *tracker)
{
	memset(config, 0, sizeof(*config));
	config->manifest = &manifest->manifest;
	config->artifact_descriptors = descriptors;
	config->artifact_count = 3;
	config->grant = &grant->grant;
	config->resource_handles = resource_descriptor;
	config->resource_handle_count = 1;
	config->import_resource = import_resource;
	config->release_resource = release_resource;
	config->resource_context = tracker;
	config->validate_shared = validate_shared;
	config->core_operations_node_id = KOBOX_FIXTURE_CORE_NODE_ID;
	config->core_operations_symbol = "kobox_fixture_core_operations";
	config->core_operations_symbol_length =
		sizeof("kobox_fixture_core_operations") - 1;
	config->logical_cpu_count = KOBOX_FIXTURE_CPU_COUNT;
}

KOBOX_MANUAL_ELF_CALL static int call_consumer(uintptr_t address,
					       uint64_t *result_out)
{
	kobox_fixture_module_run_fn function;

	if (sizeof(address) != sizeof(function))
		return -1;
	memcpy(&function, &address, sizeof(function));
	return function(result_out);
}

static int lookup_lifecycle_snapshot(
	void *core_handle, kobox_fixture_lifecycle_snapshot_fn *snapshot_out)
{
	kobox_fixture_lifecycle_snapshot_fn snapshot;
	void *symbol;

	dlerror();
	symbol = dlsym(core_handle, "kobox_fixture_lifecycle_snapshot");
	if (!symbol || sizeof(symbol) != sizeof(snapshot))
		return -1;
	memcpy(&snapshot, &symbol, sizeof(snapshot));
	*snapshot_out = snapshot;
	return 0;
}

static int lifecycle_suffix_matches(void *core_handle, size_t baseline,
				    const uint32_t *expected,
				    size_t expected_count)
{
	kobox_fixture_lifecycle_snapshot_fn snapshot;
	uint32_t records[32];
	size_t count;

	if (lookup_lifecycle_snapshot(core_handle, &snapshot) ||
	    snapshot(records, ARRAY_SIZE(records), &count) ||
	    count != baseline + expected_count ||
	    memcmp(records + baseline, expected,
		   expected_count * sizeof(expected[0])))
		return 0;
	return 1;
}

static int lifecycle_count(void *core_handle, size_t *count_out)
{
	kobox_fixture_lifecycle_snapshot_fn snapshot;
	uint32_t records[32];

	return !lookup_lifecycle_snapshot(core_handle, &snapshot) &&
		       !snapshot(records, ARRAY_SIZE(records), count_out)
		       ? 0
		       : -1;
}

static int run_valid_closure(const struct artifact_object artifacts[3],
			     const int descriptors[3], int resource_descriptor,
			     void *core_handle)
{
	static const uint32_t expected_lifecycle[] = {
		(KOBOX_FIXTURE_CORE_NODE_ID << 8) | KOBOX_FIXTURE_LIFECYCLE_INIT,
		(KOBOX_FIXTURE_PROVIDER_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_INIT,
		(KOBOX_FIXTURE_CONSUMER_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_INIT,
		(KOBOX_FIXTURE_CONSUMER_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_QUIESCE,
		(KOBOX_FIXTURE_PROVIDER_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_QUIESCE,
		(KOBOX_FIXTURE_CORE_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_QUIESCE,
		(KOBOX_FIXTURE_CONSUMER_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_CLEANUP,
		(KOBOX_FIXTURE_PROVIDER_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_CLEANUP,
		(KOBOX_FIXTURE_CORE_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_CLEANUP,
	};
	struct manifest_object manifest;
	struct grant_object grant;
	struct import_tracker tracker = { 0 };
	struct kobox_closure_loader_config config;
	struct kobox_closure_loader *loader = NULL;
	uintptr_t run_address;
	uintptr_t provider_address;
	uint64_t result;
	uint32_t root_node;
	size_t baseline;
	int status = -1;

	if (lifecycle_count(core_handle, &baseline) ||
	    make_manifest(artifacts, 1, &manifest) ||
	    make_grant(&manifest, 7, &grant))
		return -1;
	configure_loader(&config, &manifest, &grant, descriptors,
			 &resource_descriptor, &tracker);
	if (kobox_closure_loader_open(&config, &loader) != KOBOX_CLOSURE_OK ||
	    tracker.imports != 1 || tracker.releases != 0 ||
	    kobox_closure_loader_symbol(
		    loader, KOBOX_FIXTURE_PROVIDER_NODE_ID,
		    "kobox_fixture_provider_add",
		    sizeof("kobox_fixture_provider_add") - 1,
		    KB2_CLOSURE_SYMBOL_FUNCTION, &provider_address) !=
		    KOBOX_CLOSURE_OK ||
	    !provider_address ||
	    kobox_closure_loader_root_symbol(
		    loader, "kobox_fixture_consumer_run",
		    sizeof("kobox_fixture_consumer_run") - 1,
		    KB2_CLOSURE_SYMBOL_FUNCTION, &root_node,
		    &run_address) != KOBOX_CLOSURE_OK ||
	    root_node != KOBOX_FIXTURE_CONSUMER_NODE_ID ||
	    call_consumer(run_address, &result) ||
	    result != KOBOX_FIXTURE_RESULT ||
	    kobox_closure_loader_quiesce(loader) != KOBOX_CLOSURE_OK ||
	    kobox_closure_loader_close(&loader) != KOBOX_CLOSURE_OK ||
	    tracker.releases != 1 ||
	    !lifecycle_suffix_matches(core_handle, baseline, expected_lifecycle,
				      ARRAY_SIZE(expected_lifecycle)))
		goto out;
	status = 0;

out:
	if (loader &&
	    kobox_closure_loader_quiesce(loader) == KOBOX_CLOSURE_OK)
		kobox_closure_loader_close(&loader);
	free(grant.bytes);
	free(manifest.bytes);
	return status;
}

static int verify_init_rollback(const struct artifact_object artifacts[3],
				const struct artifact_object *failing_consumer,
				int resource_descriptor, void *core_handle)
{
	static const uint32_t expected_lifecycle[] = {
		(KOBOX_FIXTURE_CORE_NODE_ID << 8) | KOBOX_FIXTURE_LIFECYCLE_INIT,
		(KOBOX_FIXTURE_PROVIDER_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_INIT,
		(KOBOX_FIXTURE_PROVIDER_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_CLEANUP,
		(KOBOX_FIXTURE_CORE_NODE_ID << 8) |
			KOBOX_FIXTURE_LIFECYCLE_CLEANUP,
	};
	struct artifact_object failing_artifacts[3] = {
		artifacts[0], artifacts[1], *failing_consumer
	};
	int descriptors[3] = {
		artifacts[0].descriptor,
		artifacts[1].descriptor,
		failing_consumer->descriptor,
	};
	struct manifest_object manifest;
	struct grant_object grant;
	struct import_tracker tracker = { 0 };
	struct kobox_closure_loader_config config;
	struct kobox_closure_loader *loader = NULL;
	size_t baseline;
	enum kobox_closure_loader_status open_status;
	int result = -1;

	if (lifecycle_count(core_handle, &baseline) ||
	    make_manifest(failing_artifacts, 1, &manifest) ||
	    make_grant(&manifest, 8, &grant))
		return -1;
	configure_loader(&config, &manifest, &grant, descriptors,
			 &resource_descriptor, &tracker);
	open_status = kobox_closure_loader_open(&config, &loader);
	if (open_status ==
		    KOBOX_CLOSURE_LIFECYCLE_FAILURE &&
	    !loader && tracker.imports == 1 && tracker.releases == 1 &&
	    lifecycle_suffix_matches(core_handle, baseline, expected_lifecycle,
				     ARRAY_SIZE(expected_lifecycle)))
		result = 0;
	if (result)
		fprintf(stderr,
			"rollback status=%u loader=%p imports=%zu releases=%zu baseline=%zu\n",
			(unsigned int)open_status, (void *)loader, tracker.imports,
			tracker.releases, baseline);
	free(grant.bytes);
	free(manifest.bytes);
	return result;
}

static int reject_incomplete_export_set(
	const struct artifact_object artifacts[3], const int descriptors[3],
	int resource_descriptor)
{
	struct manifest_object manifest;
	struct grant_object grant;
	struct import_tracker tracker = { 0 };
	struct kobox_closure_loader_config config;
	struct kobox_closure_loader *loader = NULL;
	int result = -1;

	if (make_manifest(artifacts, 0, &manifest) ||
	    make_grant(&manifest, 9, &grant))
		return -1;
	configure_loader(&config, &manifest, &grant, descriptors,
			 &resource_descriptor, &tracker);
	if (kobox_closure_loader_open(&config, &loader) ==
		    KOBOX_CLOSURE_SYMBOL_FAILURE &&
	    !loader && !tracker.imports && !tracker.releases)
		result = 0;
	free(grant.bytes);
	free(manifest.bytes);
	return result;
}

static int reject_resource_import(
	const struct artifact_object artifacts[3], const int descriptors[3],
	int resource_descriptor)
{
	struct manifest_object manifest;
	struct grant_object grant;
	struct import_tracker tracker = { .fail = 1 };
	struct kobox_closure_loader_config config;
	struct kobox_closure_loader *loader = NULL;
	int result = -1;

	if (make_manifest(artifacts, 1, &manifest) ||
	    make_grant(&manifest, 10, &grant))
		return -1;
	configure_loader(&config, &manifest, &grant, descriptors,
			 &resource_descriptor, &tracker);
	if (kobox_closure_loader_open(&config, &loader) ==
		    KOBOX_CLOSURE_RESOURCE_FAILURE &&
	    !loader && !tracker.imports && !tracker.releases)
		result = 0;
	free(grant.bytes);
	free(manifest.bytes);
	return result;
}

int main(int argument_count, char **arguments)
{
	struct artifact_object artifacts[4];
	int descriptors[3];
	char core_path[64];
	void *core_handle = NULL;
	int resource_descriptor = -1;
	int result = 1;
	size_t index;

	if (argument_count != 5)
		return 2;
	for (index = 0; index < ARRAY_SIZE(artifacts); index++)
		artifacts[index].descriptor = -1;
	if (make_artifact(arguments[1], "closure-loader-core", &artifacts[0]) ||
	    make_artifact(arguments[2], "closure-loader-provider",
			  &artifacts[1]) ||
	    make_artifact(arguments[3], "closure-loader-consumer",
			  &artifacts[2]) ||
	    make_artifact(arguments[4], "closure-loader-consumer-fail",
			  &artifacts[3]))
		goto out;
	for (index = 0; index < ARRAY_SIZE(descriptors); index++)
		descriptors[index] = artifacts[index].descriptor;
	resource_descriptor = memfd_create("closure-loader-resource", MFD_CLOEXEC);
	if (resource_descriptor < 0 ||
	    snprintf(core_path, sizeof(core_path), "/proc/self/fd/%d",
		     artifacts[0].descriptor) <= 0)
		goto out;
	core_handle = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
	if (!core_handle) {
		fprintf(stderr, "failed to pin fixture core: %s\n", dlerror());
		goto out;
	}
	if (run_valid_closure(artifacts, descriptors, resource_descriptor,
			      core_handle)) {
		fprintf(stderr, "valid closure scenario failed\n");
		goto out;
	}
	if (verify_init_rollback(artifacts, &artifacts[3], resource_descriptor,
				 core_handle)) {
		fprintf(stderr, "init rollback scenario failed\n");
		goto out;
	}
	if (reject_incomplete_export_set(artifacts, descriptors,
					 resource_descriptor)) {
		fprintf(stderr, "export closure scenario failed\n");
		goto out;
	}
	if (reject_resource_import(artifacts, descriptors, resource_descriptor)) {
		fprintf(stderr, "resource import scenario failed\n");
		goto out;
	}
	result = 0;

out:
	if (core_handle)
		dlclose(core_handle);
	if (resource_descriptor >= 0)
		close(resource_descriptor);
	for (index = 0; index < ARRAY_SIZE(artifacts); index++) {
		if (artifacts[index].descriptor >= 0)
			close(artifacts[index].descriptor);
	}
	if (result)
		fprintf(stderr, "generic closure loader test failed\n");
	return result;
}
