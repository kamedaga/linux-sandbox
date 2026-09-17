/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_IMAGE_LAYOUT_H
#define KOBOX_BOOT_IMAGE_LAYOUT_H

/* A low, fixed VMA lets native x86 absolute 32-bit references remain valid.
 * The physical base is the matching offset in the hosted RAM backing. */
#define KOBOX_CORE_LINK_BASE 0x10000000
#define KOBOX_CORE_PHYSICAL_BASE 0x01000000
#define KOBOX_CORE_VMEMMAP_BASE 0x18000000
#define KOBOX_CORE_VMEMMAP_SIZE 0x01000000
/* Keep the fixed vmalloc window clear of supported hosts' native launcher
 * images while retaining the low-address range required by x86-64 kernel
 * code-model references. */
#define KOBOX_CORE_VMALLOC_BASE 0x70000000
#define KOBOX_CORE_VMALLOC_SIZE 0x10000000

#endif
