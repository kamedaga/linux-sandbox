// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "elf64_loader.h"

#include <elf.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct loaded_section {
	uint8_t *address;
	size_t size;
	int protection;
};

static int file_range_valid(size_t file_size, uint64_t offset, uint64_t length)
{
	return offset <= file_size && length <= file_size - (size_t)offset;
}

static int align_size(size_t value, size_t alignment, size_t *result_out)
{
	size_t mask;

	if (!alignment || (alignment & (alignment - 1)))
		return -1;
	mask = alignment - 1;
	if (value > SIZE_MAX - mask)
		return -1;
	*result_out = (value + mask) & ~mask;
	return 0;
}

static int add_signed_u64(uint64_t value, int64_t addend,
			  uint64_t *result_out)
{
	uint64_t magnitude;

	if (addend >= 0) {
		if (value > UINT64_MAX - (uint64_t)addend)
			return -1;
		*result_out = value + (uint64_t)addend;
		return 0;
	}
	magnitude = (uint64_t)(-(addend + 1)) + 1;
	if (value < magnitude)
		return -1;
	*result_out = value - magnitude;
	return 0;
}

static int symbol_name(const uint8_t *file, size_t file_size,
		       const Elf64_Shdr *sections, size_t section_count,
		       const Elf64_Shdr *symbol_table, const Elf64_Sym *symbol,
		       const char **name_out)
{
	const Elf64_Shdr *strings;
	const char *name;
	size_t remaining;

	if (symbol_table->sh_link >= section_count)
		return -1;
	strings = &sections[symbol_table->sh_link];
	if (strings->sh_type != SHT_STRTAB ||
	    !file_range_valid(file_size, strings->sh_offset, strings->sh_size) ||
	    symbol->st_name >= strings->sh_size)
		return -1;
	name = (const char *)file + strings->sh_offset + symbol->st_name;
	remaining = (size_t)strings->sh_size - symbol->st_name;
	if (!memchr(name, '\0', remaining))
		return -1;
	*name_out = name;
	return 0;
}

static int resolve_symbol(const uint8_t *file, size_t file_size,
			  const Elf64_Shdr *sections, size_t section_count,
			  const struct loaded_section *loaded,
			  const Elf64_Shdr *symbol_table,
			  const Elf64_Sym symbol[],
			  kobox_elf64_resolve_fn resolve, void *resolve_context,
			  uint64_t *value_out)
{
	const char *name;
	uintptr_t resolved;

	if (symbol->st_shndx == SHN_ABS) {
		*value_out = symbol->st_value;
		return 0;
	}
	if (symbol->st_shndx == SHN_UNDEF) {
		if (symbol_name(file, file_size, sections, section_count,
				symbol_table, symbol, &name))
			return -1;
		if (!resolve || resolve(resolve_context, name, &resolved))
			return -1;
		*value_out = (uint64_t)resolved;
		return 0;
	}
	if (symbol->st_shndx >= section_count ||
	    !loaded[symbol->st_shndx].address ||
	    symbol->st_value > loaded[symbol->st_shndx].size)
		return -1;
	*value_out = (uint64_t)(uintptr_t)loaded[symbol->st_shndx].address +
		     symbol->st_value;
	return 0;
}

static int write_relocation(uint8_t *location, uint32_t type,
			    uint64_t symbol, int64_t addend,
			    uint64_t place)
{
	uint64_t value;
	uint64_t difference;
	int64_t signed_value;

	if (add_signed_u64(symbol, addend, &value))
		return -1;
	switch (type) {
	case R_X86_64_NONE:
		return 0;
	case R_X86_64_64:
		memcpy(location, &value, sizeof(value));
		return 0;
	case R_X86_64_PC32:
	case R_X86_64_PLT32:
		if (value >= place) {
			difference = value - place;
			if (difference > INT32_MAX)
				return -1;
			signed_value = (int64_t)difference;
		} else {
			difference = place - value;
			if (difference > (uint64_t)INT32_MAX + 1)
				return -1;
			signed_value = -(int64_t)difference;
		}
		{
			int32_t word = (int32_t)signed_value;

			memcpy(location, &word, sizeof(word));
		}
		return 0;
	case R_X86_64_PC64:
		if (value >= place) {
			difference = value - place;
			if (difference > INT64_MAX)
				return -1;
			signed_value = (int64_t)difference;
		} else {
			difference = place - value;
			if (difference > (uint64_t)INT64_MAX + 1)
				return -1;
			signed_value = difference == (uint64_t)INT64_MAX + 1
					       ? INT64_MIN
					       : -(int64_t)difference;
		}
		memcpy(location, &signed_value, sizeof(signed_value));
		return 0;
	case R_X86_64_32:
		if (value > UINT32_MAX)
			return -1;
		{
			uint32_t word = (uint32_t)value;

			memcpy(location, &word, sizeof(word));
		}
		return 0;
	case R_X86_64_32S:
		if (value > INT32_MAX)
			return -1;
		{
			int32_t word = (int32_t)value;

			memcpy(location, &word, sizeof(word));
		}
		return 0;
	default:
		return -1;
	}
}

