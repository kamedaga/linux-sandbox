/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_DEVICE_RESOURCE_INTERFACES_H
#define KOBOX_DEVICE_RESOURCE_INTERFACES_H

#include "../runtime/module_context.h"

struct kobox_pci_function_identity {
	kobox_abi_u64 generation;
	kobox_abi_u64 object_id;
	kobox_abi_u32 segment;
	kobox_abi_u8 bus;
	kobox_abi_u8 device;
	kobox_abi_u8 function;
	kobox_abi_u8 revision;
	kobox_abi_u16 vendor_id;
	kobox_abi_u16 device_id;
	kobox_abi_u16 subsystem_vendor_id;
	kobox_abi_u16 subsystem_device_id;
	kobox_abi_u32 class_code;
};

struct kobox_pci_function_resource_operations {
	struct kobox_resource_interface_operations base;
	int (*identity)(void *object,
			struct kobox_pci_function_identity *identity_out);
	int (*config_read)(void *object, kobox_abi_u32 offset,
			   kobox_abi_u32 width, kobox_abi_u32 *value_out);
	int (*config_write)(void *object, kobox_abi_u32 offset,
			    kobox_abi_u32 width, kobox_abi_u32 value);
	int (*bar_info)(void *object, kobox_abi_u32 bar,
			kobox_abi_u64 *cpu_address_out, kobox_abi_u64 *length_out,
			kobox_abi_u32 *flags_out);
	/* Local callbacks only. A non-NULL target is a caller-owned reservation,
	 * not an address accepted from a resource-grant wire message. Fixed
	 * unmap restores that reservation; cache types must not be ignored.
	 * page_offset is relative to the page-aligned BAR base, unlike bar_read
	 * and bar_write offsets. The host must authorize complete mapped pages,
	 * including padding outside a sub-page BAR, or fail the request.
	 */
	int (*bar_map)(void *object, kobox_abi_u32 bar, kobox_abi_u64 page_offset,
		       size_t length, kobox_abi_u32 protection,
		       kobox_abi_u32 cache_type, void *requested_address,
		       void **address_out);
	int (*bar_unmap)(void *object, void *address, size_t length);
	int (*bar_read)(void *object, kobox_abi_u32 bar, kobox_abi_u64 offset,
			kobox_abi_u32 width, kobox_abi_u32 *value_out);
	int (*bar_write)(void *object, kobox_abi_u32 bar, kobox_abi_u64 offset,
			 kobox_abi_u32 width, kobox_abi_u32 value);
};

struct kobox_dma_domain_constraints {
	kobox_abi_u64 generation;
	kobox_abi_u64 object_id;
	kobox_abi_u64 minimum_alignment;
	kobox_abi_u64 maximum_segment_length;
	kobox_abi_u32 address_bits;
	kobox_abi_u32 coherent;
};

struct kobox_dma_domain_resource_operations {
	struct kobox_resource_interface_operations base;
	int (*constraints)(void *object,
			   struct kobox_dma_domain_constraints *constraints_out);
	int (*allocation_create)(void *object, size_t length, size_t alignment,
				 kobox_abi_u32 flags, void **allocation_out,
				 void **cpu_address_out);
	int (*allocation_release)(void *object, void *allocation);
	int (*mapping_create)(void *object, void *allocation, size_t offset,
			      size_t length, kobox_abi_u32 direction,
			      void **mapping_out,
			      kobox_abi_u64 *device_address_out);
	int (*mapping_release)(void *object, void *mapping);
	int (*sync_for_cpu)(void *object, void *mapping, size_t offset,
			    size_t length);
	int (*sync_for_device)(void *object, void *mapping, size_t offset,
			       size_t length);
	int (*drain)(void *object, size_t *allocation_count_out,
		     size_t *mapping_count_out);
	int (*mapping_create_span)(void *object, void *cpu_address, size_t length,
				   kobox_abi_u32 direction, void **mapping_out,
				   kobox_abi_u64 *device_address_out);
};

typedef void (*kobox_irq_endpoint_handler_fn)(void *argument);

struct kobox_irq_endpoint_resource_operations {
	struct kobox_resource_interface_operations base;
	int (*identity)(void *object, kobox_abi_u64 *generation_out,
			kobox_abi_u64 *object_id_out);
	int (*handler_register)(void *object,
				kobox_irq_endpoint_handler_fn handler,
				void *argument);
	int (*handler_unregister)(void *object,
				  kobox_irq_endpoint_handler_fn handler,
				  void *argument);
	int (*enable)(void *object);
	int (*disable_and_synchronize)(void *object);
};

#endif
