// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "host.h"
#include "../host/posix/host.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_RAM_SIZE (256UL << 20)
#define TEST_VMEMMAP_SIZE (16UL << 20)
#define TEST_VMALLOC_SIZE (256UL << 20)
#define TEST_KERNEL_PHYSICAL_BASE (16UL << 20)

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "check failed at line %d: %s\n", \
			__LINE__, #expression); \
		return 1; \
	} \
} while (0)

static int host_map(void *window, size_t window_offset, void *backing,
		    size_t backing_offset, size_t size,
		    unsigned int protection, void **address_out)
{
	unsigned int native = 0;

	if (protection & KOBOX_LINUX_MEMORY_READ)
		native |= KOBOX_POSIX_MEMORY_READ;
	if (protection & KOBOX_LINUX_MEMORY_WRITE)
		native |= KOBOX_POSIX_MEMORY_WRITE;
	return kobox_posix_memory_window_map(window, window_offset, backing,
		backing_offset, size, native, address_out);
}

static int host_reset(void *window, size_t window_offset, size_t size)
{
	return kobox_posix_memory_window_reset(window, window_offset, size);
}

static int load_entry(void *handle, kobox_linux_memory_boot_fn *entry_out)
{
	void *address;

	dlerror();
	address = dlsym(handle, "kobox_linux_memory_early_boot");
	if (dlerror() || !address)
		return -1;
	memcpy(entry_out, &address, sizeof(address));
	return 0;
}

static void crash_handler(int signal_number)
{
	void *frames[32];
	int count = backtrace(frames, sizeof(frames) / sizeof(frames[0]));

	backtrace_symbols_fd(frames, count, STDERR_FILENO);
	_exit(128 + signal_number);
}

static int install_crash_handlers(void)
{
	struct sigaction action = {
		.sa_handler = crash_handler,
	};

	sigemptyset(&action.sa_mask);
	return sigaction(SIGBUS, &action, NULL) ||
		sigaction(SIGILL, &action, NULL) ||
		sigaction(SIGSEGV, &action, NULL);
}

static int check_phase_boundary(void *handle)
{
	typedef void (*boundary_fn)(void);
	boundary_fn boundary;
	void *address;
	pid_t child;
	int status;

	dlerror();
	address = dlsym(handle, "try_to_free_pages");
	if (dlerror() || !address)
		return -1;
	memcpy(&boundary, &address, sizeof(boundary));
	child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		boundary();
		_exit(125);
	}
	if (waitpid(child, &status, 0) != child)
		return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == 126 ? 0 : -1;
}

int main(int argument_count, char **arguments)
{
	static const struct kobox_linux_memory_host_operations operations = {
		.size = sizeof(operations),
		.identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
		.map = host_map,
		.reset = host_reset,
	};
	struct kobox_posix_memory_backing backing = {0};
	struct kobox_posix_memory_window direct = {0};
	struct kobox_posix_memory_window vmemmap = {0};
	struct kobox_posix_memory_window vmalloc = {0};
	struct kobox_linux_memory_layout layout;
	struct kobox_linux_memory_report report = {
		.size = sizeof(report),
		.identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
	};
	kobox_linux_memory_boot_fn entry;
	void *boundary_handle;
	void *direct_address;
	void *handle;

	CHECK(argument_count == 3);
	CHECK(install_crash_handlers() == 0);
	CHECK(kobox_posix_memory_backing_init(&backing, TEST_RAM_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&direct, TEST_RAM_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&vmemmap, TEST_VMEMMAP_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&vmalloc, TEST_VMALLOC_SIZE) == 0);
	CHECK(kobox_posix_memory_window_map(
		&direct, 0, &backing, 0, TEST_RAM_SIZE,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE,
		&direct_address) == 0);

	boundary_handle = dlopen(arguments[1], RTLD_NOW | RTLD_GLOBAL);
	if (!boundary_handle) {
		fprintf(stderr, "cannot load memory phase boundary: %s\n",
			dlerror());
		return 1;
	}
	CHECK(check_phase_boundary(boundary_handle) == 0);
	handle = dlopen(arguments[2], RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		fprintf(stderr, "cannot load Linux memory core: %s\n", dlerror());
		return 1;
	}
	CHECK(load_entry(handle, &entry) == 0);
	layout = (struct kobox_linux_memory_layout) {
		.size = sizeof(layout),
		.identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
		.operations = &operations,
		.ram_backing = &backing,
		.vmemmap_window = &vmemmap,
		.vmalloc_window = &vmalloc,
		.direct_map = direct_address,
		.ram_size = TEST_RAM_SIZE,
		.vmemmap_base = vmemmap.address,
		.vmemmap_size = vmemmap.size,
		.vmalloc_base = vmalloc.address,
		.vmalloc_size = vmalloc.size,
		.kernel_image_physical_base = TEST_KERNEL_PHYSICAL_BASE,
	};
	CHECK(entry(&layout, &report) == 0);
	CHECK(report.mm_core_initialized);
	CHECK(report.page_pfn_roundtrip);
	CHECK(report.direct_map_roundtrip);
	CHECK(report.kmalloc_ready);
	CHECK(report.slub_ready);
	CHECK(report.static_percpu_ready);
	CHECK(report.dynamic_percpu_ready);
	CHECK(report.vmap_alias_ready);
	CHECK(report.kernel_image_translation_ready);
	CHECK(report.early_cpuhp_registrations_ready);
	CHECK(report.early_cpuhp_registration_count >= 2);
	CHECK(report.logical_cpu_count == KOBOX_LINUX_MEMORY_LOGICAL_CPUS);
	CHECK(report.ram_size == TEST_RAM_SIZE);
	CHECK(report.page_offset_base == (uintptr_t)direct.address);
	CHECK(report.vmemmap_base == (uintptr_t)vmemmap.address);
	CHECK(report.vmalloc_base == (uintptr_t)vmalloc.address);
	return 0;
}