static size_t relocation_width(uint32_t type)
{
	switch (type) {
	case R_X86_64_NONE:
		return 0;
	case R_X86_64_64:
	case R_X86_64_PC64:
		return 8;
	case R_X86_64_PC32:
	case R_X86_64_PLT32:
	case R_X86_64_32:
	case R_X86_64_32S:
		return 4;
	default:
		return SIZE_MAX;
	}
}

static int apply_relocations(const uint8_t *file, size_t file_size,
			     const Elf64_Shdr *sections, size_t section_count,
			     struct loaded_section *loaded,
			     kobox_elf64_resolve_fn resolve,
			     void *resolve_context)
{
	size_t section_index;

	for (section_index = 0; section_index < section_count; section_index++) {
		const Elf64_Shdr *relocation_section = &sections[section_index];
		const Elf64_Shdr *symbol_table;
		const Elf64_Sym *symbols;
		const Elf64_Rela *relocations;
		size_t symbol_count;
		size_t relocation_count;
		size_t relocation_index;

		if (relocation_section->sh_type == SHT_REL &&
		    relocation_section->sh_info < section_count &&
		    loaded[relocation_section->sh_info].address)
			return -1;
		if (relocation_section->sh_type != SHT_RELA)
			continue;
		if (relocation_section->sh_info >= section_count ||
		    !loaded[relocation_section->sh_info].address)
			continue;
		if (relocation_section->sh_link >= section_count ||
		    relocation_section->sh_entsize != sizeof(Elf64_Rela) ||
		    relocation_section->sh_size % sizeof(Elf64_Rela) ||
		    !file_range_valid(file_size, relocation_section->sh_offset,
				      relocation_section->sh_size))
			return -1;
		symbol_table = &sections[relocation_section->sh_link];
		if (symbol_table->sh_type != SHT_SYMTAB ||
		    symbol_table->sh_entsize != sizeof(Elf64_Sym) ||
		    symbol_table->sh_size % sizeof(Elf64_Sym) ||
		    !file_range_valid(file_size, symbol_table->sh_offset,
				      symbol_table->sh_size))
			return -1;
		symbols = (const Elf64_Sym *)(file + symbol_table->sh_offset);
		symbol_count = symbol_table->sh_size / sizeof(*symbols);
		relocations = (const Elf64_Rela *)(file +
						 relocation_section->sh_offset);
		relocation_count = relocation_section->sh_size /
				   sizeof(*relocations);
		for (relocation_index = 0; relocation_index < relocation_count;
		     relocation_index++) {
			const Elf64_Rela *relocation = &relocations[relocation_index];
			struct loaded_section *target =
				&loaded[relocation_section->sh_info];
			uint32_t type = ELF64_R_TYPE(relocation->r_info);
			size_t width = relocation_width(type);
			uint64_t symbol;
			size_t symbol_index = ELF64_R_SYM(relocation->r_info);

			if (type == R_X86_64_NONE)
				continue;
			if (width == SIZE_MAX || relocation->r_offset > target->size ||
			    width > target->size - (size_t)relocation->r_offset ||
			    symbol_index >= symbol_count ||
			    resolve_symbol(file, file_size, sections, section_count,
					   loaded, symbol_table,
					   &symbols[symbol_index], resolve,
					   resolve_context, &symbol) ||
			    write_relocation(target->address + relocation->r_offset,
					     type, symbol, relocation->r_addend,
					     (uint64_t)(uintptr_t)(target->address +
								 relocation->r_offset)))
				return -1;
		}
	}
	return 0;
}

