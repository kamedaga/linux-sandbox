/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FIXED_IMAGE_H
#define KOBOX_BOOT_FIXED_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define KOBOX_FIXED_IMAGE_MAX_SEGMENTS 16U

enum kobox_fixed_image_protection {
	KOBOX_FIXED_IMAGE_READ = 1U << 0,
	KOBOX_FIXED_IMAGE_WRITE = 1U << 1,
	KOBOX_FIXED_IMAGE_EXECUTE = 1U << 2,
};

struct kobox_fixed_image_segment {
	size_t file_offset;
	size_t image_offset;
	size_t file_size;
	size_t memory_size;
	size_t mapping_size;
	unsigned int protection;
};

/* OS-independent description of the fully linked core. file must remain
 * available until the last symbol lookup; mapped bytes are owned by the OS
 * backend and are deliberately not represented here. */
struct kobox_fixed_image {
	const unsigned char *file;
	size_t file_size;
	uintptr_t link_base;
	size_t image_size;
	size_t page_size;
	struct kobox_fixed_image_segment segments[KOBOX_FIXED_IMAGE_MAX_SEGMENTS];
	unsigned int segment_count;
	const void *symbols;
	size_t symbol_count;
	const char *strings;
	size_t strings_size;
};

enum kobox_fixed_image_result {
	KOBOX_FIXED_IMAGE_OK,
	KOBOX_FIXED_IMAGE_INVALID,
	KOBOX_FIXED_IMAGE_TOO_MANY_SEGMENTS,
	KOBOX_FIXED_IMAGE_MISSING_SYMBOLS,
	KOBOX_FIXED_IMAGE_MISSING_SYMBOL,
};

enum kobox_fixed_image_result kobox_fixed_image_open(
	const void *file, size_t file_size, size_t page_size,
	struct kobox_fixed_image *image);
enum kobox_fixed_image_result kobox_fixed_image_symbol(
	const struct kobox_fixed_image *image, const char *name,
	size_t *image_offset);

#endif
