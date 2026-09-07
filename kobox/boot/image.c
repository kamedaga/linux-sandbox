// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "image.h"

#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct image_mapping {
	uintptr_t address;
	size_t offset;
	size_t size;
	unsigned int protection;
};

struct image_layout {
	uintptr_t base;
	size_t page_size;
	size_t capacity;
	uintptr_t ram_start;
	uintptr_t ram_end;
	size_t physical_base;
	size_t image_size;
	struct image_mapping *mappings;
	unsigned int count;
	int status;
	bool found;
};

static int collect_image(struct dl_phdr_info *info, size_t size, void *argument)
{
	struct image_layout *layout = argument;
	size_t previous_end = 0;
	unsigned int index;

	(void)size;
	if (info->dlpi_addr != layout->base)
		return 0;
	layout->found = true;
	layout->mappings = calloc(info->dlpi_phnum, sizeof(*layout->mappings));
	if (!layout->mappings) {
		layout->status = ENOMEM;
		return 1;
	}
	for (index = 0; index < info->dlpi_phnum; index++) {
		const ElfW(Phdr) *header = &info->dlpi_phdr[index];
		struct image_mapping *mapping;
		size_t rounded;

		if (header->p_type != PT_LOAD || !header->p_memsz)
			continue;
		if (header->p_vaddr % layout->page_size ||
		    header->p_vaddr < previous_end ||
		    header->p_filesz > header->p_memsz ||
		    header->p_memsz > SIZE_MAX - (layout->page_size - 1) ||
		    (header->p_flags & (PF_W | PF_X)) == (PF_W | PF_X) ||
		    !(header->p_flags & PF_R)) {
			layout->status = ENOEXEC;
			return 1;
		}
		rounded = (header->p_memsz + layout->page_size - 1) &
			  ~(layout->page_size - 1);
		if (header->p_vaddr > layout->capacity ||
		    rounded > layout->capacity - header->p_vaddr ||
		    layout->base > UINTPTR_MAX - header->p_vaddr ||
		    rounded > UINTPTR_MAX - (layout->base + header->p_vaddr)) {
			layout->status = EOVERFLOW;
			return 1;
		}
		mapping = &layout->mappings[layout->count++];
		mapping->address = layout->base + header->p_vaddr;
		mapping->offset = header->p_vaddr;
		mapping->size = rounded;
		if (mapping->address < layout->ram_end &&
		    mapping->address + rounded > layout->ram_start) {
			layout->status = EINVAL;
			return 1;
		}
		mapping->protection = KOBOX_POSIX_MEMORY_READ;
		if (header->p_flags & PF_W)
			mapping->protection |= KOBOX_POSIX_MEMORY_WRITE;
		if (header->p_flags & PF_X)
			mapping->protection |= KOBOX_POSIX_MEMORY_EXECUTE;
		previous_end = header->p_vaddr + rounded;
	}
	layout->image_size = previous_end;
	if (!layout->count || layout->mappings[0].offset)
		layout->status = ENOEXEC;
	return 1;
}

int kobox_boot_image_alias(void *library,
			   struct kobox_posix_memory_backing *backing,
			   void *direct_map, size_t physical_base,
			   struct kobox_boot_image *image)
{
	struct kobox_posix_memory_window probe = {0};
	struct image_layout layout = {0};
	struct image_layout *saved = NULL;
	struct link_map *map;
	unsigned int index;
	void *address;
	long page_size;
	int status;

