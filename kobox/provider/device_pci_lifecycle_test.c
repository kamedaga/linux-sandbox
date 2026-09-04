// SPDX-License-Identifier: GPL-2.0-only

#include "device_pci_lifecycle.h"

#include <kobox2/closure_layout.h>
#include <kobox2/dma_domain_layout.h>
#include <kobox2/irq_endpoint_layout.h>
#include <kobox2/pci_function_layout.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GENERATION UINT64_C(47)
#define TEST_PCI_OBJECT UINT64_C(101)
#define TEST_DMA_OBJECT UINT64_C(102)
#define TEST_IRQ_OBJECT UINT64_C(103)
#define TEST_IRQ_COUNT 2u

#define CHECK(expression)                                                     \
	do {                                                                    \
		if (!(expression)) {                                              \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,    \
				__LINE__, #expression);                              \
			return -1;                                                  \
		}                                                               \
	} while (0)

struct test_dma_allocation {
	void *memory;
	size_t length;
};

struct test_dma_mapping {
	struct test_dma_allocation *allocation;
	size_t offset;
	size_t length;
};

struct test_irq_resource {
	uint64_t object_id;
	kobox_irq_endpoint_handler_fn handler;
	void *handler_argument;
	uint32_t irq_enabled;
	uint32_t irq_disable_count;
};

struct test_resources {
	uint8_t config[KB2_PCI_FUNCTION_CONFIG_SPACE_SIZE];
	uint8_t bar[4096];
	size_t allocations;
	size_t mappings;
	struct test_irq_resource irqs[TEST_IRQ_COUNT];
	uint64_t pci_generation;
};

static struct test_resources test_resources;

static int pci_identity(void *object,
			struct kobox_pci_function_identity *identity_out)
{
	struct test_resources *resources = object;

	if (!resources || !identity_out)
		return -1;
	*identity_out = (struct kobox_pci_function_identity){
		.generation = resources->pci_generation,
		.object_id = TEST_PCI_OBJECT,
		.segment = 0,
		.bus = 2,
		.device = 0,
		.function = 0,
		.revision = 1,
		.vendor_id = 0x1af4,
		.device_id = 0x1050,
		.subsystem_vendor_id = 0x1af4,
		.subsystem_device_id = 0x1100,
		.class_code = 0x030000,
	};
	return 0;
}

static int pci_config_range(uint32_t offset, uint32_t width)
{
	return (width == 1 || width == 2 || width == 4) &&
	       !(offset & (width - 1)) &&
	       offset <= KB2_PCI_FUNCTION_CONFIG_SPACE_SIZE - width;
}

static int pci_config_read(void *object, uint32_t offset, uint32_t width,
			   uint32_t *value_out)
{
	struct test_resources *resources = object;
	uint32_t value = 0;

	if (!resources || !value_out || !pci_config_range(offset, width))
		return -1;
	memcpy(&value, resources->config + offset, width);
	*value_out = value;
	return 0;
}

static int pci_config_write(void *object, uint32_t offset, uint32_t width,
			    uint32_t value)
{
	struct test_resources *resources = object;

	if (!resources || !pci_config_range(offset, width))
		return -1;
	memcpy(resources->config + offset, &value, width);
	return 0;
}

static int pci_bar_info(void *object, uint32_t bar, uint64_t *length_out,
			uint32_t *flags_out)
{
	if (!object || bar || !length_out || !flags_out)
		return -1;
	*length_out = sizeof(test_resources.bar);
	*flags_out = KB2_PCI_FUNCTION_BAR_FLAG_MEMORY;
	return 0;
}

static int pci_bar_map(void *object, uint32_t bar, uint64_t offset,
		       size_t length, uint32_t protection, void **address_out)
{
	struct test_resources *resources = object;

	if (!resources || bar || offset > sizeof(resources->bar) ||
	    length > sizeof(resources->bar) - offset || !length ||
	    !protection ||
	    (protection & ~(KB2_PCI_FUNCTION_MAP_PROTECTION_READ |
			    KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE)) ||
	    !address_out)
		return -1;
	*address_out = resources->bar + offset;
	return 0;
}

static int pci_bar_unmap(void *object, void *address, size_t length)
{
	struct test_resources *resources = object;
	uintptr_t start;
	uintptr_t end;

	if (!resources || !address || !length)
		return -1;
	start = (uintptr_t)resources->bar;
	end = start + sizeof(resources->bar);
	return (uintptr_t)address >= start && (uintptr_t)address <= end - length ?
		0 : -1;
}

