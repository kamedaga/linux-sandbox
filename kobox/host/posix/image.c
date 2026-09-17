// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "image.h"
#include "../../boot/image_layout.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct image_layout {
	uintptr_t base;
	uintptr_t ram_start;
	size_t physical_base;
	const struct kobox_fixed_image *image;
};

static unsigned int memory_protection(unsigned int protection)
{
	unsigned int native = 0;

	if (protection & KOBOX_FIXED_IMAGE_READ)
		native |= KOBOX_POSIX_MEMORY_READ;
	if (protection & KOBOX_FIXED_IMAGE_WRITE)
		native |= KOBOX_POSIX_MEMORY_WRITE;
	if (protection & KOBOX_FIXED_IMAGE_EXECUTE)
		native |= KOBOX_POSIX_MEMORY_EXECUTE;
	return native;
}

int kobox_boot_image_alias(struct kobox_posix_core *core,
			   struct kobox_posix_memory_backing *backing,
			   void *direct_map, size_t physical_base,
			   struct kobox_boot_image *boot_image)
{
	struct kobox_posix_memory_window probe = {0};
	const struct kobox_fixed_image *image;
	struct image_layout *layout;
	uintptr_t ram_start, ram_end, base;
	unsigned int index;
	void *address;
	int status;

	image = kobox_posix_core_image(core);
	base = (uintptr_t)kobox_posix_core_base(core);
	if (!image || !base || !backing || !backing->initialized || !direct_map ||
	    !boot_image || boot_image->layout ||
	    physical_base != KOBOX_CORE_PHYSICAL_BASE ||
	    (uintptr_t)direct_map % image->page_size ||
	    backing->size > UINTPTR_MAX - (uintptr_t)direct_map ||
	    physical_base >= backing->size ||
	    image->image_size > backing->size - physical_base)
		return EINVAL;
	ram_start = (uintptr_t)direct_map;
	ram_end = ram_start + backing->size;
	if (base > UINTPTR_MAX - image->image_size ||
	    (base < ram_end && base + image->image_size > ram_start))
		return EINVAL;
	layout = calloc(1, sizeof(*layout));
	if (!layout)
		return ENOMEM;
	layout->base = base;
	layout->ram_start = ram_start;
	layout->physical_base = physical_base;
	layout->image = image;

	/* Refuse a backing that cannot be executed before replacing any core
	 * mapping. This keeps failure atomic for the common policy check. */
	status = kobox_posix_memory_window_init(&probe, image->page_size);
	if (status)
		goto fail;
	status = kobox_posix_memory_window_map(
		&probe, 0, backing, physical_base, image->page_size,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_EXECUTE, &address);
	if (status)
		goto destroy_probe;
	for (index = 0; index < image->segment_count; index++) {
		const struct kobox_fixed_image_segment *segment =
			&image->segments[index];

		memcpy((unsigned char *)direct_map + physical_base +
		       segment->image_offset,
		       (const unsigned char *)base + segment->image_offset,
		       segment->mapping_size);
	}
	for (index = 0; index < image->segment_count; index++) {
		const struct kobox_fixed_image_segment *segment =
			&image->segments[index];
		struct kobox_posix_memory_window owned = {
			.address = (void *)(base + segment->image_offset),
			.size = segment->mapping_size,
			.initialized = true,
		};

		status = kobox_posix_memory_window_map(
			&owned, 0, backing,
			physical_base + segment->image_offset,
			segment->mapping_size,
			memory_protection(segment->protection), &address);
		if (status)
			goto destroy_probe;
	}
destroy_probe:
	{
		int destroyed = kobox_posix_memory_window_destroy(&probe);

		if (!status)
			status = destroyed;
	}
	if (status)
		goto fail;
	boot_image->layout = layout;
	boot_image->size = image->image_size;
	return 0;
fail:
	free(layout);
	return status;
}

int kobox_boot_image_protect(void *argument, size_t offset, size_t length,
			     unsigned int protection)
{
	struct kobox_boot_image *boot_image = argument;
	struct image_layout *layout;
	size_t end, covered;
	unsigned int index;
	int native = PROT_NONE;

	if (!boot_image || !boot_image->layout || !length ||
	    protection & ~(KOBOX_IMAGE_READ | KOBOX_IMAGE_WRITE |
			   KOBOX_IMAGE_EXECUTE) ||
	    (protection & (KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE)) ==
	    (KOBOX_IMAGE_WRITE | KOBOX_IMAGE_EXECUTE))
		return EINVAL;
	layout = boot_image->layout;
	if (offset % layout->image->page_size ||
	    length % layout->image->page_size ||
	    offset >= layout->image->image_size ||
	    length > layout->image->image_size - offset)
		return EINVAL;
	end = offset + length;
	covered = offset;
	for (index = 0; index < layout->image->segment_count && covered < end;
	     index++) {
		const struct kobox_fixed_image_segment *segment =
			&layout->image->segments[index];
		size_t segment_end = segment->image_offset + segment->mapping_size;

		if (segment_end <= covered)
			continue;
		if (segment->image_offset > covered)
			return EINVAL;
		covered = segment_end < end ? segment_end : end;
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
	/* The direct alias must not defeat read-only publication. Revocation
	 * leaves backing RAM writable so Linux buddy can reuse released pages. */
	if (mprotect((void *)(layout->ram_start + layout->physical_base + offset),
		     length, protection ? native & ~PROT_EXEC : PROT_READ | PROT_WRITE))
		return errno;
	return 0;
}

void kobox_boot_image_destroy(struct kobox_boot_image *image)
{
	if (!image || !image->layout)
		return;
	free(image->layout);
	memset(image, 0, sizeof(*image));
}
