/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_ELF64_LOADER_H
#define KOBOX_ELF64_LOADER_H

#include <stddef.h>
#include <stdint.h>

struct kobox_elf64_module {
	void *mapping;
	size_t mapping_size;
};

#define KOBOX_ELF64_SYMBOL_FUNCTION 0u
#define KOBOX_ELF64_SYMBOL_OBJECT 1u

struct kobox_elf64_export {
	const char *name;
	uint32_t kind;
	uintptr_t address;
};

struct kobox_elf64_symbol {
	const char *name;
	uint32_t kind;
};

typedef int (*kobox_elf64_resolve_fn)(void *context, const char *name,
				     uintptr_t *address_out);

int kobox_elf64_module_load_fd(int file_descriptor,
			       struct kobox_elf64_export *exports,
			       size_t export_count,
			       kobox_elf64_resolve_fn resolve,
			       void *resolve_context,
			       struct kobox_elf64_module *module_out);
void kobox_elf64_module_unload(struct kobox_elf64_module *module);
int kobox_elf64_validate_export_set_fd(
	int file_descriptor, const struct kobox_elf64_symbol *expected,
	size_t expected_count);

#endif
