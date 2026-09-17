// SPDX-License-Identifier: GPL-2.0-only
#include "fixed_image.h"
#include "image_layout.h"

#include <elf.h>
#include <string.h>

static int range_valid(size_t total, size_t offset, size_t size)
{
	return offset <= total && size <= total - offset;
}

static int table_valid(size_t total, size_t offset, size_t count,
		       size_t entry_size)
{
	return !entry_size || (count <= SIZE_MAX / entry_size &&
		range_valid(total, offset, count * entry_size));
}

static int power_of_two(size_t value)
{
	return value && !(value & (value - 1));
}

static int symbol_name_equal(const char *candidate, size_t available,
			     const char *name)
{
	size_t i;

	for (i = 0; i < available; i++) {
		if (candidate[i] != *name)
			return 0;
		if (!candidate[i])
			return 1;
		name++;
	}
	return 0;
}

static size_t round_up(size_t value, size_t alignment, int *valid)
{
	if (value > SIZE_MAX - (alignment - 1)) {
		*valid = 0;
		return 0;
	}
	return (value + alignment - 1) & ~(alignment - 1);
}

static int symbol_in_segment(const struct kobox_fixed_image *image,
			     uintptr_t value, size_t size)
{
	unsigned int i;

	if (value < image->link_base)
		return 0;
	for (i = 0; i < image->segment_count; i++) {
		const struct kobox_fixed_image_segment *segment =
			&image->segments[i];
		size_t offset = (size_t)(value - image->link_base);

		if (offset < segment->image_offset ||
		    offset > segment->image_offset + segment->memory_size)
			continue;
		return size <= segment->memory_size -
			(offset - segment->image_offset);
	}
	return 0;
}

enum kobox_fixed_image_result kobox_fixed_image_open(
	const void *file, size_t file_size, size_t page_size,
	struct kobox_fixed_image *image)
{
	const unsigned char expected[EI_NIDENT] = {
		ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3,
		ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_SYSV,
	};
	const Elf64_Ehdr *header;
	const Elf64_Phdr *programs;
	const Elf64_Shdr *sections;
	const Elf64_Shdr *symbol_section = NULL;
	struct kobox_fixed_image parsed = {0};
	size_t previous_end = 0;
	unsigned int i;
	int valid = 1;