static int pci_bar_read(void *object, uint32_t bar, uint64_t offset,
			uint32_t width, uint32_t *value_out)
{
	struct test_resources *resources = object;
	uint32_t value = 0;

	if (!resources || bar || !value_out ||
	    !pci_config_range((uint32_t)offset, width) ||
	    offset > sizeof(resources->bar) - width)
		return -1;
	memcpy(&value, resources->bar + offset, width);
	*value_out = value;
	return 0;
}

static int pci_bar_write(void *object, uint32_t bar, uint64_t offset,
			 uint32_t width, uint32_t value)
{
	struct test_resources *resources = object;

	if (!resources || bar || !pci_config_range((uint32_t)offset, width) ||
	    offset > sizeof(resources->bar) - width)
		return -1;
	memcpy(resources->bar + offset, &value, width);
	return 0;
}

static int dma_constraints(
	void *object, struct kobox_dma_domain_constraints *constraints_out)
{
	if (!object || !constraints_out)
		return -1;
	*constraints_out = (struct kobox_dma_domain_constraints){
		.generation = TEST_GENERATION,
		.object_id = TEST_DMA_OBJECT,
		.minimum_alignment = 4096,
		.maximum_segment_length = 1024 * 1024,
		.address_bits = 64,
		.coherent = 1,
	};
	return 0;
}

static int dma_allocation_create(void *object, size_t length, size_t alignment,
				 uint32_t flags, void **allocation_out,
				 void **cpu_address_out)
{
	struct test_resources *resources = object;
	struct test_dma_allocation *allocation;

	if (!resources || !length || alignment != 4096 ||
	    flags & ~(KB2_DMA_DOMAIN_ALLOCATION_FLAG_CPU_MAP |
		      KB2_DMA_DOMAIN_ALLOCATION_FLAG_ZERO |
		      KB2_DMA_DOMAIN_ALLOCATION_FLAG_COHERENT) ||
	    !allocation_out || !cpu_address_out)
		return -1;
	allocation = calloc(1, sizeof(*allocation));
	if (!allocation)
		return -1;
	allocation->memory = aligned_alloc(alignment,
					 (length + alignment - 1) & ~(alignment - 1));
	if (!allocation->memory) {
		free(allocation);
		return -1;
	}
	allocation->length = length;
	resources->allocations++;
	*allocation_out = allocation;
	*cpu_address_out = allocation->memory;
	return 0;
}

static int dma_allocation_release(void *object, void *opaque_allocation)
{
	struct test_resources *resources = object;
	struct test_dma_allocation *allocation = opaque_allocation;

	if (!resources || !allocation || !resources->allocations)
		return -1;
	free(allocation->memory);
	free(allocation);
	resources->allocations--;
	return 0;
}

static int dma_mapping_create(void *object, void *opaque_allocation,
			      size_t offset, size_t length, uint32_t direction,
			      void **mapping_out,
			      uint64_t *device_address_out)
{
	struct test_resources *resources = object;
	struct test_dma_allocation *allocation = opaque_allocation;
	struct test_dma_mapping *mapping;

	if (!resources || !allocation || offset > allocation->length ||
	    length > allocation->length - offset || !length ||
	    direction < KB2_DMA_DOMAIN_DIRECTION_TO_DEVICE ||
	    direction > KB2_DMA_DOMAIN_DIRECTION_BIDIRECTIONAL ||
	    !mapping_out || !device_address_out)
		return -1;
	mapping = malloc(sizeof(*mapping));
	if (!mapping)
		return -1;
	*mapping = (struct test_dma_mapping){
		.allocation = allocation,
		.offset = offset,
		.length = length,
	};
	resources->mappings++;
	*mapping_out = mapping;
	*device_address_out = UINT64_C(0x10000000) + offset;
	return 0;
}

static int dma_mapping_create_span(void *object, void *cpu_address,
				   size_t length, uint32_t direction,
				   void **mapping_out,
				   uint64_t *device_address_out)
{
	struct test_resources *resources = object;
	struct test_dma_mapping *mapping;

	if (!resources || !cpu_address || !length ||
	    direction < KB2_DMA_DOMAIN_DIRECTION_TO_DEVICE ||
	    direction > KB2_DMA_DOMAIN_DIRECTION_BIDIRECTIONAL ||
	    !mapping_out || !device_address_out)
		return -1;
	mapping = calloc(1, sizeof(*mapping));
	if (!mapping)
		return -1;
	mapping->length = length;
	resources->mappings++;
	*mapping_out = mapping;
	*device_address_out = (uintptr_t)cpu_address;
	return 0;
}

