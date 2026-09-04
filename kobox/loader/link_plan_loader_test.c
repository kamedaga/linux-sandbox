// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "link_plan_loader.h"

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

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static int copy_all(int destination, int source)
{
	uint8_t buffer[4096];

	for (;;) {
		ssize_t received = read(source, buffer, sizeof(buffer));

		if (received < 0 && errno == EINTR)
			continue;
		if (received < 0)
			return -1;
		if (!received)
			return 0;
		{
			size_t offset = 0;

			while (offset < (size_t)received) {
				ssize_t written = write(
					destination, buffer + offset,
					(size_t)received - offset);

				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					return -1;
				offset += (size_t)written;
			}
		}
	}
}

static int make_artifact(const char *path, const char *name,
			 struct kobox_link_plan_node *node)
{
	struct stat status;
	void *mapping = MAP_FAILED;
	int destination = -1;
	int source = -1;

	source = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (source < 0 || fstat(source, &status) || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0)
		goto error;
	mapping = mmap(NULL, (size_t)status.st_size, PROT_READ, MAP_PRIVATE,
		       source, 0);
	if (mapping == MAP_FAILED)
		goto error;
	destination = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (destination < 0 || copy_all(destination, source) ||
	    lseek(destination, 0, SEEK_SET) != 0 ||
	    fcntl(destination, F_ADD_SEALS,
		  F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE))
		goto error;
	node->content_size = (uint64_t)status.st_size;
	kb2_sha256(mapping, (size_t)status.st_size, node->content_digest);
	munmap(mapping, (size_t)status.st_size);
	close(source);
	return destination;

error:
	if (mapping != MAP_FAILED)
		munmap(mapping, (size_t)status.st_size);
	if (source >= 0)
		close(source);
	if (destination >= 0)
		close(destination);
	return -1;
}