static int find_symbol_address(const uint8_t *file, size_t file_size,
			       const Elf64_Shdr *sections, size_t section_count,
			       const struct loaded_section *loaded,
			       const char *wanted, uint32_t wanted_kind,
			       uintptr_t *address_out)
{
	size_t section_index;

	for (section_index = 0; section_index < section_count; section_index++) {
		const Elf64_Shdr *symbol_table = &sections[section_index];
		const Elf64_Sym *symbols;
		size_t symbol_count;
		size_t symbol_index;

		if (symbol_table->sh_type != SHT_SYMTAB)
			continue;
		if (symbol_table->sh_entsize != sizeof(Elf64_Sym) ||
		    symbol_table->sh_size % sizeof(Elf64_Sym) ||
		    !file_range_valid(file_size, symbol_table->sh_offset,
				      symbol_table->sh_size))
			return -1;
		symbols = (const Elf64_Sym *)(file + symbol_table->sh_offset);
		symbol_count = symbol_table->sh_size / sizeof(*symbols);
		for (symbol_index = 0; symbol_index < symbol_count; symbol_index++) {
			const Elf64_Sym *symbol = &symbols[symbol_index];
			const char *name;
			uint64_t value;

			if (ELF64_ST_TYPE(symbol->st_info) !=
				    (wanted_kind == KOBOX_ELF64_SYMBOL_FUNCTION
					     ? STT_FUNC
					     : STT_OBJECT) ||
			    ELF64_ST_BIND(symbol->st_info) != STB_GLOBAL ||
			    symbol->st_shndx == SHN_UNDEF ||
			    symbol->st_shndx >= section_count ||
			    !loaded[symbol->st_shndx].address ||
			    symbol->st_value >= loaded[symbol->st_shndx].size ||
			    symbol->st_size > loaded[symbol->st_shndx].size -
					      symbol->st_value ||
			    symbol_name(file, file_size, sections, section_count,
					symbol_table, symbol, &name))
				continue;
			if (strcmp(name, wanted))
				continue;
			if (resolve_symbol(file, file_size, sections, section_count,
					   loaded, symbol_table, symbol, NULL, NULL,
					   &value))
				return -1;
			*address_out = (uintptr_t)value;
			return 0;
		}
	}
	return -1;
}

int kobox_elf64_module_load_fd(int file_descriptor,
			       struct kobox_elf64_export *exports,
			       size_t export_count,
			       kobox_elf64_resolve_fn resolve,
			       void *resolve_context,
			       struct kobox_elf64_module *module_out)
{
	const Elf64_Ehdr *header;
	const Elf64_Shdr *sections;
	struct loaded_section *loaded = NULL;
	struct stat status;
	uint8_t *file = MAP_FAILED;
	uint8_t *mapping = MAP_FAILED;
	size_t file_size;
	size_t mapping_size = 0;
	size_t page_size;
	size_t section_index;
	long native_page_size;
	int result = -1;

