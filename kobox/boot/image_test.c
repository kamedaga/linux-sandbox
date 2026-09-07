// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "image.h"

#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define RAM_SIZE (8UL << 20)
#define IMAGE_BASE (1UL << 20)

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "image check failed at line %d: %s\n", \
			__LINE__, #expression); \
		return 1; \
	} \
} while (0)

static bool access_faults(void *address, bool writing)
{
	pid_t child = fork();
	int status;

	if (child < 0)
		return false;
	if (!child) {
		struct rlimit limit = {0};
		volatile unsigned char *byte = address;
		unsigned char value;

		if (setrlimit(RLIMIT_CORE, &limit) ||
		    signal(SIGSEGV, SIG_DFL) == SIG_ERR)
			_exit(2);
		value = *byte;
		if (writing)
			*byte = value;
		_exit(0);
	}
	return waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
		WTERMSIG(status) == SIGSEGV;
}

int main(int argc, char **argv)
{
	struct kobox_posix_memory_backing backing = {0};
	struct kobox_posix_memory_window direct = {0};
	struct kobox_boot_image image = {0};
	struct link_map *map;
	unsigned int (*advance)(void);
	unsigned long *data;
	unsigned long *alias;
	void *memory;
	void *function;
	void *handle;
	size_t data_offset;

	CHECK(argc == 2);
	handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	CHECK(handle);
	CHECK(dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0);
	function = dlsym(handle, "fixture_advance");
	data = dlsym(handle, "fixture_data");
	CHECK(function && data);
	memcpy(&advance, &function, sizeof(advance));
	CHECK(advance() == 8);
	CHECK(kobox_posix_memory_backing_init(&backing, RAM_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&direct, RAM_SIZE) == 0);
	CHECK(kobox_posix_memory_window_map(&direct, 0, &backing, 0, RAM_SIZE,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE, &memory) == 0);
	CHECK(kobox_boot_image_alias(NULL, &backing, memory, IMAGE_BASE,
		&image) == EINVAL);
	CHECK(kobox_boot_image_alias(handle, &backing, memory, RAM_SIZE,
		&image) == EINVAL);
	CHECK(kobox_boot_image_alias(handle, &backing, memory, RAM_SIZE - 4096,
		&image) == EOVERFLOW);
	CHECK(advance() == 9 && *data == 0x87654321);
	CHECK(kobox_boot_image_alias(handle, &backing, memory, IMAGE_BASE,
		&image) == 0);
	CHECK(image.size && image.size < RAM_SIZE - IMAGE_BASE);
	alias = (unsigned long *)((char *)memory + IMAGE_BASE +
		((uintptr_t)data - map->l_addr));
	CHECK(*alias == *data && advance() == 10);
	*data = 0x1234;
	CHECK(*alias == 0x1234);
	*alias = 0x5678;
	CHECK(*data == 0x5678);
	CHECK(memcmp(function, (char *)memory + IMAGE_BASE +
		((uintptr_t)function - map->l_addr), 16) == 0);
	data_offset = ((uintptr_t)data - map->l_addr) & ~(size_t)4095;
	CHECK(kobox_boot_image_protect(NULL, 0, 4096, KOBOX_IMAGE_READ) == EINVAL);
	CHECK(kobox_boot_image_protect(&image, data_offset, 4096,
		KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE) == EINVAL);
	CHECK(kobox_boot_image_protect(&image, image.size, 4096, 0) == EINVAL);
	CHECK(kobox_boot_image_protect(&image, data_offset + 1, 4096, 0) == EINVAL);
	CHECK(kobox_boot_image_protect(&image, data_offset, 4096,
		KOBOX_IMAGE_READ) == 0);
	CHECK(*data == 0x5678 && access_faults(data, true));
	CHECK(access_faults(alias, true));
	CHECK(kobox_boot_image_protect(&image, data_offset, 4096, 0) == 0);
	CHECK(access_faults(data, false));
	/* The backing survives image revocation for Linux buddy reuse. */
	CHECK(*alias == 0x5678);
	*alias = 0xabcdef;
	CHECK(*alias == 0xabcdef);
	CHECK(advance() == 11);
	kobox_boot_image_destroy(&image);
	CHECK(dlclose(handle) == 0);
	CHECK(kobox_posix_memory_window_destroy(&direct) == 0);
	CHECK(kobox_posix_memory_backing_destroy(&backing) == 0);
	return 0;
}