	if (!file || !image || !power_of_two(page_size) ||
	    file_size < sizeof(*header))
		return KOBOX_FIXED_IMAGE_INVALID;
	header = file;
	if (memcmp(header->e_ident, expected, sizeof(expected)) ||
	    header->e_ident[EI_ABIVERSION] != 0 || header->e_type != ET_EXEC ||
	    header->e_machine != EM_X86_64 || header->e_version != EV_CURRENT ||
	    header->e_ehsize != sizeof(*header) ||
	    header->e_phentsize != sizeof(Elf64_Phdr) || !header->e_phnum ||
	    header->e_shentsize != sizeof(Elf64_Shdr) || !header->e_shnum ||
	    !table_valid(file_size, header->e_phoff, header->e_phnum,
			 sizeof(Elf64_Phdr)) ||
	    !table_valid(file_size, header->e_shoff, header->e_shnum,
			 sizeof(Elf64_Shdr)))
		return KOBOX_FIXED_IMAGE_INVALID;
	programs = (const void *)((const unsigned char *)file + header->e_phoff);
	sections = (const void *)((const unsigned char *)file + header->e_shoff);
	parsed.file = file;
	parsed.file_size = file_size;
	parsed.link_base = KOBOX_CORE_LINK_BASE;
	parsed.page_size = page_size;
	for (i = 0; i < header->e_phnum; i++) {
		const Elf64_Phdr *program = &programs[i];
		struct kobox_fixed_image_segment *segment;
		size_t offset, rounded;

		if (program->p_type == PT_INTERP || program->p_type == PT_DYNAMIC ||
		    program->p_type == PT_TLS)
			return KOBOX_FIXED_IMAGE_INVALID;
		if (program->p_type != PT_LOAD || !program->p_memsz)
			continue;
		if (parsed.segment_count == KOBOX_FIXED_IMAGE_MAX_SEGMENTS)
			return KOBOX_FIXED_IMAGE_TOO_MANY_SEGMENTS;
		if (program->p_vaddr < parsed.link_base ||
		    program->p_vaddr - parsed.link_base > SIZE_MAX ||
		    program->p_filesz > program->p_memsz ||
		    !range_valid(file_size, program->p_offset, program->p_filesz) ||
		    program->p_offset % page_size != program->p_vaddr % page_size ||
		    !(program->p_flags & PF_R) ||
		    (program->p_flags & (PF_W | PF_X)) == (PF_W | PF_X))
			return KOBOX_FIXED_IMAGE_INVALID;
		offset = (size_t)(program->p_vaddr - parsed.link_base);
		if (offset % page_size || program->p_offset % page_size)
			return KOBOX_FIXED_IMAGE_INVALID;
		rounded = round_up(program->p_memsz, page_size, &valid);
		if (!valid || offset < previous_end || offset > SIZE_MAX - rounded)
			return KOBOX_FIXED_IMAGE_INVALID;
		segment = &parsed.segments[parsed.segment_count++];
		segment->file_offset = program->p_offset;
		segment->image_offset = offset;
		segment->file_size = program->p_filesz;
		segment->memory_size = program->p_memsz;
		segment->mapping_size = rounded;
		segment->protection = KOBOX_FIXED_IMAGE_READ;
		if (program->p_flags & PF_W)
			segment->protection |= KOBOX_FIXED_IMAGE_WRITE;
		if (program->p_flags & PF_X)
			segment->protection |= KOBOX_FIXED_IMAGE_EXECUTE;
		previous_end = offset + rounded;
	}
	if (!parsed.segment_count || parsed.segments[0].image_offset ||
	    parsed.segments[0].file_offset ||
	    parsed.segments[0].file_size < sizeof(*header))
		return KOBOX_FIXED_IMAGE_INVALID;
	parsed.image_size = previous_end;
	for (i = 0; i < header->e_shnum; i++) {
		const Elf64_Shdr *section = &sections[i];

		if (section->sh_type != SHT_NOBITS &&
		    !range_valid(file_size, section->sh_offset, section->sh_size))
			return KOBOX_FIXED_IMAGE_INVALID;
		if (section->sh_type != SHT_SYMTAB)
			continue;
		if (symbol_section || section->sh_entsize != sizeof(Elf64_Sym) ||
		    section->sh_size % sizeof(Elf64_Sym) ||
		    section->sh_link >= header->e_shnum)
			return KOBOX_FIXED_IMAGE_INVALID;
		symbol_section = section;
	}
	if (!symbol_section)
		return KOBOX_FIXED_IMAGE_MISSING_SYMBOLS;
	{
		const Elf64_Shdr *strings = &sections[symbol_section->sh_link];

		if (strings->sh_type != SHT_STRTAB || !strings->sh_size ||
		    !range_valid(file_size, strings->sh_offset, strings->sh_size))
			return KOBOX_FIXED_IMAGE_INVALID;
		parsed.symbols = (const unsigned char *)file + symbol_section->sh_offset;
		parsed.symbol_count = symbol_section->sh_size / sizeof(Elf64_Sym);
		parsed.strings = (const char *)file + strings->sh_offset;
		parsed.strings_size = strings->sh_size;
		if (parsed.strings[parsed.strings_size - 1] != '\0')
			return KOBOX_FIXED_IMAGE_INVALID;
	}
	*image = parsed;
	return KOBOX_FIXED_IMAGE_OK;
}

enum kobox_fixed_image_result kobox_fixed_image_symbol(
	const struct kobox_fixed_image *image, const char *name,
	size_t *image_offset)
{
	const Elf64_Sym *symbols;
	size_t i;

	if (!image || !image->file || !name || !name[0] || !image_offset ||
	    !image->symbols || !image->strings)
		return KOBOX_FIXED_IMAGE_INVALID;
	symbols = image->symbols;
	for (i = 0; i < image->symbol_count; i++) {
		const Elf64_Sym *symbol = &symbols[i];
		const char *candidate;
		size_t remaining;

		if (symbol->st_shndx == SHN_UNDEF ||
		    symbol->st_name >= image->strings_size ||
		    (ELF64_ST_BIND(symbol->st_info) != STB_GLOBAL &&
		     ELF64_ST_BIND(symbol->st_info) != STB_WEAK))
			continue;
		candidate = image->strings + symbol->st_name;
		remaining = image->strings_size - symbol->st_name;
		if (!symbol_name_equal(candidate, remaining, name))
			continue;
		if (!symbol_in_segment(image, symbol->st_value, symbol->st_size))
			return KOBOX_FIXED_IMAGE_INVALID;
		*image_offset = (size_t)(symbol->st_value - image->link_base);
		return KOBOX_FIXED_IMAGE_OK;
	}
	return KOBOX_FIXED_IMAGE_MISSING_SYMBOL;
}
