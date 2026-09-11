// SPDX-License-Identifier: GPL-2.0-only
#include "elf.h"

#include <elf.h>
#include <string.h>

bool kobox_x86_64_elf_matches(const void *data, size_t size, bool core)
{
	Elf64_Ehdr header;

	if (!data || size < sizeof(header))
		return false;
	memcpy(&header, data, sizeof(header));
	/* Complete relocation validation remains with the actual loaders. */
	return !memcmp(header.e_ident, ELFMAG, SELFMAG) &&
	       header.e_ident[EI_CLASS] == ELFCLASS64 &&
	       header.e_ident[EI_DATA] == ELFDATA2LSB &&
	       header.e_ident[EI_VERSION] == EV_CURRENT &&
	       header.e_version == EV_CURRENT && header.e_machine == EM_X86_64 &&
	       header.e_ehsize == sizeof(header) &&
	       header.e_type == (core ? ET_DYN : ET_REL);
}