int main(int argument_count, char **arguments)
{
	static const struct kobox_link_plan_export provider_exports[] = {
		{ "kobox_fixture_provider_add",
		  KOBOX_LINK_PLAN_SYMBOL_FUNCTION },
	};
	static const struct kobox_link_plan_import consumer_imports[] = {
		{ "kobox_fixture_provider_add", 1u, 0u },
	};
	static const struct kobox_link_plan_export lifecycle_exports[] = {
		{ "cleanup_module", KOBOX_LINK_PLAN_SYMBOL_FUNCTION },
		{ "init_module", KOBOX_LINK_PLAN_SYMBOL_FUNCTION },
	};
	struct kobox_link_plan_node nodes[] = {
		{
			.name = "fixture_core.so",
			.kind = KOBOX_LINK_PLAN_SHARED_PROVIDER,
		},
		{
			.name = "fixture_provider.ko",
			.kind = KOBOX_LINK_PLAN_RELOCATABLE_MODULE,
			.exports = provider_exports,
			.export_count = ARRAY_SIZE(provider_exports),
		},
		{
			.name = "fixture_consumer.ko",
			.kind = KOBOX_LINK_PLAN_RELOCATABLE_MODULE,
			.imports = consumer_imports,
			.import_count = ARRAY_SIZE(consumer_imports),
		},
	};
	struct kobox_link_plan plan = {
		.identity = "dev",
		.nodes = nodes,
		.node_count = ARRAY_SIZE(nodes),
	};
	struct kobox_link_plan_loader_config config;
	struct kobox_link_plan_loader *loader = NULL;
	struct kobox_link_plan_error error = { 0 };
	struct kobox_link_plan_node lifecycle_nodes[] = {
		{
			.name = "fixture_core.so",
			.kind = KOBOX_LINK_PLAN_SHARED_PROVIDER,
		},
		{
			.name = "fixture_raw.ko",
			.kind = KOBOX_LINK_PLAN_RELOCATABLE_MODULE,
			.exports = lifecycle_exports,
			.export_count = ARRAY_SIZE(lifecycle_exports),
			.init_symbol = "init_module",
			.cleanup_symbol = "cleanup_module",
		},
	};
	struct kobox_link_plan lifecycle_plan = {
		.identity = "dev",
		.nodes = lifecycle_nodes,
		.node_count = ARRAY_SIZE(lifecycle_nodes),
	};
	struct kobox_link_plan_node failure_nodes[3];
	struct kobox_link_plan failure_plan;
	int descriptors[ARRAY_SIZE(nodes)] = { -1, -1, -1 };
	int lifecycle_descriptors[3] = { -1, -1, -1 };
	size_t failed_node;
	uintptr_t entry_address;
	int entry_status;
	size_t index;
	int result = 1;

	if (argument_count != 6)
		return 2;
	for (index = 0; index < ARRAY_SIZE(nodes); index++) {
		descriptors[index] = make_artifact(
			arguments[index + 1], nodes[index].name, &nodes[index]);
		if (descriptors[index] < 0)
			goto out;
	}
	config = (struct kobox_link_plan_loader_config){
		.plan = &plan,
		.artifact_descriptors = descriptors,
		.artifact_count = ARRAY_SIZE(descriptors),
		.error = &error,
	};
	if (kobox_link_plan_loader_open(&config, &loader) !=
		    KOBOX_LINK_PLAN_OK ||
	    kobox_link_plan_loader_count(loader) != ARRAY_SIZE(nodes) ||
	    kobox_link_plan_loader_close(&loader) != KOBOX_LINK_PLAN_OK)
		goto out;
	lifecycle_descriptors[0] = make_artifact(
		arguments[1], lifecycle_nodes[0].name, &lifecycle_nodes[0]);
	lifecycle_descriptors[1] = make_artifact(
		arguments[4], lifecycle_nodes[1].name, &lifecycle_nodes[1]);
	if (lifecycle_descriptors[0] < 0 || lifecycle_descriptors[1] < 0)
		goto out;
	config = (struct kobox_link_plan_loader_config){
		.plan = &lifecycle_plan,
		.artifact_descriptors = lifecycle_descriptors,
		.artifact_count = lifecycle_plan.node_count,
		.error = &error,
	};
	if (kobox_link_plan_loader_open(&config, &loader) !=
		    KOBOX_LINK_PLAN_OK ||
	    kobox_link_plan_loader_find_node(
		    loader, "fixture_raw.ko", &failed_node) !=
		    KOBOX_LINK_PLAN_OK ||
	    failed_node != 1 ||
	    kobox_link_plan_loader_export(
		    loader, failed_node, "init_module",
		    KOBOX_LINK_PLAN_SYMBOL_FUNCTION, &entry_address) !=
		    KOBOX_LINK_PLAN_OK ||
	    !entry_address ||
	    kobox_link_plan_loader_start_modules_through(
		    loader, failed_node, NULL, NULL) != KOBOX_LINK_PLAN_OK ||
	    kobox_link_plan_loader_lifecycle_cursor(loader) != failed_node ||
	    kobox_link_plan_loader_close(&loader) !=
		    KOBOX_LINK_PLAN_INVALID_STATE ||
	    kobox_link_plan_loader_start_modules(loader, NULL, NULL) !=
		    KOBOX_LINK_PLAN_OK ||
	    kobox_link_plan_loader_close(&loader) !=
		    KOBOX_LINK_PLAN_INVALID_STATE ||
	    kobox_link_plan_loader_stop_modules_to(loader, failed_node) !=
		    KOBOX_LINK_PLAN_OK ||
	    kobox_link_plan_loader_lifecycle_cursor(loader) != failed_node ||
	    kobox_link_plan_loader_stop_modules(loader) != KOBOX_LINK_PLAN_OK ||
	    kobox_link_plan_loader_close(&loader) != KOBOX_LINK_PLAN_OK)
		goto out;
	memcpy(failure_nodes, lifecycle_nodes, sizeof(lifecycle_nodes));
	failure_nodes[2] = lifecycle_nodes[1];
	failure_nodes[2].name = "fixture_raw_fail.ko";
	failure_plan = lifecycle_plan;
	failure_plan.nodes = failure_nodes;
	failure_plan.node_count = ARRAY_SIZE(failure_nodes);
	lifecycle_descriptors[2] = make_artifact(
		arguments[5], failure_nodes[2].name, &failure_nodes[2]);
	if (lifecycle_descriptors[2] < 0)
		goto out;
	config.plan = &failure_plan;
	config.artifact_count = failure_plan.node_count;
	if (kobox_link_plan_loader_open(&config, &loader) !=
		    KOBOX_LINK_PLAN_OK ||
	    kobox_link_plan_loader_start_modules(
		    loader, &failed_node, &entry_status) !=
		    KOBOX_LINK_PLAN_ENTRY_FAILURE ||
	    failed_node != 2 || entry_status != -23 ||
	    kobox_link_plan_loader_close(&loader) != KOBOX_LINK_PLAN_OK)
		goto out;
	nodes[2].content_digest[0] ^= 1u;
	config.plan = &plan;
	config.artifact_descriptors = descriptors;
	config.artifact_count = ARRAY_SIZE(descriptors);
	if (kobox_link_plan_loader_open(&config, &loader) !=
		    KOBOX_LINK_PLAN_ARTIFACT_FAILURE ||
	    loader || error.phase != KOBOX_LINK_PLAN_PHASE_VALIDATE ||
	    error.node_index != 2)
		goto out;
	result = 0;

out:
	if (loader)
		kobox_link_plan_loader_close(&loader);
	for (index = 0; index < ARRAY_SIZE(descriptors); index++) {
		if (descriptors[index] >= 0)
			close(descriptors[index]);
	}
	for (index = 0; index < ARRAY_SIZE(lifecycle_descriptors); index++) {
		if (lifecycle_descriptors[index] >= 0)
			close(lifecycle_descriptors[index]);
	}
	if (result)
		fprintf(stderr, "link plan loader test failed at phase %u node %zu\n",
			(unsigned int)error.phase, error.node_index);
	return result;
}
