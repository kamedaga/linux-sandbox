// SPDX-License-Identifier: GPL-2.0-only

#include "../host/posix/resource.h"
#include "../boot/resource_registry.h"

#include <kobox2/sha256.h>

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static size_t allocation_count, fail_at;

void *__real_calloc(size_t count, size_t size);

/* Link-time injection only in this host-registry unit test. */
void *__wrap_calloc(size_t count, size_t size)
{
	allocation_count++;
	if (fail_at && allocation_count == fail_at)
		return NULL;
	return __real_calloc(count, size);
}

static void check(int condition, unsigned int line)
{
	if (!condition) {
		fprintf(stderr, "resource rollback failed at %u (allocation %zu)\n",
			line, fail_at);
		exit(1);
	}
}

#define CHECK(expression) check(!!(expression), __LINE__)

struct tracker {
	unsigned int imported, released, fail_import;
	uint64_t released_ids[2];
};

static int import_object(void *context, const kb2_resource_grant_slot_t *slot,
			 const kb2_resource_grant_object_t *object,
			 const struct kobox_resource_native_handle *handles,
			 size_t handle_count, void **object_out,
			 const struct kobox_resource_interface_operations **operations_out)
{
	static const struct kobox_resource_interface_operations operations = {
		.size = sizeof(operations),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
	};
	struct tracker *tracker = context;
	uint64_t *identity;

	CHECK(slot->slot_id == 1 && handle_count == 1 && handles[0].role == 0);
	CHECK(handles[0].handle == (int)object->object_id + 100);
	if (tracker->fail_import == tracker->imported + 1)
		return -1;
	identity = calloc(1, sizeof(*identity));
	if (!identity)
		return -1;
	*identity = object->object_id;
	*object_out = identity;
	*operations_out = &operations;
	tracker->imported++;
	return 0;
}

static void release_object(void *context, void *object)
{
	struct tracker *tracker = context;
	uint64_t *identity = object;

	CHECK(tracker->released < tracker->imported && tracker->released < 2);
	tracker->released_ids[tracker->released++] = *identity;
	free(identity);
}