	if (file_descriptor < 0 || !exports || !export_count || !module_out)
		return -1;
	for (section_index = 0; section_index < export_count; section_index++) {
		if (!exports[section_index].name ||
		    exports[section_index].kind > KOBOX_ELF64_SYMBOL_OBJECT)
			return -1;
		exports[section_index].address = 0;
	}
	memset(module_out, 0, sizeof(*module_out));
	if (fstat(file_descriptor, &status) ||
	    !S_ISREG(status.st_mode) || status.st_size < (off_t)sizeof(*header))
		goto out;
	file_size = (size_t)status.st_size;
	if ((off_t)file_size != status.st_size)
		goto out;
	file = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
	if (file == MAP_FAILED)
		goto out;
	header = (const Elf64_Ehdr *)file;
	if (memcmp(header->e_ident, ELFMAG, SELFMAG) ||
	    header->e_ident[EI_CLASS] != ELFCLASS64 ||
	    header->e_ident[EI_DATA] != ELFDATA2LSB ||
	    header->e_ident[EI_VERSION] != EV_CURRENT ||
	    header->e_type != ET_REL || header->e_machine != EM_X86_64 ||
	    header->e_version != EV_CURRENT || header->e_ehsize != sizeof(*header) ||
	    header->e_phnum != 0 ||
	    header->e_shentsize != sizeof(Elf64_Shdr) || !header->e_shnum ||
	    header->e_shstrndx == SHN_XINDEX ||
	    (header->e_shstrndx != SHN_UNDEF &&
	     header->e_shstrndx >= header->e_shnum) ||
	    !file_range_valid(file_size, header->e_shoff,
			      (uint64_t)header->e_shnum * sizeof(Elf64_Shdr)))
		goto out;
	sections = (const Elf64_Shdr *)(file + header->e_shoff);
	loaded = calloc(header->e_shnum, sizeof(*loaded));
	if (!loaded)
		goto out;
	native_page_size = sysconf(_SC_PAGESIZE);
	if (native_page_size <= 0)
		goto out;
	page_size = (size_t)native_page_size;
	for (section_index = 0; section_index < header->e_shnum;
	     section_index++) {
		const Elf64_Shdr *section = &sections[section_index];
		size_t section_size;
		size_t next_size;

		if (!(section->sh_flags & SHF_ALLOC) || !section->sh_size)
			continue;
		if (section->sh_size > SIZE_MAX || section->sh_addralign > page_size ||
		    (section->sh_addralign &&
		     (section->sh_addralign & (section->sh_addralign - 1))) ||
		    align_size(mapping_size, page_size, &mapping_size) ||
		    align_size((size_t)section->sh_size, page_size, &section_size) ||
		    section_size > SIZE_MAX - mapping_size)
			goto out;
		loaded[section_index].size = (size_t)section->sh_size;
		loaded[section_index].protection = PROT_READ;
		if (section->sh_flags & SHF_WRITE)
			loaded[section_index].protection |= PROT_WRITE;
		if (section->sh_flags & SHF_EXECINSTR)
			loaded[section_index].protection |= PROT_EXEC;
		if ((loaded[section_index].protection &
		     (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
			goto out;
		next_size = mapping_size + section_size;
		loaded[section_index].address = (uint8_t *)(uintptr_t)mapping_size;
		mapping_size = next_size;
	}
	if (!mapping_size)
		goto out;
	mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		goto out;
	for (section_index = 0; section_index < header->e_shnum;
	     section_index++) {
		const Elf64_Shdr *section = &sections[section_index];
		uintptr_t offset = (uintptr_t)loaded[section_index].address;

		if (!loaded[section_index].size)
			continue;
		loaded[section_index].address = mapping + offset;
		if (section->sh_type != SHT_NOBITS) {
			if (!file_range_valid(file_size, section->sh_offset,
					      section->sh_size))
				goto out;
			memcpy(loaded[section_index].address,
			       file + section->sh_offset, loaded[section_index].size);
		}
	}
	if (apply_relocations(file, file_size, sections, header->e_shnum,
			      loaded, resolve, resolve_context))
		goto out;
	for (section_index = 0; section_index < export_count; section_index++) {
		if (find_symbol_address(file, file_size, sections, header->e_shnum,
					loaded, exports[section_index].name,
					exports[section_index].kind,
					&exports[section_index].address))
			goto out;
	}
	if (mprotect(mapping, mapping_size, PROT_NONE))
		goto out;
	for (section_index = 0; section_index < header->e_shnum;
	     section_index++) {
		size_t protected_size;

		if (!loaded[section_index].size)
			continue;
		if (align_size(loaded[section_index].size, page_size,
			       &protected_size))
			goto out;
		if (mprotect(loaded[section_index].address, protected_size,
			     loaded[section_index].protection))
			goto out;
	}
	__builtin___clear_cache((char *)mapping, (char *)mapping + mapping_size);
	module_out->mapping = mapping;
	module_out->mapping_size = mapping_size;
	mapping = MAP_FAILED;
	result = 0;

out:
	if (mapping != MAP_FAILED)
		munmap(mapping, mapping_size);
	if (file != MAP_FAILED)
		munmap(file, file_size);
	free(loaded);
	if (result)
		memset(module_out, 0, sizeof(*module_out));
	if (result) {
		for (section_index = 0; section_index < export_count;
		     section_index++)
			exports[section_index].address = 0;
	}
	return result;
}

void kobox_elf64_module_unload(struct kobox_elf64_module *module)
{
	if (!module)
		return;
	if (module->mapping)
		munmap(module->mapping, module->mapping_size);
	memset(module, 0, sizeof(*module));
}

int kobox_elf64_validate_export_set_fd(
	int file_descriptor, const struct kobox_elf64_symbol *expected,
	size_t expected_count)
{
	const Elf64_Ehdr *header;
	const Elf64_Shdr *sections;
	struct stat status;
	uint8_t *file = MAP_FAILED;
	uint8_t *matched = NULL;
	size_t file_size = 0;
	size_t actual_count = 0;
	size_t section_index;
	uint32_t table_type;
	int result = -1;

	if (file_descriptor < 0 || !expected || !expected_count)
		return -1;
	for (section_index = 0; section_index < expected_count; section_index++) {
		if (!expected[section_index].name || !expected[section_index].name[0] ||
		    expected[section_index].kind > KOBOX_ELF64_SYMBOL_OBJECT)
			return -1;
	}
	if (fstat(file_descriptor, &status) ||
	    status.st_size < (off_t)sizeof(Elf64_Ehdr) ||
	    (uint64_t)status.st_size > SIZE_MAX)
		return -1;
	file_size = (size_t)status.st_size;
	file = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
	if (file == MAP_FAILED)
		goto out;
	header = (const Elf64_Ehdr *)file;
	if (memcmp(header->e_ident, ELFMAG, SELFMAG) ||
	    header->e_ident[EI_CLASS] != ELFCLASS64 ||
	    header->e_ident[EI_DATA] != ELFDATA2LSB ||
	    header->e_machine != EM_X86_64 ||
	    (header->e_type != ET_REL && header->e_type != ET_DYN) ||
	    header->e_shentsize != sizeof(Elf64_Shdr) || !header->e_shnum ||
	    !file_range_valid(file_size, header->e_shoff,
			      (uint64_t)header->e_shnum * sizeof(Elf64_Shdr)))
		goto out;
	sections = (const Elf64_Shdr *)(file + header->e_shoff);
	table_type = header->e_type == ET_REL ? SHT_SYMTAB : SHT_DYNSYM;
	matched = calloc(expected_count, sizeof(*matched));
	if (!matched)
		goto out;
	for (section_index = 0; section_index < header->e_shnum;
	     section_index++) {
		const Elf64_Shdr *table = &sections[section_index];
		const Elf64_Sym *symbols;
		size_t symbol_count;
		size_t symbol_index;

		if (table->sh_type != table_type)
			continue;
		if (table->sh_entsize != sizeof(Elf64_Sym) ||
		    table->sh_size % sizeof(Elf64_Sym) ||
		    !file_range_valid(file_size, table->sh_offset, table->sh_size))
			goto out;
		symbols = (const Elf64_Sym *)(file + table->sh_offset);
		symbol_count = table->sh_size / sizeof(*symbols);
		for (symbol_index = 0; symbol_index < symbol_count;
		     symbol_index++) {
			const Elf64_Sym *symbol = &symbols[symbol_index];
			unsigned int binding = ELF64_ST_BIND(symbol->st_info);
			unsigned int type = ELF64_ST_TYPE(symbol->st_info);
			unsigned int visibility = ELF64_ST_VISIBILITY(symbol->st_other);
			uint32_t kind;
			const char *name;
			size_t expected_index;

			if ((binding != STB_GLOBAL && binding != STB_WEAK) ||
			    symbol->st_shndx == SHN_UNDEF ||
			    (visibility != STV_DEFAULT &&
			     visibility != STV_PROTECTED) ||
			    (type != STT_FUNC && type != STT_OBJECT))
				continue;
			if (symbol_name(file, file_size, sections, header->e_shnum,
					table, symbol, &name) || !name[0])
				goto out;
			kind = type == STT_FUNC ? KOBOX_ELF64_SYMBOL_FUNCTION :
						 KOBOX_ELF64_SYMBOL_OBJECT;
			for (expected_index = 0; expected_index < expected_count;
			     expected_index++) {
				if (expected[expected_index].kind == kind &&
				    !strcmp(expected[expected_index].name, name))
					break;
			}
			if (expected_index == expected_count ||
			    matched[expected_index])
				goto out;
			matched[expected_index] = 1;
			actual_count++;
		}
	}
	if (actual_count != expected_count)
		goto out;
	for (section_index = 0; section_index < expected_count; section_index++) {
		if (!matched[section_index])
			goto out;
	}
	result = 0;

out:
	free(matched);
	if (file != MAP_FAILED)
		munmap(file, file_size);
	return result;
}
