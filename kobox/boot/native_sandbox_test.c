// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "boot_test.h"
#include "../host/posix/package.h"
#include "pci_fixture.h"
#include "resource_registry.h"
#include "lifecycle_fixture.h"
#include "module_launch.h"
#include "pci_resource.h"
#include "dma_fixture.h"

#include <kobox2_test/bootstrap.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define DESCRIPTORS (2 + KOBOX_BOOT_PACKAGE_MAX_ARTIFACTS + 2)

struct native_launch {
	struct kobox_boot_package *package;
	struct kobox_resource_runtime *runtime;
	struct kobox_pci_fixture_owner owner;
	struct kobox_lifecycle_fixture lifecycle;
	struct kobox_boot_pci_resource pci;
	struct kobox_linux_resource_port port;
	struct kobox_dma_fixture *dma;
	void *dma_object;
	int pci_mode;
};

static int import_resource(void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles, size_t count, void **out,
	const struct kobox_resource_interface_operations **operations)
{
	struct native_launch *launch = context;
	int result;

	if (slot->slot_id == 2 && launch->dma) {
		result = kobox_dma_fixture_import(launch->dma, slot, object, handles, count, out, operations);
		if (!result)
			launch->dma_object = *out;
		return result;
	}
	return launch->pci_mode ?
		kobox_pci_enum_fixture_import(&launch->owner, slot, object, handles, count, out, operations) :
		kobox_pci_fixture_import(&launch->owner, slot, object, handles, count, out, operations);
}

static void release_resource(void *context, void *object)
{
	struct native_launch *launch = context;

	if (object == launch->dma_object) {
		kobox_dma_fixture_release(launch->dma, object);
		launch->dma_object = NULL;
	} else if (launch->pci_mode) {
		kobox_pci_enum_fixture_release(&launch->owner, object);
	} else {
		kobox_pci_fixture_release(&launch->owner, object);
	}
}

static int prepare_dma(void *context, int descriptor, size_t length)
{
	struct native_launch *launch = context;

	return kobox_dma_fixture_prepare(launch->dma, &launch->port, descriptor, length);
}

static void close_resources(void *context)
{
	struct native_launch *launch = context;

	kobox_lifecycle_fixture_close(&launch->lifecycle);
	if (launch->dma && kobox_dma_fixture_finish(launch->dma))
		abort();
	kobox_resource_runtime_close(&launch->runtime);
	kobox_dma_fixture_destroy(launch->dma);
	kobox_posix_package_close(&launch->package);
	if (launch->owner.imported != 1 || launch->owner.released != 1)
		__builtin_trap();
	fprintf(stderr, "Native resource cleanup: imports=1 releases=1 generation=%llu\n",
		(unsigned long long)launch->owner.generation);
}

static int number(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	if (!text[0] || text[0] < '0' || text[0] > '9')
		return -EINVAL;
	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno || *end || !parsed)
		return -EINVAL;
	*value = parsed;
	return 0;
}