int main(void)
{
	kb2_closure_manifest_artifact_t artifact = {
		.node_id = 1, .kind = KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
		.flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT | KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX,
		.namespace_name = {.data = "core", .length = 4}, .content_size = 64,
	};
	kb2_closure_manifest_resource_t resource = {
		.slot_id = 1, .type = KB2_CLOSURE_RESOURCE_CHANNEL,
		.minimum_count = 2, .maximum_count = 2,
		.required_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND,
		.maximum_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND,
		.flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED,
	};
	kb2_closure_manifest_binding_t binding = {.slot_id = 1, .node_id = 1};
	kb2_closure_manifest_source_t manifest_source = {
		.artifacts = &artifact, .artifact_count = 1,
		.resources = &resource, .resource_count = 1,
		.bindings = &binding, .binding_count = 1,
	};
	kb2_resource_grant_slot_source_t slot = {
		.slot_id = 1, .resource_type = KB2_CLOSURE_RESOURCE_CHANNEL,
		.state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
	};
	kb2_resource_grant_object_source_t objects[2] = {
		{.slot_id = 1, .object_id = 10, .granted_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND},
		{.slot_id = 1, .object_id = 11, .granted_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND},
	};
	kb2_resource_grant_handle_binding_t handles[2] = {
		{.object_id = 10, .role = 0, .transfer_handle_index = 0},
		{.object_id = 11, .role = 0, .transfer_handle_index = 1},
	};
	kb2_resource_grant_source_t grant_source = {
		.generation = 17, .slots = &slot, .slot_count = 1,
		.objects = objects, .object_count = 2,
		.handle_bindings = handles, .handle_binding_count = 2,
	};
	uint8_t manifest_bytes[1024], grant_bytes[1024];
	kb2_closure_manifest_t manifest;
	kb2_resource_grant_t grant;
	struct kobox_resource_runtime *runtime = NULL;
	struct kobox_linux_resource_port port;
	struct kobox_linux_resource_binding native;
	struct tracker tracker = {0};
	const int native_handles[2] = {110, 111};
	struct kobox_posix_resource_config config = {
		.manifest = &manifest, .grant = &grant,
		.native_handles = native_handles, .native_handle_count = 2,
		.import_object = import_object, .release_object = release_object,
		.object_context = &tracker,
	};
	size_t manifest_size, grant_size, allocations, index;

	memset(artifact.content_digest, 1, 32);
	CHECK(kb2_protocol_copy_schema_digest(resource.interface_schema_digest, 32) == KB2_PROTOCOL_OK);
	memcpy(slot.interface_schema_digest, resource.interface_schema_digest, 32);
	CHECK(kb2_closure_manifest_encode(manifest_bytes, sizeof(manifest_bytes), &manifest_size,
					 &manifest_source) == KB2_PROTOCOL_OK);
	CHECK(kb2_closure_manifest_decode(manifest_bytes, manifest_size, &manifest) == KB2_PROTOCOL_OK);
	kb2_sha256(manifest_bytes, manifest_size, grant_source.closure_manifest_digest);
	CHECK(kb2_resource_grant_encode(grant_bytes, sizeof(grant_bytes), &grant_size,
				       &grant_source) == KB2_PROTOCOL_OK);
	CHECK(kb2_resource_grant_decode(grant_bytes, grant_size, &grant) == KB2_PROTOCOL_OK);
	CHECK(kobox_posix_resource_open(&config, &runtime) == KOBOX_RESOURCE_RUNTIME_OK);
	allocations = allocation_count;
	CHECK(runtime && tracker.imported == 2);
	CHECK(!kobox_boot_resource_port(runtime, &port));
	CHECK(port.generation == 17);
	CHECK(!port.lookup(port.context, NULL, 1, 0, KB2_CLOSURE_CHANNEL_RIGHT_SEND,
			   resource.interface_schema_digest, &native));
	CHECK(native.generation == 17 && native.object_id == 10 && native.object);
	CHECK(native.rights == KB2_CLOSURE_CHANNEL_RIGHT_SEND);
	CHECK(native.type == KB2_CLOSURE_RESOURCE_CHANNEL);
	CHECK(port.lookup(port.context, "ungranted_module", 1, 0, 0,
			  resource.interface_schema_digest, &native) == -ENOENT);
	CHECK(!native.object);
	CHECK(port.lookup(port.context, NULL, 1, 0, KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE,
			  resource.interface_schema_digest, &native) == -EACCES);
	CHECK(!native.object);
	CHECK(port.lookup(port.context, NULL, 2, 0, 0,
			  resource.interface_schema_digest, &native) == -EACCES);
	CHECK(port.lookup(port.context, NULL, 1, 2, 0,
			  resource.interface_schema_digest, &native) == -ENOENT);
	resource.interface_schema_digest[0] ^= 1;
	CHECK(port.lookup(port.context, NULL, 1, 0, 0,
			  resource.interface_schema_digest, &native) == -EPROTOTYPE);
	resource.interface_schema_digest[0] ^= 1;
	CHECK(allocation_count == allocations);
	kobox_resource_runtime_close(&runtime);
	CHECK(!runtime && tracker.released == 2);
	CHECK(tracker.released_ids[0] == 11 && tracker.released_ids[1] == 10);
	for (index = 1; index <= allocations; index++) {
		tracker = (struct tracker) {0};
		allocation_count = 0;
		fail_at = index;
		CHECK(kobox_posix_resource_open(&config, &runtime) != KOBOX_RESOURCE_RUNTIME_OK);
		CHECK(!runtime && tracker.imported == tracker.released);
	}
	fail_at = 0;
	for (index = 1; index <= 2; index++) {
		tracker = (struct tracker) {.fail_import = index};
		CHECK(kobox_posix_resource_open(&config, &runtime) == KOBOX_RESOURCE_RUNTIME_IMPORT_FAILURE);
		CHECK(!runtime && tracker.imported == tracker.released && tracker.imported == index - 1);
	}
	/* A missing visibility binding must be rejected before importing. */
	manifest_source.binding_count = 0;
	CHECK(kb2_closure_manifest_encode(manifest_bytes, sizeof(manifest_bytes), &manifest_size,
					 &manifest_source) == KB2_PROTOCOL_OK);
	CHECK(kb2_closure_manifest_decode(manifest_bytes, manifest_size, &manifest) == KB2_PROTOCOL_OK);
	kb2_sha256(manifest_bytes, manifest_size, grant_source.closure_manifest_digest);
	CHECK(kb2_resource_grant_encode(grant_bytes, sizeof(grant_bytes), &grant_size,
				       &grant_source) == KB2_PROTOCOL_OK);
	CHECK(kb2_resource_grant_decode(grant_bytes, grant_size, &grant) == KB2_PROTOCOL_OK);
	tracker = (struct tracker) {0};
	CHECK(kobox_posix_resource_open(&config, &runtime) == KOBOX_RESOURCE_RUNTIME_MALFORMED);
	CHECK(!runtime && !tracker.imported && !tracker.released);
	printf("resource registry: %zu allocation failures and two import failures rolled back\n", allocations);
	return 0;
}
