// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../host/posix/package.h"

#include <kobox2/sha256.h>

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void require(int condition, unsigned int line)
{
	if (!condition) {
		fprintf(stderr, "boot package test failed at line %u\n", line);
		exit(1);
	}
}

#define CHECK(condition) require(!!(condition), __LINE__)

static int allocation_failure_after = -1;
static int mapping_failure_after = -1;
static unsigned int live_mappings;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_mmap(void *address, size_t size, int protection, int flags,
		  int descriptor, off_t offset);
int __real_munmap(void *address, size_t size);

static int fail_allocation(void)
{
	if (allocation_failure_after < 0)
		return 0;
	if (!allocation_failure_after) {
		allocation_failure_after = -1;
		return 1;
	}
	allocation_failure_after--;
	return 0;
}

void *__wrap_malloc(size_t size)
{
	return fail_allocation() ? NULL : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
	return fail_allocation() ? NULL : __real_calloc(count, size);
}

void *__wrap_mmap(void *address, size_t size, int protection, int flags,
		  int descriptor, off_t offset)
{
	void *result;

	if (!mapping_failure_after) {
		mapping_failure_after = -1;
		errno = ENOMEM;
		return MAP_FAILED;
	}
	if (mapping_failure_after > 0)
		mapping_failure_after--;
	result = __real_mmap(address, size, protection, flags, descriptor, offset);
	if (result != MAP_FAILED)
		live_mappings++;
	return result;
}

int __wrap_munmap(void *address, size_t size)
{
	int result = __real_munmap(address, size);

	if (!result) {
		CHECK(live_mappings);
		live_mappings--;
	}
	return result;
}

