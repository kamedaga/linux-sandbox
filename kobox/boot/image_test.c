// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../host/posix/image.h"
#include "image_layout.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define RAM_SIZE (64UL << 20)

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
	struct kobox_posix_core *core = NULL;
	struct kobox_boot_image image = {0};
	struct kobox_boot_core *boot;
	unsigned long *data, *alias;
	unsigned long *(*probe)(void);
	void *base, *memory, *function;
	size_t data_offset;

	CHECK(argc == 2);
	CHECK(!kobox_posix_core_open(argv[1], &core));
	boot = kobox_posix_core_boot(core);
	base = kobox_posix_core_base(core);
	CHECK(boot && base);
	function = boot->lookup(boot->loader, "kobox_linux_tls_probe");
	data = boot->lookup(boot->loader, "jiffies");
	CHECK(function && data);
	memcpy(&probe, &function, sizeof(probe));
	CHECK(*probe() == 0x12345678UL);
	*data = 0x87654321UL;
	CHECK(!kobox_posix_memory_backing_init(&backing, RAM_SIZE));
	CHECK(!kobox_posix_memory_window_init(&direct, RAM_SIZE));
	CHECK(!kobox_posix_memory_window_map(&direct, 0, &backing, 0, RAM_SIZE,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE, &memory));
	CHECK(kobox_boot_image_alias(NULL, &backing, memory,
		KOBOX_CORE_PHYSICAL_BASE, &image) == EINVAL);
	CHECK(kobox_boot_image_alias(core, &backing, memory,
		RAM_SIZE, &image) == EINVAL);
	CHECK(!kobox_boot_image_alias(core, &backing, memory,
		KOBOX_CORE_PHYSICAL_BASE, &image));
	CHECK(image.size && image.size < RAM_SIZE - KOBOX_CORE_PHYSICAL_BASE);
	alias = (unsigned long *)((unsigned char *)memory +
		KOBOX_CORE_PHYSICAL_BASE + ((uintptr_t)data - (uintptr_t)base));
	CHECK(*alias == *data && *probe() == 0x12345678UL);
	*data = 0x1234;
	CHECK(*alias == 0x1234);
	*alias = 0x5678;
	CHECK(*data == 0x5678);
	CHECK(!memcmp(function, (unsigned char *)memory +
		KOBOX_CORE_PHYSICAL_BASE +
		((uintptr_t)function - (uintptr_t)base), 16));
	data_offset = ((uintptr_t)data - (uintptr_t)base) & ~(size_t)4095;
	CHECK(kobox_boot_image_protect(NULL, 0, 4096, KOBOX_IMAGE_READ) == EINVAL);
	CHECK(kobox_boot_image_protect(&image, data_offset, 4096,
		KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE) == EINVAL);
	CHECK(kobox_boot_image_protect(&image, image.size, 4096, 0) == EINVAL);
	CHECK(kobox_boot_image_protect(&image, data_offset + 1, 4096, 0) == EINVAL);
	CHECK(!kobox_boot_image_protect(&image, data_offset, 4096,
		KOBOX_IMAGE_READ));
	CHECK(*data == 0x5678 && access_faults(data, true));
	CHECK(access_faults(alias, true));
	CHECK(!kobox_boot_image_protect(&image, data_offset, 4096, 0));
	CHECK(access_faults(data, false));
	CHECK(*alias == 0x5678);
	*alias = 0xabcdef;
	CHECK(*alias == 0xabcdef);
	kobox_boot_image_destroy(&image);
	CHECK(!kobox_posix_core_close(&core) && !core);
	CHECK(!kobox_posix_memory_window_destroy(&direct));
	CHECK(!kobox_posix_memory_backing_destroy(&backing));
	return 0;
}
