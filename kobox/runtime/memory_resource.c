// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "memory_resource.h"

#include <kobox2/closure_layout.h>
#include <kobox2/memory_arena_layout.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define KOBOX_MEMORY_OBJECT_MAGIC UINT64_C(0x6b62326d656d6f72)

_Static_assert(KB2_MEMORY_ARENA_ABI_IDENTITY_SIZE ==
		       KOBOX_MODULE_INTERFACE_IDENTITY_SIZE,
	       "memory arena identity size must match the resource ABI");
_Static_assert(KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE ==
		       KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE,
	       "memory arena digest size must match the resource ABI");

struct linux_memory_object {
	uint64_t magic;
	void *address;
	size_t length;
};

static int mapped_range(void *opaque_object, void **address_out,
			size_t *length_out)
{
	struct linux_memory_object *object = opaque_object;

	if (!object || object->magic != KOBOX_MEMORY_OBJECT_MAGIC ||
	    !address_out || !length_out)
		return -1;
	*address_out = object->address;
	*length_out = object->length;
	return 0;
}

static const struct kobox_memory_arena_resource_operations memory_operations = {
	.base = {
		.size = sizeof(memory_operations),
		.identity = KB2_MEMORY_ARENA_ABI_IDENTITY_BYTES,
	},
	.mapped_range = mapped_range,
};

int kobox_linux_memory_resource_import(
	void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles,
	size_t handle_count, void **native_object_out,
	const struct kobox_resource_interface_operations **operations_out)
{
	static const uint8_t interface_digest[
		KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE] =
			KB2_MEMORY_ARENA_SCHEMA_SHA256_BYTES;
	const int required_seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW;
	struct linux_memory_object *memory_object;
	struct stat status;
	void *mapping;
	int descriptor_flags;
	int seals;

	(void)context;
	if (!native_object_out || !operations_out)
		return -1;
	*native_object_out = NULL;
	*operations_out = NULL;
	if (!slot || !object || !handles || handle_count != 1 ||
	    slot->state != KB2_RESOURCE_GRANT_SLOT_PRESENT ||
	    slot->resource_type != KB2_CLOSURE_RESOURCE_MEMORY ||
	    memcmp(slot->interface_schema_digest, interface_digest,
		   sizeof(interface_digest)) ||
	    object->slot_id != slot->slot_id ||
	    object->granted_rights != KB2_MEMORY_ARENA_REQUIRED_RIGHTS ||
	    object->handle_count != 1 ||
	    handles[0].role != KB2_MEMORY_ARENA_NATIVE_HANDLE_ROLE_MEMORY ||
	    handles[0].handle < 0)
		return -1;
	descriptor_flags = fcntl(handles[0].handle, F_GETFD);
	seals = fcntl(handles[0].handle, F_GET_SEALS);
	if (descriptor_flags < 0 || !(descriptor_flags & FD_CLOEXEC) ||
	    seals != required_seals || fstat(handles[0].handle, &status) ||
	    !S_ISREG(status.st_mode) || status.st_size < 0 ||
	    (uint64_t)status.st_size > SIZE_MAX ||
	    (uint64_t)status.st_size < KB2_MEMORY_ARENA_MINIMUM_LENGTH ||
	    (uint64_t)status.st_size % KB2_MEMORY_ARENA_PAGE_SIZE)
		return -1;
	mapping = mmap(NULL, (size_t)status.st_size, PROT_READ | PROT_WRITE,
		       MAP_SHARED, handles[0].handle, 0);
	if (mapping == MAP_FAILED)
		return -1;
	if ((uintptr_t)mapping % KB2_MEMORY_ARENA_PAGE_SIZE ||
	    madvise(mapping, (size_t)status.st_size, MADV_DONTDUMP)) {
		munmap(mapping, (size_t)status.st_size);
		return -1;
	}
	memory_object = calloc(1, sizeof(*memory_object));
	if (!memory_object) {
		munmap(mapping, (size_t)status.st_size);
		return -1;
	}
	memory_object->magic = KOBOX_MEMORY_OBJECT_MAGIC;
	memory_object->address = mapping;
	memory_object->length = (size_t)status.st_size;
	*native_object_out = memory_object;
	*operations_out = &memory_operations.base;
	return 0;
}

void kobox_linux_memory_resource_release(void *context, void *native_object)
{
	struct linux_memory_object *object = native_object;

	(void)context;
	if (!object || object->magic != KOBOX_MEMORY_OBJECT_MAGIC)
		return;
	object->magic = 0;
	munmap(object->address, object->length);
	free(object);
}
