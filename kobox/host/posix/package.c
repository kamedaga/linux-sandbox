// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "package.h"
#include "../../arch/x86_64/elf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct package_owner {
	struct kobox_boot_package package;
	int error;
};

struct blob_owner {
	int descriptor;
};

static void close_blob(void *context, struct kobox_boot_blob *blob)
{
	struct blob_owner *owner = blob->owner;

	(void)context;
	if (blob->data && munmap((void *)blob->data, blob->size))
		abort();
	if (owner) {
		if (owner->descriptor >= 0)
			close(owner->descriptor);
		free(owner);
	}
	*blob = (struct kobox_boot_blob) {0};
}

static enum kobox_package_result open_blob(
	void *context, const void *input, size_t maximum,
	struct kobox_boot_blob *blob)
{
	const int seals = F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
	struct package_owner *package = context;
	struct blob_owner *owner;
	struct stat status;
	void *data;
	int flags;
	enum kobox_package_result result = KOBOX_PACKAGE_IO;

	if (!input)
		return KOBOX_PACKAGE_INVALID;
	owner = malloc(sizeof(*owner));
	if (!owner)
		return KOBOX_PACKAGE_NO_MEMORY;
	blob->owner = owner;
	owner->descriptor = fcntl(*(const int *)input, F_DUPFD_CLOEXEC, 0);
	if (owner->descriptor < 0) {
		package->error = errno;
		goto fail;
	}
	flags = fcntl(owner->descriptor, F_GET_SEALS);
	if (flags < 0 || (flags & seals) != seals) {
		result = KOBOX_PACKAGE_IMMUTABILITY;
		goto fail;
	}
	if (fstat(owner->descriptor, &status)) {
		package->error = errno;
		goto fail;
	}
	if (!S_ISREG(status.st_mode) || status.st_size <= 0 ||
	    (uint64_t)status.st_size > maximum) {
		result = KOBOX_PACKAGE_TOO_LARGE;
		goto fail;
	}
	blob->size = status.st_size;
	data = mmap(NULL, blob->size, PROT_READ, MAP_PRIVATE, owner->descriptor, 0);
	if (data == MAP_FAILED) {
		package->error = errno;
		goto fail;
	}
	blob->data = data;
	return KOBOX_PACKAGE_OK;
fail:
	close_blob(context, blob);
	return result;
}

static const struct kobox_package_operations operations = {
	.open = open_blob,
	.close = close_blob,
	.elf_matches = kobox_x86_64_elf_matches,
};

static int native_error(enum kobox_package_result result, int error)
{
	switch (result) {
	case KOBOX_PACKAGE_OK: return 0;
	case KOBOX_PACKAGE_INVALID: return -EINVAL;
	case KOBOX_PACKAGE_NO_MEMORY: return -ENOMEM;
	case KOBOX_PACKAGE_IO: return error ? -error : -EIO;
	case KOBOX_PACKAGE_IMMUTABILITY: return -EPERM;
	case KOBOX_PACKAGE_TOO_LARGE: return -EFBIG;
	case KOBOX_PACKAGE_MALFORMED: return -EBADMSG;
	case KOBOX_PACKAGE_EXEC_FORMAT: return -ENOEXEC;
	case KOBOX_PACKAGE_STALE: return -ESTALE;
	}
	return -EIO;
}

int kobox_posix_package_open(const struct kobox_posix_package_input *input,
			     struct kobox_boot_package **out)
{
	const void *artifacts[KOBOX_BOOT_PACKAGE_MAX_ARTIFACTS];
	struct kobox_boot_package_input common;
	struct package_owner *owner;
	enum kobox_package_result result;
	size_t index;
	int error;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!input || !input->artifact_descriptors || !input->artifact_count ||
	    input->artifact_count > KOBOX_BOOT_PACKAGE_MAX_ARTIFACTS)
		return -EINVAL;
	owner = calloc(1, sizeof(*owner));
	if (!owner)
		return -ENOMEM;
	for (index = 0; index < input->artifact_count; index++)
		artifacts[index] = &input->artifact_descriptors[index];
	common = (struct kobox_boot_package_input) {
		.expected_generation = input->expected_generation,
		.manifest = &input->manifest_descriptor,
		.grant = &input->grant_descriptor,
		.artifacts = artifacts, .artifact_count = input->artifact_count,
		.operations = &operations, .context = owner,
	};
	memcpy(common.expected_manifest_digest, input->expected_manifest_digest, 32);
	memcpy(common.expected_grant_digest, input->expected_grant_digest, 32);
	result = kobox_boot_package_open(&common, &owner->package);
	if (result != KOBOX_PACKAGE_OK) {
		error = native_error(result, owner->error);
		free(owner);
		return error;
	}
	*out = &owner->package;
	return 0;
}

void kobox_posix_package_close(struct kobox_boot_package **package)
{
	struct package_owner *owner;

	if (!package || !*package)
		return;
	owner = (*package)->context;
	kobox_boot_package_close(*package);
	*package = NULL;
	free(owner);
}

int kobox_posix_blob_descriptor(const struct kobox_boot_blob *blob)
{
	const struct blob_owner *owner = blob ? blob->owner : NULL;

	return owner ? owner->descriptor : -1;
}