static int dma_mapping_release(void *object, void *opaque_mapping)
{
	struct test_resources *resources = object;

	if (!resources || !opaque_mapping || !resources->mappings)
		return -1;
	free(opaque_mapping);
	resources->mappings--;
	return 0;
}

static int dma_sync(void *object, void *opaque_mapping, size_t offset,
		    size_t length)
{
	struct test_dma_mapping *mapping = opaque_mapping;

	if (!object || !mapping || offset > mapping->length ||
	    length > mapping->length - offset)
		return -1;
	return 0;
}

static int dma_drain(void *object, size_t *allocation_count_out,
		     size_t *mapping_count_out)
{
	struct test_resources *resources = object;

	if (!resources || !allocation_count_out || !mapping_count_out)
		return -1;
	*allocation_count_out = resources->allocations;
	*mapping_count_out = resources->mappings;
	return 0;
}

static int irq_identity(void *object, uint64_t *generation_out,
			uint64_t *object_id_out)
{
	struct test_irq_resource *endpoint = object;

	if (!endpoint || !generation_out || !object_id_out)
		return -1;
	*generation_out = TEST_GENERATION;
	*object_id_out = endpoint->object_id;
	return 0;
}

static int irq_handler_register(void *object,
				kobox_irq_endpoint_handler_fn handler,
				void *argument)
{
	struct test_irq_resource *endpoint = object;

	if (!endpoint || !handler || endpoint->handler)
		return -1;
	endpoint->handler = handler;
	endpoint->handler_argument = argument;
	return 0;
}

static int irq_handler_unregister(void *object,
				  kobox_irq_endpoint_handler_fn handler,
				  void *argument)
{
	struct test_irq_resource *endpoint = object;

	if (!endpoint || endpoint->irq_enabled || endpoint->handler != handler ||
	    endpoint->handler_argument != argument)
		return -1;
	endpoint->handler = NULL;
	endpoint->handler_argument = NULL;
	return 0;
}

static int irq_enable(void *object)
{
	struct test_irq_resource *endpoint = object;

	if (!endpoint || !endpoint->handler || endpoint->irq_enabled)
		return -1;
	endpoint->irq_enabled = 1;
	endpoint->handler(endpoint->handler_argument);
	return 0;
}

static int irq_disable_and_synchronize(void *object)
{
	struct test_irq_resource *endpoint = object;

	if (!endpoint)
		return -1;
	endpoint->irq_enabled = 0;
	endpoint->irq_disable_count++;
	return 0;
}

static const struct kobox_pci_function_resource_operations pci_operations = {
	.base = {
		.size = sizeof(pci_operations),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
	},
	.identity = pci_identity,
	.config_read = pci_config_read,
	.config_write = pci_config_write,
	.bar_info = pci_bar_info,
	.bar_map = pci_bar_map,
	.bar_unmap = pci_bar_unmap,
	.bar_read = pci_bar_read,
	.bar_write = pci_bar_write,
};

static const struct kobox_dma_domain_resource_operations dma_operations = {
	.base = {
		.size = sizeof(dma_operations),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
	},
	.constraints = dma_constraints,
	.allocation_create = dma_allocation_create,
	.allocation_release = dma_allocation_release,
	.mapping_create = dma_mapping_create,
	.mapping_release = dma_mapping_release,
	.sync_for_cpu = dma_sync,
	.sync_for_device = dma_sync,
	.drain = dma_drain,
	.mapping_create_span = dma_mapping_create_span,
};

static const struct kobox_irq_endpoint_resource_operations irq_operations = {
	.base = {
		.size = sizeof(irq_operations),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
	},
	.identity = irq_identity,
	.handler_register = irq_handler_register,
	.handler_unregister = irq_handler_unregister,
	.enable = irq_enable,
	.disable_and_synchronize = irq_disable_and_synchronize,
};

