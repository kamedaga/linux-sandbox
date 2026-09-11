// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "memory_resource.h"

#include "../../provider/arena.h"

#include <kobox2/closure_layout.h>
#include <kobox2/memory_arena_layout.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_ARENA_SIZE (16u * 1024u * 1024u)

#define CHECK(expression)                                                     \
	do {                                                                    \
		if (!(expression)) {                                              \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,    \
				__LINE__, #expression);                              \
			return -1;                                                  \
		}                                                               \
	} while (0)

static int make_memory(size_t length, int seals)
{
	int descriptor = memfd_create("kobox-memory-resource",
				      MFD_CLOEXEC | MFD_ALLOW_SEALING);

	if (descriptor < 0 || ftruncate(descriptor, (off_t)length) ||
	    fcntl(descriptor, F_ADD_SEALS, seals)) {
		if (descriptor >= 0)
			close(descriptor);
		return -1;
	}
	return descriptor;
}

static void make_records(int descriptor, kb2_resource_grant_slot_t *slot,
			 kb2_resource_grant_object_t *object,
			 struct kobox_resource_native_handle *handle)
{
	static const uint8_t digest[KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE] =
		KB2_MEMORY_ARENA_SCHEMA_SHA256_BYTES;

	memset(slot, 0, sizeof(*slot));
	memset(object, 0, sizeof(*object));
	*slot = (kb2_resource_grant_slot_t){
		.slot_id = 1,
		.resource_type = KB2_CLOSURE_RESOURCE_MEMORY,
		.state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
		.object_count = 1,
	};
	memcpy(slot->interface_schema_digest, digest, sizeof(digest));
	*object = (kb2_resource_grant_object_t){
		.slot_id = slot->slot_id,
		.object_id = 1,
		.granted_rights = KB2_MEMORY_ARENA_REQUIRED_RIGHTS,
		.handle_count = 1,
	};
	*handle = (struct kobox_resource_native_handle){
		.role = KB2_MEMORY_ARENA_NATIVE_HANDLE_ROLE_MEMORY,
		.handle = descriptor,
	};
}

static int test_valid_mapping(void)
{
	const int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW;
	static const uint8_t identity[KB2_MEMORY_ARENA_ABI_IDENTITY_SIZE] =
		KB2_MEMORY_ARENA_ABI_IDENTITY_BYTES;
	const struct kobox_memory_arena_resource_operations *operations;
	const struct kobox_resource_interface_operations *base_operations;
	struct kobox_resource_native_handle handle;
	kb2_resource_grant_object_t object;
	kb2_resource_grant_slot_t slot;
	struct kobox_provider_arena *arena;
	void *native_object;
	void *allocation;
	void *address;
	size_t length;
	int descriptor;

	descriptor = make_memory(TEST_ARENA_SIZE, seals);
	CHECK(descriptor >= 0);
	make_records(descriptor, &slot, &object, &handle);
	CHECK(!kobox_linux_memory_resource_import(
		      NULL, &slot, &object, &handle, 1, &native_object,
		      &base_operations));
	operations = (const struct kobox_memory_arena_resource_operations *)
		base_operations;
	CHECK(operations->base.size == sizeof(*operations) &&
	      !memcmp(operations->base.identity, identity, sizeof(identity)) &&
	      operations->mapped_range &&
	      !operations->mapped_range(native_object, &address, &length) &&
	      length == TEST_ARENA_SIZE &&
	      (uintptr_t)address % KB2_MEMORY_ARENA_PAGE_SIZE == 0);
	CHECK(kobox_provider_arena_init(address, length, &arena) ==
	      KOBOX_PROVIDER_ARENA_OK);
	allocation = kobox_provider_arena_allocate(arena, 3);
	CHECK(allocation &&
	      kobox_provider_arena_release(arena, allocation, 3) ==
		      KOBOX_PROVIDER_ARENA_OK &&
	      kobox_provider_arena_destroy(&arena) == KOBOX_PROVIDER_ARENA_OK);
	kobox_linux_memory_resource_release(NULL, native_object);
	close(descriptor);
	return 0;
}

static int import_rejected(int descriptor, kb2_resource_grant_slot_t *slot,
			   kb2_resource_grant_object_t *object,
			   struct kobox_resource_native_handle *handle)
{
	const struct kobox_resource_interface_operations *operations = NULL;
	void *native_object = NULL;

	handle->handle = descriptor;
	return kobox_linux_memory_resource_import(
		       NULL, slot, object, handle, 1, &native_object, &operations) &&
	       !native_object && !operations;
}

static int test_rejections(void)
{
	const int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW;
	struct kobox_resource_native_handle handle;
	kb2_resource_grant_object_t object;
	kb2_resource_grant_slot_t slot;
	int descriptor;

	descriptor = make_memory(TEST_ARENA_SIZE, seals);
	CHECK(descriptor >= 0);
	make_records(descriptor, &slot, &object, &handle);
	slot.interface_schema_digest[0] ^= 1u;
	CHECK(import_rejected(descriptor, &slot, &object, &handle));
	make_records(descriptor, &slot, &object, &handle);
	object.granted_rights = KB2_CLOSURE_MEMORY_RIGHT_READ;
	CHECK(import_rejected(descriptor, &slot, &object, &handle));
	make_records(descriptor, &slot, &object, &handle);
	CHECK(!fcntl(descriptor, F_SETFD, 0));
	CHECK(import_rejected(descriptor, &slot, &object, &handle));
	close(descriptor);

	descriptor = make_memory(TEST_ARENA_SIZE,
				 F_SEAL_SHRINK | F_SEAL_GROW);
	CHECK(descriptor >= 0);
	make_records(descriptor, &slot, &object, &handle);
	CHECK(import_rejected(descriptor, &slot, &object, &handle));
	close(descriptor);

	descriptor = make_memory(TEST_ARENA_SIZE + 1, seals);
	CHECK(descriptor >= 0);
	make_records(descriptor, &slot, &object, &handle);
	CHECK(import_rejected(descriptor, &slot, &object, &handle));
	close(descriptor);
	return 0;
}

int main(void)
{
	return test_valid_mapping() || test_rejections() ? EXIT_FAILURE :
							 EXIT_SUCCESS;
}
