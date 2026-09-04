/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_MEMORY_HOST_H
#define KOBOX_LINUX_MEMORY_HOST_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define KOBOX_LINUX_MEMORY_HOST_IDENTITY 0x4b4f424f584d454dULL
#define KOBOX_LINUX_MEMORY_PAGE_SIZE 4096U
#define KOBOX_LINUX_MEMORY_LOGICAL_CPUS 2U

enum kobox_linux_memory_protection {
	KOBOX_LINUX_MEMORY_READ = 1U << 0,
	KOBOX_LINUX_MEMORY_WRITE = 1U << 1,
};

struct kobox_linux_memory_host_operations {
	size_t size;
	uint64_t identity;
	int (*map)(void *window, size_t window_offset, void *backing,
		   size_t backing_offset, size_t size, unsigned int protection,
		   void **address_out);
	int (*reset)(void *window, size_t window_offset, size_t size);
};

struct kobox_linux_memory_layout {
	size_t size;
	uint64_t identity;
	const struct kobox_linux_memory_host_operations *operations;
	void *ram_backing;
	void *vmemmap_window;
	void *vmalloc_window;
	void *direct_map;
	size_t ram_size;
	void *vmemmap_base;
	size_t vmemmap_size;
	void *vmalloc_base;
	size_t vmalloc_size;
	uint64_t kernel_image_physical_base;
};

struct kobox_linux_memory_report {
	size_t size;
	uint64_t identity;
	uint64_t ram_size;
	uint64_t page_offset_base;
	uint64_t vmemmap_base;
	uint64_t vmalloc_base;
	uint64_t phys_base;
	uint64_t allocated_pfn;
	uint32_t logical_cpu_count;
	uint32_t early_cpuhp_registration_count;
	uint8_t mm_core_initialized;
	uint8_t page_pfn_roundtrip;
	uint8_t direct_map_roundtrip;
	uint8_t kmalloc_ready;
	uint8_t slub_ready;
	uint8_t static_percpu_ready;
	uint8_t dynamic_percpu_ready;
	uint8_t vmap_alias_ready;
	uint8_t kernel_image_translation_ready;
	uint8_t early_cpuhp_registrations_ready;
};

typedef int (*kobox_linux_memory_boot_fn)(
	const struct kobox_linux_memory_layout *layout,
	struct kobox_linux_memory_report *report);

#endif /* KOBOX_LINUX_MEMORY_HOST_H */
