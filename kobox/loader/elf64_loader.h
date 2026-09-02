/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_ELF64_LOADER_H
#define KOBOX_ELF64_LOADER_H

#include <stddef.h>
#include <stdint.h>

struct kobox_elf64_module {
	void *mapping;
	size_t mapping_size;
	uintptr_t init_address;
	uintptr_t exit_address;
};

int kobox_elf64_module_load(const char *path,
			    struct kobox_elf64_module *module_out);
void kobox_elf64_module_unload(struct kobox_elf64_module *module);

#endif