	page_size = sysconf(_SC_PAGESIZE);
	if (!library || !backing || !backing->initialized || !direct_map ||
	    !image || image->layout || page_size <= 0 ||
	    (page_size & (page_size - 1)) ||
	    (uintptr_t)direct_map % page_size ||
	    backing->size > UINTPTR_MAX - (uintptr_t)direct_map ||
	    physical_base % page_size || physical_base >= backing->size)
		return EINVAL;
	if (dlinfo(library, RTLD_DI_LINKMAP, &map) || !map->l_addr ||
	    !map->l_name || !map->l_name[0])
		return ENOEXEC;
	layout.base = map->l_addr;
	layout.page_size = page_size;
	layout.capacity = backing->size - physical_base;
	layout.ram_start = (uintptr_t)direct_map;
	layout.ram_end = layout.ram_start + backing->size;
	layout.physical_base = physical_base;
	dl_iterate_phdr(collect_image, &layout);
	status = layout.found ? layout.status : ENOEXEC;
	if (status)
		goto out;
	saved = malloc(sizeof(*saved));
	if (!saved) {
		status = ENOMEM;
		goto out;
	}
	/* Test executable backing before changing any owned image mapping. */
	status = kobox_posix_memory_window_init(&probe, page_size);
	if (status)
		goto out;
	status = kobox_posix_memory_window_map(
		&probe, 0, backing, physical_base, page_size,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_EXECUTE, &address);
	if (status)
		goto out_probe;
	/* Preserve relocated bytes, including the loader's own ELF tables. */
	for (index = 0; index < layout.count; index++) {
		struct image_mapping *mapping = &layout.mappings[index];

		memcpy((char *)direct_map + physical_base + mapping->offset,
		       (void *)mapping->address, mapping->size);
	}
	for (index = 0; index < layout.count; index++) {
		struct image_mapping *mapping = &layout.mappings[index];
		struct kobox_posix_memory_window owned = {
			.address = (void *)mapping->address,
			.size = mapping->size,
			.initialized = true,
		};

		status = kobox_posix_memory_window_map(
			&owned, 0, backing, physical_base + mapping->offset,
			mapping->size, mapping->protection, &address);
		if (status)
			goto out_probe;
	}
out_probe:
	{
		int destroyed = kobox_posix_memory_window_destroy(&probe);

		if (!status)
			status = destroyed;
	}
out:
	if (!status) {
		*saved = layout;
		image->layout = saved;
		image->size = layout.image_size;
		return 0;
	}
	free(saved);
	free(layout.mappings);
	return status;
}

int kobox_boot_image_protect(void *argument, size_t offset, size_t length,
			     unsigned int protection)
{
	struct kobox_boot_image *image = argument;
	struct image_layout *layout;
	size_t end, covered;
	unsigned int index;
	int native = PROT_NONE;

	if (!image || !image->layout || !length ||
	    protection & ~(KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE |
			   KOBOX_IMAGE_EXECUTE) ||
	    (protection & (KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE)) ==
	    (KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE))
		return EINVAL;
	layout = image->layout;
	if (offset % layout->page_size || length % layout->page_size ||
	    offset >= layout->image_size || length > layout->image_size - offset)
		return EINVAL;
	end = offset + length;
	covered = offset;
	for (index = 0; index < layout->count && covered < end; index++) {
		const struct image_mapping *mapping = &layout->mappings[index];
		size_t mapping_end = mapping->offset + mapping->size;

		if (mapping_end <= covered)
			continue;
		if (mapping->offset > covered)
			return EINVAL;
		covered = mapping_end < end ? mapping_end : end;
	}
	if (covered != end)
		return EINVAL;
	if (protection & KOBOX_IMAGE_READ)
		native |= PROT_READ;
	if (protection & KOBOX_IMAGE_WRITE)
		native |= PROT_WRITE;
	if (protection & KOBOX_IMAGE_EXECUTE)
		native |= PROT_EXEC;
	if (mprotect((void *)(layout->base + offset), length, native))
		return errno;
	/* The direct alias must not defeat read-only publication. Revoking an
	 * init image mapping instead leaves RAM writable for Linux buddy reuse.
	 */
	if (mprotect((void *)(layout->ram_start + layout->physical_base + offset),
		     length, protection ? native & ~PROT_EXEC : PROT_READ | PROT_WRITE))
		return errno;
	return 0;
}

void kobox_boot_image_destroy(struct kobox_boot_image *image)
{
	struct image_layout *layout;

	if (!image || !image->layout)
		return;
	layout = image->layout;
	free(layout->mappings);
	free(layout);
	memset(image, 0, sizeof(*image));
}
