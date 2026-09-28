// SPDX-License-Identifier: GPL-2.0-only
/* Exercise an ordered module closure in a fresh hosted Linux with no device
 * grant. This is intentionally unable to probe a real PCI function.
 */
#include "boot_test.h"
#include "module_launch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_MODULES 64
#define MAX_FIRMWARE KOBOX_FIRMWARE_MAX_FILES

static struct kobox_linux_native_module modules[MAX_MODULES];
static struct kobox_linux_firmware_file firmware[MAX_FIRMWARE];
static struct kobox_linux_lifecycle lifecycle;

static int ready(void *context)
{
	return context == &lifecycle ? 0 : -EINVAL;
}

static int pending(void *context)
{
	return context == &lifecycle ? 1 : -EINVAL;
}

static void close_images(void *context)
{
	size_t index;
	size_t count = *(size_t *)context;

	for (index = 0; index < count; index++) {
		munmap((void *)modules[index].image, modules[index].length);
		free((void *)modules[index].name);
		free((void *)modules[index].parameters);
	}
	for (index = 0; index < MAX_FIRMWARE; index++) {
		if (!firmware[index].name)
			break;
		munmap((void *)firmware[index].data, firmware[index].size);
		free((void *)firmware[index].name);
	}
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static int add_firmware(const char *entry, size_t index)
{
	const char *separator = strchr(entry, '=');
	const char *path;
	struct stat metadata;
	int descriptor;

	if (!separator || separator == entry || separator - entry > 127 ||
	    strlen(separator + 1) < 66 || separator[65] != '=')
		return -EINVAL;
	path = separator + 66;
	if (!*path)
		return -EINVAL;
	for (size_t i = 0; i < 32; i++) {
		int hi = hex_digit(separator[1 + 2 * i]);
		int lo = hex_digit(separator[2 + 2 * i]);

		if (hi < 0 || lo < 0)
			return -EINVAL;
		firmware[index].sha256[i] = (hi << 4) | lo;
	}
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -errno;
	if (fstat(descriptor, &metadata) || !S_ISREG(metadata.st_mode) ||
	    metadata.st_size <= 0 || metadata.st_size > (8L << 20)) {
		close(descriptor);
		return -EINVAL;
	}
	firmware[index].size = metadata.st_size;
	firmware[index].data = mmap(NULL, metadata.st_size, PROT_READ,
				    MAP_PRIVATE, descriptor, 0);
	close(descriptor);
	if (firmware[index].data == MAP_FAILED)
		return -errno;
	firmware[index].name = strndup(entry, separator - entry);
	if (!firmware[index].name) {
		munmap((void *)firmware[index].data, firmware[index].size);
		return -ENOMEM;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct kobox_linux_module_launch launch = {
		.size = sizeof(launch), .modules = modules, .lifecycle = &lifecycle,
	};
	struct kobox_boot_test_resources resources = {
		.modules = &launch, .close = close_images, .context = &launch.count,
	};
	size_t index;

	if (argc < 3 || (unsigned int)argc > MAX_MODULES + MAX_FIRMWARE + 2) {
		fprintf(stderr, "usage: %s CORE [--firmware=NAME=SHA256=FILE ...] NAME=MODULE.ko [...]\n", argv[0]);
		return 64;
	}
	lifecycle = (struct kobox_linux_lifecycle) {
		.size = sizeof(lifecycle), .context = &lifecycle,
		.ready = ready, .pending = pending,
	};
	index = 2;
	while (index < (size_t)argc &&
	       !strncmp(argv[index], "--firmware=", 11)) {
		if (launch.firmware_count == MAX_FIRMWARE ||
		    add_firmware(argv[index] + 11, launch.firmware_count))
			return 64;
		launch.firmware_count++;
		index++;
	}
	if (launch.firmware_count)
		launch.firmware = firmware;
	launch.count = argc - index;
	if (!launch.count || launch.count > MAX_MODULES)
		return 64;
	for (index = 0; index < launch.count; index++) {
		const char *entry = argv[argc - launch.count + index];
		const char *separator = strchr(entry, '=');
		const char *parameters;
		char *path;
		struct stat metadata;
		int descriptor;

		if (!separator || separator == entry || !separator[1] ||
		    separator - entry >= 56)
			return 64;
		modules[index].name = strndup(entry, separator - entry);
		if (!modules[index].name)
			return 65;
		parameters = strchr(separator + 1, ';');
		path = parameters ? strndup(separator + 1,
					    parameters - separator - 1) :
				  strdup(separator + 1);
		if (!path)
			return 65;
		if (parameters) {
			modules[index].parameters = strdup(parameters + 1);
			if (!modules[index].parameters)
				return 65;
		}
		descriptor = open(path, O_RDONLY | O_CLOEXEC);
		free(path);
		if (descriptor < 0 || fstat(descriptor, &metadata) ||
		    metadata.st_size <= 0 || metadata.st_size > (64L << 20))
			return 66;
		modules[index].length = metadata.st_size;
		modules[index].image = mmap(NULL, metadata.st_size, PROT_READ,
					   MAP_PRIVATE, descriptor, 0);
		close(descriptor);
		if (modules[index].image == MAP_FAILED)
			return 66;
	}
	return kobox_boot_test_run(2, argv, &resources);
}
