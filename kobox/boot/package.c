// SPDX-License-Identifier: GPL-2.0-only

#include "package.h"

#include <kobox2/sha256.h>

#include <stdbool.h>
#include <string.h>

/* Bound untrusted pre-boot mappings before decoding any counts or tables. */
#define MAX_METADATA_SIZE (16U * 1024U * 1024U)
#define MAX_ARTIFACT_SIZE (128U * 1024U * 1024U)

static void close_blob(struct kobox_boot_package *package,
		       struct kobox_boot_blob *blob)
{
	if (blob->owner)
		package->operations->close(package->context, blob);
	*blob = (struct kobox_boot_blob) {0};
}

void kobox_boot_package_close(struct kobox_boot_package *package)
{
	size_t index;

	if (!package || !package->operations)
		return;
	for (index = package->artifact_count; index; index--)
		close_blob(package, &package->artifacts[index - 1]);
	close_blob(package, &package->grant_blob);
	close_blob(package, &package->manifest_blob);
	memset(package, 0, sizeof(*package));
}

static enum kobox_package_result open_blob(
	struct kobox_boot_package *package, const void *input,
	const uint8_t expected_digest[32], size_t maximum,
	struct kobox_boot_blob *blob)
{
	uint8_t digest[32];
	enum kobox_package_result result;

	result = package->operations->open(package->context, input, maximum, blob);
	if (result != KOBOX_PACKAGE_OK)
		return result;
	if (!blob->owner || !blob->data || !blob->size || blob->size > maximum) {
		close_blob(package, blob);
		return KOBOX_PACKAGE_INVALID;
	}
	kb2_sha256(blob->data, blob->size, digest);
	if (memcmp(digest, expected_digest, sizeof(digest)))
		return KOBOX_PACKAGE_MALFORMED;
	return KOBOX_PACKAGE_OK;
}

static enum kobox_package_result import_artifacts(struct kobox_boot_package *package,
			    const struct kobox_boot_package_input *input)
{
	size_t index;

	for (index = 0; index < package->artifact_count; index++) {
		kb2_closure_manifest_artifact_t artifact;
		struct kobox_boot_blob *blob = &package->artifacts[index];
		enum kobox_package_result result;

		if (kb2_closure_manifest_artifact(&package->manifest, index, &artifact) !=
		    KB2_PROTOCOL_OK || !(artifact.flags & KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX) ||
		    artifact.kind != (index ?
		    KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE : KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER) ||
		    artifact.init_symbol.length || artifact.quiesce_symbol.length ||
		    artifact.cleanup_symbol.length)
			return KOBOX_PACKAGE_EXEC_FORMAT;
		result = open_blob(package, input->artifacts[index], artifact.content_digest,
				   MAX_ARTIFACT_SIZE, blob);
		if (result)
			return result;
		if (blob->size != artifact.content_size)
			return KOBOX_PACKAGE_MALFORMED;
		if (!package->operations->elf_matches(blob->data, blob->size, !index))
			return KOBOX_PACKAGE_EXEC_FORMAT;
	}
	return KOBOX_PACKAGE_OK;
}

static enum kobox_package_result validate_dependency_order(const struct kobox_boot_package *package)
{
	size_t index;

	/* The canonical manifest orders artifacts by node ID. Require each
	 * dependency before its consumer; never invent a parallel Linux loader.
	 */
	for (index = 0; index < kb2_closure_manifest_dependency_count(&package->manifest);
	     index++) {
		kb2_closure_manifest_dependency_t dependency;

		if (kb2_closure_manifest_dependency(&package->manifest, index, &dependency) !=
		    KB2_PROTOCOL_OK || dependency.provider_node_id >= dependency.consumer_node_id)
			return KOBOX_PACKAGE_EXEC_FORMAT;
	}
	return KOBOX_PACKAGE_OK;
}

enum kobox_package_result kobox_boot_package_open(
	const struct kobox_boot_package_input *input,
	struct kobox_boot_package *package)
{
	enum kobox_package_result result;

	if (!package || package->operations || !input ||
	    !input->expected_generation || !input->artifacts ||
	    !input->artifact_count ||
	    input->artifact_count > KOBOX_BOOT_PACKAGE_MAX_ARTIFACTS ||
	    !input->operations || !input->operations->open ||
	    !input->operations->close || !input->operations->elf_matches)
		return KOBOX_PACKAGE_INVALID;
	memset(package, 0, sizeof(*package));
	package->operations = input->operations;
	package->context = input->context;
	package->artifact_count = input->artifact_count;
	result = open_blob(package, input->manifest, input->expected_manifest_digest,
			   MAX_METADATA_SIZE, &package->manifest_blob);
	if (result)
		goto fail;
	result = open_blob(package, input->grant, input->expected_grant_digest,
			   MAX_METADATA_SIZE, &package->grant_blob);
	if (result)
		goto fail;
	if (kb2_closure_manifest_decode(package->manifest_blob.data, package->manifest_blob.size,
					&package->manifest) != KB2_PROTOCOL_OK ||
	    kb2_closure_manifest_artifact_count(&package->manifest) != package->artifact_count ||
	    kb2_resource_grant_decode(package->grant_blob.data, package->grant_blob.size,
				     &package->grant) != KB2_PROTOCOL_OK ||
	    kb2_resource_grant_validate_manifest(&package->grant, &package->manifest) !=
	    KB2_PROTOCOL_OK) {
		result = KOBOX_PACKAGE_MALFORMED;
		goto fail;
	}
	if (package->grant.generation != input->expected_generation) {
		result = KOBOX_PACKAGE_STALE;
		goto fail;
	}
	result = validate_dependency_order(package);
	if (!result)
		result = import_artifacts(package, input);
	if (result)
		goto fail;
	return KOBOX_PACKAGE_OK;
fail:
	kobox_boot_package_close(package);
	return result;
}