static int digest(const char *text, uint8_t output[32])
{
	const char *digits = "0123456789abcdef";
	size_t index;

	if (strlen(text) != 64)
		return -EINVAL;
	for (index = 0; index < 32; index++) {
		const char *high = strchr(digits, text[index * 2]);
		const char *low = strchr(digits, text[index * 2 + 1]);

		if (!high || !low)
			return -EINVAL;
		output[index] = ((high - digits) << 4) | (low - digits);
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct native_launch launch = {0};
	struct kobox_posix_package_input input = {0};
	struct kobox_linux_native_module modules[KOBOX_BOOT_PACKAGE_MAX_ARTIFACTS];
	char names[KOBOX_BOOT_PACKAGE_MAX_ARTIFACTS][128];
	struct kobox_linux_module_launch module_launch = {
		.size = sizeof(module_launch), .modules = modules,
		.lifecycle = &launch.lifecycle.port,
	};
	struct kobox_posix_resource_config config = {0};
	struct kobox_boot_test_resources resources = {
		.port = &launch.port, .lifecycle = &launch.lifecycle.port,
		.modules = &module_launch,
		.close = close_resources, .context = &launch,
	};
	struct stat anchor;
	int descriptors[DESCRIPTORS];
	char core_path[64];
	char *arguments[] = {argv[0], core_path, NULL};
	uint64_t parent;
	size_t count = 0, index, resource_count;
	int result;
	int dma_mode;

	if (argc == 3 && (!strcmp(argv[1], "--dma-engine") ||
			  !strcmp(argv[1], "--dma-engine-fail-unmap"))) {
		if (number(argv[2], &parent) || prctl(PR_SET_PDEATHSIG, SIGKILL) ||
		    (uint64_t)getppid() != parent)
			return 64;
		return kobox_dma_fixture_engine(KB2_TEST_BOOTSTRAP_FD,
			!strcmp(argv[1], "--dma-engine-fail-unmap"));
	}

	/* Digests and generation are trusted exec arguments, independent of
	 * the received package. FD 4 is an ownership anchor, never a runtime
	 * resource: only fstat it, then close it before importing SCM_RIGHTS.
	 */
	if (argc != 7 || number(argv[1], &input.expected_generation) ||
	    number(argv[2], &launch.owner.object_id) ||
	    digest(argv[3], input.expected_manifest_digest) ||
	    digest(argv[4], input.expected_grant_digest) || number(argv[5], &parent) ||
	    (strcmp(argv[6], "normal") && strcmp(argv[6], "fail-init") &&
	     strcmp(argv[6], "pci") && strcmp(argv[6], "pci-dma")) ||
	    prctl(PR_SET_PDEATHSIG, SIGKILL) || (uint64_t)getppid() != parent ||
	    fstat(KB2_TEST_NATIVE_OWNER_FD, &anchor))
		return 64;
	dma_mode = !strcmp(argv[6], "pci-dma");
	launch.pci_mode = !strcmp(argv[6], "pci") || dma_mode;
	resource_count = dma_mode ? 2 : 1;
	launch.owner.generation = input.expected_generation;
	launch.owner.device = anchor.st_dev;
	launch.owner.inode = anchor.st_ino;
	close(KB2_TEST_NATIVE_OWNER_FD);
	if (dma_mode) {
		if (launch.owner.object_id == UINT64_MAX ||
		    fstat(KB2_TEST_NATIVE_IOMMU_OWNER_FD, &anchor) ||
		    kobox_dma_fixture_create(input.expected_generation, launch.owner.object_id + 1,
			launch.owner.object_id, anchor.st_dev, anchor.st_ino, &launch.dma))
			return 64;
		close(KB2_TEST_NATIVE_IOMMU_OWNER_FD);
		resources.dma = kobox_dma_fixture_test(launch.dma);
		resources.prepare_dma = prepare_dma;
	}
	if (!kb2_test_receive_handles(KB2_TEST_BOOTSTRAP_FD, descriptors,
				     DESCRIPTORS, &count))
		return 65;
	if (count < 4 + resource_count) {
		for (index = 0; index < count; index++)
			close(descriptors[index]);
		return 65;
	}
	input.manifest_descriptor = descriptors[0];
	input.grant_descriptor = descriptors[1];
	input.artifact_descriptors = descriptors + 2;
	input.artifact_count = count - 2 - resource_count;
	result = kobox_posix_package_open(&input, &launch.package);
	for (index = 0; index < count - resource_count; index++)
		close(descriptors[index]);
	if (result) {
		fprintf(stderr, "Native package rejected: %d\n", result);
		for (index = count - resource_count; index < count; index++)
			close(descriptors[index]);
		return 66;
	}
	config = (struct kobox_posix_resource_config) {
		.manifest = &launch.package->manifest, .grant = &launch.package->grant,
		.native_handles = &descriptors[count - resource_count], .native_handle_count = resource_count,
		.import_object = import_resource, .release_object = release_resource,
		.object_context = &launch,
	};
	result = kobox_posix_resource_open(&config, &launch.runtime);
	for (index = count - resource_count; index < count; index++)
		close(descriptors[index]);
	if (result) {
		fprintf(stderr, "Native resource rejected: %d\n", result);
		kobox_posix_package_close(&launch.package);
		return 67;
	}
	if (kobox_boot_resource_port(launch.runtime, &launch.port))
		return 68;
	if (launch.pci_mode) {
		result = kobox_boot_pci_resource(&launch.port, 1, 0, &launch.pci);
		if (result) {
			fprintf(stderr, "Native PCI grant rejected: %d\n", result);
			kobox_resource_runtime_close(&launch.runtime);
			kobox_posix_package_close(&launch.package);
			return 68;
		}
		resources.pci = &launch.pci.host;
	}
	if (kobox_lifecycle_fixture_start(&launch.lifecycle, KB2_TEST_BOOTSTRAP_FD,
					  launch.owner.generation))
		return 68;
	module_launch.count = launch.package->artifact_count - 1;
	for (index = 0; index < module_launch.count; index++) {
		kb2_closure_manifest_artifact_t artifact;
		const struct kobox_boot_blob *image = &launch.package->artifacts[index + 1];

		if (kb2_closure_manifest_artifact(&launch.package->manifest, index + 1,
						 &artifact) != KB2_PROTOCOL_OK ||
		    artifact.namespace_name.length >= sizeof(names[index]))
			return 69;
		memcpy(names[index], artifact.namespace_name.data, artifact.namespace_name.length);
		names[index][artifact.namespace_name.length] = 0;
		modules[index] = (struct kobox_linux_native_module) {
			.image = image->data, .length = image->size, .name = names[index],
		};
		/* Conformance driver parameters are fixture policy, not part of
		 * the generic native module lifecycle or resource port.
		 */
		if (!strcmp(names[index], "resource_test"))
			modules[index].parameters = !strcmp(argv[6], "fail-init") ?
				"fail_after_map=1" : "heartbeat=1";
	}
	if (snprintf(core_path, sizeof(core_path), "/proc/self/fd/%d",
		     kobox_posix_blob_descriptor(&launch.package->artifacts[0])) >= (int)sizeof(core_path))
		return 69;
	return kobox_boot_test_run(2, arguments, &resources);
}
