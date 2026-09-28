/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_IMAGE_LAYOUT_H
#define KOBOX_BOOT_IMAGE_LAYOUT_H

/* A low, fixed VMA lets native x86 absolute 32-bit references remain valid.
 * The physical base is the matching offset in the hosted RAM backing. */
#define KOBOX_CORE_LINK_BASE 0x10000000
#define KOBOX_CORE_PHYSICAL_BASE 0x01000000
#define KOBOX_CORE_VMEMMAP_BASE 0x18000000
#define KOBOX_CORE_VMEMMAP_SIZE 0x01000000
/* A single PCI BAR mapping can consume 256 MiB plus vmalloc guard pages.
 * Keep room for other allocations, while leaving the native launcher,
 * process stack and read-only launch data below this window. The exclusive
 * end stays below 2 GiB for the hosted x86-64 kernel code model. */
#define KOBOX_CORE_VMALLOC_BASE 0x50000000
#define KOBOX_CORE_VMALLOC_SIZE 0x30000000

#endif