static int resource_count(const struct kobox_module_context *context,
			  uint32_t slot_id, uint32_t *state_out,
			  size_t *count_out)
{
	if (!context || !state_out || !count_out || slot_id < 1 || slot_id > 3)
		return KOBOX_MODULE_RESOURCE_NOT_VISIBLE;
	*state_out = KOBOX_MODULE_RESOURCE_PRESENT_STATE;
	*count_out = slot_id == KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID ?
		TEST_IRQ_COUNT : 1;
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_acquire(
	const struct kobox_module_context *context, uint32_t slot_id,
	size_t object_index, uint64_t required_rights,
	struct kobox_module_resource_handle *handle_out)
{
	static const uint64_t rights[] = {
		0,
		KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
		KB2_DMA_DOMAIN_REQUIRED_RIGHTS,
		KB2_IRQ_ENDPOINT_REQUIRED_RIGHTS,
	};

	if (!context || slot_id < 1 || slot_id > 3 ||
	    (slot_id != KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID && object_index) ||
	    (slot_id == KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID &&
	     object_index >= TEST_IRQ_COUNT) ||
	    required_rights != rights[slot_id] || !handle_out)
		return KOBOX_MODULE_RESOURCE_INVALID_ARGUMENT;
	*handle_out = (struct kobox_module_resource_handle){
		.generation = context->generation,
		.object_id = slot_id == KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID ?
			TEST_IRQ_OBJECT + object_index :
			TEST_PCI_OBJECT + slot_id - 1,
	};
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_info(
	const struct kobox_module_context *context,
	struct kobox_module_resource_handle handle,
	struct kobox_module_resource_info *info_out)
{
	if (!context || handle.generation != context->generation || !info_out ||
	    handle.object_id < TEST_PCI_OBJECT ||
	    handle.object_id >= TEST_IRQ_OBJECT + TEST_IRQ_COUNT)
		return KOBOX_MODULE_RESOURCE_STALE;
	*info_out = (struct kobox_module_resource_info){
		.resource_type = handle.object_id >= TEST_IRQ_OBJECT ?
			KB2_CLOSURE_RESOURCE_NOTIFICATION :
			KB2_CLOSURE_RESOURCE_DEVICE,
		.granted_rights = handle.object_id == TEST_PCI_OBJECT ?
			KB2_PCI_FUNCTION_REQUIRED_RIGHTS :
			handle.object_id == TEST_DMA_OBJECT ?
				KB2_DMA_DOMAIN_REQUIRED_RIGHTS :
				KB2_IRQ_ENDPOINT_REQUIRED_RIGHTS,
	};
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_bind(
	const struct kobox_module_context *context,
	struct kobox_module_resource_handle handle,
	const uint8_t digest[KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE],
	struct kobox_module_resource_binding *binding_out)
{
	static const uint8_t pci_digest[KB2_PCI_FUNCTION_SCHEMA_DIGEST_SIZE] =
		KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;
	static const uint8_t dma_digest[KB2_DMA_DOMAIN_SCHEMA_DIGEST_SIZE] =
		KB2_DMA_DOMAIN_SCHEMA_SHA256_BYTES;
	static const uint8_t irq_digest[KB2_IRQ_ENDPOINT_SCHEMA_DIGEST_SIZE] =
		KB2_IRQ_ENDPOINT_SCHEMA_SHA256_BYTES;
	const struct kobox_resource_interface_operations *operations;
	const uint8_t *expected;

	if (!context || handle.generation != context->generation || !digest ||
	    !binding_out)
		return KOBOX_MODULE_RESOURCE_STALE;
	switch (handle.object_id) {
	case TEST_PCI_OBJECT:
		expected = pci_digest;
		operations = &pci_operations.base;
		break;
	case TEST_DMA_OBJECT:
		expected = dma_digest;
		operations = &dma_operations.base;
		break;
	default:
		if (handle.object_id < TEST_IRQ_OBJECT ||
		    handle.object_id >= TEST_IRQ_OBJECT + TEST_IRQ_COUNT)
			return KOBOX_MODULE_RESOURCE_NOT_VISIBLE;
		expected = irq_digest;
		operations = &irq_operations.base;
		break;
	}
	if (memcmp(digest, expected, KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE))
		return KOBOX_MODULE_RESOURCE_INTERFACE;
	*binding_out = (struct kobox_module_resource_binding){
		.operations = operations,
		.object = handle.object_id >= TEST_IRQ_OBJECT ?
			(void *)&test_resources.irqs[
				handle.object_id - TEST_IRQ_OBJECT] :
			(void *)&test_resources,
	};
	return KOBOX_MODULE_RESOURCE_OK;
}

static void irq_observed(void *argument)
{
	uint32_t *count = argument;

	(*count)++;
}

static int test_lifecycle(void)
{
	const struct kobox_module_runtime_operations runtime_operations = {
		.size = sizeof(runtime_operations),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
		.resource_count = resource_count,
		.resource_acquire = resource_acquire,
		.resource_bind = resource_bind,
		.resource_info = resource_info,
	};
	struct kobox_module_context context = {
		.size = sizeof(context),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
		.generation = TEST_GENERATION,
		.node_id = 2,
		.resource_view = &test_resources,
		.runtime_operations = &runtime_operations,
		.logical_cpu_count = 2,
	};
	struct kobox_linux_device_pci_resources resources;
	const struct kobox_pci_function_resource_operations *pci;
	const struct kobox_dma_domain_resource_operations *dma;
	const struct kobox_irq_endpoint_resource_operations *irq;
	void *allocation;
	void *cpu_address;
	void *mapping;
	void *bar;
	uint64_t device_address;
	uint64_t bar_length;
	uint32_t bar_flags;
	uint32_t irq_count = 0;
	uint32_t value;

	memset(&test_resources, 0, sizeof(test_resources));
	test_resources.pci_generation = TEST_GENERATION;
	test_resources.irqs[0].object_id = TEST_IRQ_OBJECT;
	test_resources.irqs[1].object_id = TEST_IRQ_OBJECT + 1;
	CHECK(!kobox_linux_device_pci_init(&context));
	CHECK(kobox_linux_device_pci_init(&context));
	CHECK(!kobox_linux_device_pci_probe_resources(&context, &resources));
	CHECK(resources.pci_identity.vendor_id == 0x1af4 &&
	      resources.pci_identity.device_id == 0x1050 &&
	      resources.dma_constraints.address_bits == 64 &&
	      resources.irq_endpoint_count == TEST_IRQ_COUNT &&
	      resources.irq_endpoints[0].object_id == TEST_IRQ_OBJECT &&
	      resources.irq_endpoints[1].object_id == TEST_IRQ_OBJECT + 1);
	pci = (const void *)resources.pci.operations;
	dma = (const void *)resources.dma.operations;
	irq = (const void *)resources.irq_endpoints[0].binding.operations;
	CHECK(!pci->config_write(resources.pci.object, 4, 2, 0x0006) &&
	      !pci->config_read(resources.pci.object, 4, 2, &value) &&
	      value == 0x0006 &&
	      !pci->bar_info(resources.pci.object, 0, &bar_length, &bar_flags) &&
	      bar_length == sizeof(test_resources.bar) &&
	      bar_flags == KB2_PCI_FUNCTION_BAR_FLAG_MEMORY &&
	      !pci->bar_map(resources.pci.object, 0, 0, 4096,
			    KB2_PCI_FUNCTION_MAP_PROTECTION_READ |
				    KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE,
			    &bar) &&
	      !pci->bar_unmap(resources.pci.object, bar, 4096));
	CHECK(!dma->allocation_create(
		      resources.dma.object, 4096, 4096,
		      KB2_DMA_DOMAIN_ALLOCATION_FLAG_CPU_MAP |
			      KB2_DMA_DOMAIN_ALLOCATION_FLAG_ZERO,
		      &allocation, &cpu_address) &&
	      cpu_address &&
	      !dma->mapping_create(resources.dma.object, allocation, 0, 4096,
			   KB2_DMA_DOMAIN_DIRECTION_BIDIRECTIONAL,
			   &mapping, &device_address) &&
	      device_address == UINT64_C(0x10000000) &&
	      !dma->sync_for_device(resources.dma.object, mapping, 0, 4096));
	CHECK(!irq->handler_register(resources.irq_endpoints[0].binding.object,
				     irq_observed,
				     &irq_count) &&
	      !irq->enable(resources.irq_endpoints[0].binding.object) &&
	      irq_count == 1);
	CHECK(kobox_linux_device_pci_quiesce(&context));
	CHECK(kobox_linux_device_pci_probe_resources(&context, &resources));
	CHECK(!dma->mapping_release(resources.dma.object, mapping) &&
	      !dma->allocation_release(resources.dma.object, allocation));
	CHECK(!irq->handler_unregister(resources.irq_endpoints[0].binding.object,
				       irq_observed,
				       &irq_count));
	CHECK(!kobox_linux_device_pci_cleanup(&context));
	CHECK(test_resources.irqs[0].irq_disable_count == 2 &&
	      test_resources.irqs[1].irq_disable_count == 2);

	test_resources.pci_generation = TEST_GENERATION - 1;
	CHECK(kobox_linux_device_pci_init(&context));
	test_resources.pci_generation = TEST_GENERATION;
	CHECK(!kobox_linux_device_pci_init(&context) &&
	      !kobox_linux_device_pci_cleanup(&context));
	return 0;
}

int main(void)
{
	return test_lifecycle() ? EXIT_FAILURE : EXIT_SUCCESS;
}
