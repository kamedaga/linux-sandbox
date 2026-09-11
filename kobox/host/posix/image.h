/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_IMAGE_H
#define KOBOX_POSIX_IMAGE_H

#include "host.h"
#include "../../boot/host.h"

struct kobox_boot_image {
	void *layout;
	size_t size;
};

/* Must run after dlopen relocation and before any CPU enters this core.
 * Only PT_LOAD pages belonging to the supplied DSO are replaced. On failure
 * the caller must not enter the core, even if some pages were already aliased.
 */
int kobox_boot_image_alias(void *library,
			   struct kobox_posix_memory_backing *backing,
			   void *direct_map, size_t physical_base,
			   struct kobox_boot_image *image);

/* Serialized by the owner; callers may not re-enable released pages. */
int kobox_boot_image_protect(void *image, size_t offset, size_t length,
			     unsigned int protection);
/* Release bookkeeping only, after CPUs stop; dlclose owns the DSO mapping. */
void kobox_boot_image_destroy(struct kobox_boot_image *image);

#endif
