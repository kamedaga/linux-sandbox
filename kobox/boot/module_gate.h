/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_MODULE_GATE_H
#define KOBOX_LINUX_MODULE_GATE_H

#include "../mm/host.h"
#include "../gem/lifetime_test.h"

#define KOBOX_GEM_MODULES 4

enum kobox_linux_module_access {
	KOBOX_MODULE_READ,
	KOBOX_MODULE_WRITE,
	KOBOX_MODULE_EXECUTE,
};

enum kobox_linux_module_access_result {
	KOBOX_MODULE_ACCESS_FAILED = -1,
	KOBOX_MODULE_ACCESS_ALLOWED,
	KOBOX_MODULE_ACCESS_DENIED,
};

/* Borrowed immutable ELF bytes in this process, not a wire ABI. */
struct kobox_linux_module_image {
	const void *data;
	size_t length;
};

struct kobox_linux_module_test {
	size_t size;
	struct kobox_linux_module_image images[KOBOX_GEM_MODULES];
	struct kobox_linux_module_image lifetime_image;
	const struct kobox_linux_vm_test *vm;
	enum kobox_gem_final_owner gem_final_owner;
	uint32_t allocation_failures;
	uint32_t buffer_cleanup;
	/* Test-only isolated real access to the inherited host mapping. */
	int (*access)(void *address, enum kobox_linux_module_access operation);
};

struct kobox_linux_module_report {
	size_t size;
	uint64_t warnings;
	uint32_t exports, loaded, unloaded, phase;
	uint32_t permissions;
	struct kobox_gem_lifetime_report gem;
	int32_t result;
	char diagnostics[8192];
};

#endif