static int blob(const void *data, size_t size, int sealed)
{
	int descriptor = memfd_create("boot-package-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	CHECK(descriptor >= 0);
	CHECK(write(descriptor, data, size) == (ssize_t)size);
	if (sealed)
		CHECK(!fcntl(descriptor, F_ADD_SEALS,
			     F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK));
	return descriptor;
}

static unsigned int fd_count(void)
{
	DIR *directory = opendir("/proc/self/fd");
	struct dirent *entry;
	unsigned int count = 0;

	CHECK(directory);
	while ((entry = readdir(directory)))
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
			count++;
	closedir(directory);
	return count;
}

struct fixture {
	struct kobox_posix_package_input input;
	kb2_closure_manifest_artifact_t artifacts[2];
	kb2_closure_manifest_resource_t resource;
	kb2_resource_grant_slot_source_t slot;
	kb2_resource_grant_object_source_t object;
	int descriptors[2];
	Elf64_Ehdr headers[2];
};

static void metadata(struct fixture *fixture, uint64_t generation)
{
	kb2_closure_manifest_dependency_t dependency = {
		.consumer_node_id = 2, .provider_node_id = 1,
	};
	kb2_closure_manifest_source_t manifest = {
		.artifacts = fixture->artifacts, .artifact_count = 2,
		.dependencies = &dependency, .dependency_count = 1,
	};
	kb2_resource_grant_source_t grant = { .generation = generation };
	kb2_closure_manifest_binding_t binding = {.slot_id = 1, .node_id = 1};
	uint8_t *bytes;
	size_t size, written;

	if (fixture->resource.slot_id) {
		manifest.resources = &fixture->resource;
		manifest.resource_count = 1;
		manifest.bindings = &binding;
		manifest.binding_count = 1;
		grant.slots = &fixture->slot;
		grant.slot_count = 1;
		grant.objects = &fixture->object;
		grant.object_count = 1;
	}
	CHECK(kb2_closure_manifest_encoded_size(&manifest, &size) == KB2_PROTOCOL_OK);
	bytes = malloc(size);
	CHECK(bytes);
	CHECK(kb2_closure_manifest_encode(bytes, size, &written, &manifest) == KB2_PROTOCOL_OK);
	CHECK(written == size);
	kb2_sha256(bytes, size, fixture->input.expected_manifest_digest);
	memcpy(grant.closure_manifest_digest, fixture->input.expected_manifest_digest, 32);
	fixture->input.manifest_descriptor = blob(bytes, size, 1);
	free(bytes);
	CHECK(kb2_resource_grant_encoded_size(&grant, &size) == KB2_PROTOCOL_OK);
	bytes = malloc(size);
	CHECK(bytes);
	CHECK(kb2_resource_grant_encode(bytes, size, &written, &grant) == KB2_PROTOCOL_OK);
	kb2_sha256(bytes, size, fixture->input.expected_grant_digest);
	fixture->input.grant_descriptor = blob(bytes, size, 1);
	free(bytes);
}

static void setup(struct fixture *fixture)
{
	unsigned int index;

	memset(fixture, 0, sizeof(*fixture));
	fixture->input.expected_generation = 17;
	fixture->input.artifact_descriptors = fixture->descriptors;
	fixture->input.artifact_count = 2;
	for (index = 0; index < 2; index++) {
		Elf64_Ehdr *header = &fixture->headers[index];
		kb2_closure_manifest_artifact_t *artifact = &fixture->artifacts[index];

		memcpy(header->e_ident, ELFMAG, SELFMAG);
		header->e_ident[EI_CLASS] = ELFCLASS64;
		header->e_ident[EI_DATA] = ELFDATA2LSB;
		header->e_ident[EI_VERSION] = EV_CURRENT;
		header->e_version = EV_CURRENT;
		header->e_type = index ? ET_REL : ET_DYN;
		header->e_machine = EM_X86_64;
		header->e_ehsize = sizeof(*header);
		artifact->node_id = index + 1;
		artifact->flags = KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX |
			(index ? KB2_CLOSURE_ARTIFACT_FLAG_ROOT : 0);
		artifact->kind = index ? KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE :
			KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER;
		artifact->namespace_name = (kb2_closure_string_t) {
			.data = index ? "module" : "core", .length = index ? 6 : 4,
		};
		artifact->content_size = sizeof(*header);
		kb2_sha256(header, sizeof(*header), artifact->content_digest);
		fixture->descriptors[index] = blob(header, sizeof(*header), 1);
	}
	metadata(fixture, 17);
}

static void finish(struct fixture *fixture)
{
	close(fixture->input.manifest_descriptor);
	close(fixture->input.grant_descriptor);
	close(fixture->descriptors[0]);
	close(fixture->descriptors[1]);
}

static void expect_failure(struct fixture *fixture, int expected)
{
	struct kobox_boot_package *package = NULL;
	unsigned int before = fd_count();

	CHECK(kobox_posix_package_open(&fixture->input, &package) == expected);
	CHECK(!package);
	CHECK(fd_count() == before);
	CHECK(!live_mappings);
}

int main(void)
{
	struct kobox_boot_package *package = NULL;
	struct fixture fixture;
	unsigned int before = fd_count(), index;
	int saved;

	setup(&fixture);
	/* Fail the package allocation and each individual blob acquisition. */
	for (index = 0; index < 5; index++) {
		allocation_failure_after = index;
		expect_failure(&fixture, -ENOMEM);
		CHECK(allocation_failure_after == -1);
	}
	for (index = 0; index < 4; index++) {
		mapping_failure_after = index;
		expect_failure(&fixture, -ENOMEM);
		CHECK(mapping_failure_after == -1);
	}
	CHECK(!kobox_posix_package_open(&fixture.input, &package));
	CHECK(package && package->grant.generation == 17 && package->artifact_count == 2);
	for (index = 0; index < 2; index++) {
		CHECK(kobox_posix_blob_descriptor(&package->artifacts[index]) != fixture.descriptors[index]);
		CHECK(fcntl(kobox_posix_blob_descriptor(&package->artifacts[index]), F_GETFD) & FD_CLOEXEC);
		CHECK(pwrite(kobox_posix_blob_descriptor(&package->artifacts[index]),
			     "x", 1, 0) == -1 && errno == EPERM);
		CHECK(!memcmp(package->artifacts[index].data, &fixture.headers[index],
			      sizeof(Elf64_Ehdr)));
	}
	/* The package owns its imported FDs, independently of the receiver. */
	finish(&fixture);
	CHECK(((const Elf64_Ehdr *)package->artifacts[1].data)->e_type == ET_REL);
	kobox_posix_package_close(&package);
	kobox_posix_package_close(&package);
	CHECK(!package && fd_count() == before);
	CHECK(!live_mappings);

	setup(&fixture);
	fixture.input.expected_generation++;
	expect_failure(&fixture, -ESTALE);
	fixture.input.expected_generation--;
	fixture.input.expected_manifest_digest[0] ^= 1;
	expect_failure(&fixture, -EBADMSG);
	fixture.input.expected_manifest_digest[0] ^= 1;
	fixture.input.expected_grant_digest[0] ^= 1;
	expect_failure(&fixture, -EBADMSG);
	fixture.input.expected_grant_digest[0] ^= 1;
	fixture.input.artifact_count = 1;
	expect_failure(&fixture, -EBADMSG);
	fixture.input.artifact_count = 2;
	saved = fixture.descriptors[1];
	fixture.descriptors[1] = blob(&fixture.headers[1], sizeof(Elf64_Ehdr), 0);
	expect_failure(&fixture, -EPERM);
	close(fixture.descriptors[1]);
	fixture.headers[1].e_type = ET_DYN;
	fixture.descriptors[1] = blob(&fixture.headers[1], sizeof(Elf64_Ehdr), 1);
	expect_failure(&fixture, -EBADMSG);
	close(fixture.descriptors[1]);
	fixture.descriptors[1] = saved;
	finish(&fixture);
	CHECK(fd_count() == before);

	/* A valid digest does not authorize rights absent from the manifest. */
	setup(&fixture);
	close(fixture.input.manifest_descriptor);
	close(fixture.input.grant_descriptor);
	fixture.resource = (kb2_closure_manifest_resource_t) {
		.slot_id = 1, .type = KB2_CLOSURE_RESOURCE_CHANNEL,
		.minimum_count = 1, .maximum_count = 1,
		.required_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND,
		.maximum_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND,
		.flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED,
	};
	CHECK(kb2_protocol_copy_schema_digest(fixture.resource.interface_schema_digest, 32) ==
		KB2_PROTOCOL_OK);
	fixture.slot = (kb2_resource_grant_slot_source_t) {
		.slot_id = 1, .resource_type = KB2_CLOSURE_RESOURCE_CHANNEL,
		.state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
	};
	memcpy(fixture.slot.interface_schema_digest, fixture.resource.interface_schema_digest, 32);
	fixture.object = (kb2_resource_grant_object_source_t) {
		.slot_id = 1, .object_id = 101,
		.granted_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND | KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE,
	};
	metadata(&fixture, 17);
	expect_failure(&fixture, -EBADMSG);
	finish(&fixture);
	CHECK(fd_count() == before);
	puts("boot package validation: immutable ownership and rollback passed (not a boot Gate)");
	return 0;
}
