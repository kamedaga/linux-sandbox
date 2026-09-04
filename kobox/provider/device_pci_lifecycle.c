// SPDX-License-Identifier: GPL-2.0-only

#include "device_pci_lifecycle.h"

#include <kobox2/closure_layout.h>
#include <kobox2/dma_domain_layout.h>
#include <kobox2/irq_endpoint_layout.h>
#include <kobox2/pci_function_layout.h>

#include <stdint.h>
#include <string.h>

enum device_pci_phase {
	DEVICE_PCI_CLEAN = 0,
	DEVICE_PCI_INITIALIZING,
	DEVICE_PCI_ACTIVE,
	DEVICE_PCI_QUIESCING,
	DEVICE_PCI_QUIESCED,
	DEVICE_PCI_FAULTED,
};

struct device_pci_state {
	const struct kobox_module_context *context;
	struct kobox_linux_device_pci_resources resources;
	struct kobox_linux_irq_endpoint_resource
		irq_endpoints[KB2_IRQ_ENDPOINT_MAXIMUM_ENDPOINTS_PER_SLOT];
	uint32_t phase;
};

static struct device_pci_state device_pci;

static int identity_valid(
	const struct kobox_resource_interface_operations *operations,
	size_t expected_size)
{
	static const uint8_t identity[KOBOX_MODULE_INTERFACE_IDENTITY_SIZE] =
		KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER;

	return operations && operations->size >= expected_size &&
	       !memcmp(operations->identity, identity, sizeof(identity));
}

static int bind_resource(
	const struct kobox_module_context *context, uint32_t slot_id,
	uint32_t resource_type, uint64_t required_rights,
	const uint8_t digest[KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE],
	struct kobox_module_resource_handle *handle_out,
	struct kobox_module_resource_binding *binding_out)
{
	struct kobox_module_resource_info info;
	size_t count;
	uint32_t state;
	int status;

	status = context->runtime_operations->resource_count(
		context, slot_id, &state, &count);
	if (status != KOBOX_MODULE_RESOURCE_OK ||
	    state != KOBOX_MODULE_RESOURCE_PRESENT_STATE || count != 1)
		return -1;
	status = context->runtime_operations->resource_acquire(
		context, slot_id, 0, required_rights, handle_out);
	if (status != KOBOX_MODULE_RESOURCE_OK)
		return -1;
	status = context->runtime_operations->resource_info(context, *handle_out,
							  &info);
	if (status != KOBOX_MODULE_RESOURCE_OK ||
	    info.resource_type != resource_type ||
	    (required_rights & ~info.granted_rights))
		return -1;
	return context->runtime_operations->resource_bind(
		context, *handle_out, digest, binding_out) ==
		KOBOX_MODULE_RESOURCE_OK ? 0 : -1;
}

static int context_valid(const struct kobox_module_context *context)
{
	static const uint8_t identity[KOBOX_MODULE_INTERFACE_IDENTITY_SIZE] =
		KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER;

	return context && context->size == sizeof(*context) &&
	       !memcmp(context->identity, identity, sizeof(identity)) &&
	       context->generation && context->resource_view &&
	       context->runtime_operations &&
	       context->runtime_operations->size ==
		       sizeof(*context->runtime_operations) &&
	       !memcmp(context->runtime_operations->identity, identity,
		       sizeof(identity)) &&
	       context->runtime_operations->resource_count &&
	       context->runtime_operations->resource_acquire &&
	       context->runtime_operations->resource_bind &&
	       context->runtime_operations->resource_info;
}

int kobox_linux_device_pci_init(const struct kobox_module_context *context)
{
	static const uint8_t pci_digest[KB2_PCI_FUNCTION_SCHEMA_DIGEST_SIZE] =
		KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;
	static const uint8_t dma_digest[KB2_DMA_DOMAIN_SCHEMA_DIGEST_SIZE] =
		KB2_DMA_DOMAIN_SCHEMA_SHA256_BYTES;
	static const uint8_t irq_digest[KB2_IRQ_ENDPOINT_SCHEMA_DIGEST_SIZE] =
		KB2_IRQ_ENDPOINT_SCHEMA_SHA256_BYTES;
	struct kobox_linux_device_pci_resources resources = { 0 };
	const struct kobox_pci_function_resource_operations *pci_operations;
	const struct kobox_dma_domain_resource_operations *dma_operations;
	struct kobox_module_resource_handle pci_handle;
	struct kobox_module_resource_handle dma_handle;
	size_t irq_count;
	size_t irq_index;
	uint32_t irq_state;

	if (!context_valid(context) ||
	    __atomic_load_n(&device_pci.phase, __ATOMIC_ACQUIRE) !=
		    DEVICE_PCI_CLEAN)
		return -1;
	__atomic_store_n(&device_pci.phase, DEVICE_PCI_INITIALIZING,
			 __ATOMIC_RELEASE);
	if (bind_resource(context, KOBOX_DEVICE_PCI_FUNCTION_SLOT_ID,
			  KB2_CLOSURE_RESOURCE_DEVICE,
			  KB2_PCI_FUNCTION_REQUIRED_RIGHTS, pci_digest,
			  &pci_handle, &resources.pci) ||
	    bind_resource(context, KOBOX_DEVICE_PCI_DMA_DOMAIN_SLOT_ID,
			  KB2_CLOSURE_RESOURCE_DEVICE,
			  KB2_DMA_DOMAIN_REQUIRED_RIGHTS, dma_digest,
			  &dma_handle, &resources.dma))
		goto fail;
	if (context->runtime_operations->resource_count(
		    context, KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID, &irq_state,
		    &irq_count) != KOBOX_MODULE_RESOURCE_OK ||
	    irq_state != KOBOX_MODULE_RESOURCE_PRESENT_STATE || !irq_count ||
	    irq_count > KB2_IRQ_ENDPOINT_MAXIMUM_ENDPOINTS_PER_SLOT)
		goto fail;
	for (irq_index = 0; irq_index < irq_count; irq_index++) {
		struct kobox_linux_irq_endpoint_resource *endpoint =
			&device_pci.irq_endpoints[irq_index];
		const struct kobox_irq_endpoint_resource_operations *operations;
		struct kobox_module_resource_info info;
		struct kobox_module_resource_handle handle;

		if (context->runtime_operations->resource_acquire(
			    context, KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID,
			    irq_index, KB2_IRQ_ENDPOINT_REQUIRED_RIGHTS,
			    &handle) != KOBOX_MODULE_RESOURCE_OK ||
		    context->runtime_operations->resource_info(
			    context, handle, &info) != KOBOX_MODULE_RESOURCE_OK ||
		    info.resource_type != KB2_CLOSURE_RESOURCE_NOTIFICATION ||
		    (KB2_IRQ_ENDPOINT_REQUIRED_RIGHTS & ~info.granted_rights) ||
		    context->runtime_operations->resource_bind(
			    context, handle, irq_digest, &endpoint->binding) !=
			    KOBOX_MODULE_RESOURCE_OK)
			goto fail;
		operations = (const void *)endpoint->binding.operations;
		if (!identity_valid(endpoint->binding.operations,
				    sizeof(*operations)) || !operations->identity ||
		    !operations->handler_register ||
		    !operations->handler_unregister || !operations->enable ||
		    !operations->disable_and_synchronize ||
		    operations->identity(endpoint->binding.object,
					 &endpoint->generation,
					 &endpoint->object_id) ||
		    endpoint->generation != context->generation ||
		    endpoint->object_id != handle.object_id)
			goto fail;
	}
	resources.irq_endpoints = device_pci.irq_endpoints;
	resources.irq_endpoint_count = irq_count;
	pci_operations = (const void *)resources.pci.operations;
	dma_operations = (const void *)resources.dma.operations;
	if (!identity_valid(resources.pci.operations, sizeof(*pci_operations)) ||
	    !identity_valid(resources.dma.operations, sizeof(*dma_operations)) ||
	    !pci_operations->identity || !pci_operations->config_read ||
	    !pci_operations->config_write || !pci_operations->bar_info ||
	    !pci_operations->bar_map || !pci_operations->bar_unmap ||
	    !pci_operations->bar_read || !pci_operations->bar_write ||
	    !dma_operations->constraints || !dma_operations->allocation_create ||
	    !dma_operations->allocation_release ||
	    !dma_operations->mapping_create ||
	    !dma_operations->mapping_release || !dma_operations->sync_for_cpu ||
	    !dma_operations->sync_for_device || !dma_operations->drain ||
	    !dma_operations->mapping_create_span ||
	    pci_operations->identity(resources.pci.object,
				     &resources.pci_identity) ||
	    dma_operations->constraints(resources.dma.object,
					&resources.dma_constraints))
		goto fail;
	if (resources.pci_identity.generation != context->generation ||
	    resources.pci_identity.object_id != pci_handle.object_id ||
	    resources.dma_constraints.generation != context->generation ||
	    resources.dma_constraints.object_id != dma_handle.object_id ||
	    resources.pci_identity.vendor_id == 0xffff ||
	    resources.dma_constraints.address_bits < 32 ||
	    resources.dma_constraints.address_bits > 64 ||
	    !resources.dma_constraints.minimum_alignment ||
	    (resources.dma_constraints.minimum_alignment &
	     (resources.dma_constraints.minimum_alignment - 1)) ||
	    !resources.dma_constraints.maximum_segment_length ||
	    resources.dma_constraints.coherent > 1)
		goto fail;
	device_pci.context = context;
	device_pci.resources = resources;
	__atomic_store_n(&device_pci.phase, DEVICE_PCI_ACTIVE,
			 __ATOMIC_RELEASE);
	return 0;

fail:
	memset(&device_pci, 0, sizeof(device_pci));
	return -1;
}

int kobox_linux_device_pci_probe_resources(
	const struct kobox_module_context *context,
	struct kobox_linux_device_pci_resources *resources_out)
{
	if (!context || !resources_out ||
	    __atomic_load_n(&device_pci.phase, __ATOMIC_ACQUIRE) !=
		    DEVICE_PCI_ACTIVE ||
	    !device_pci.context ||
	    device_pci.context->generation != context->generation ||
	    device_pci.context->node_id != context->node_id)
		return -1;
	*resources_out = device_pci.resources;
	return 0;
}

int kobox_linux_device_pci_quiesce(const struct kobox_module_context *context)
{
	const struct kobox_dma_domain_resource_operations *dma_operations;
	size_t allocations;
	size_t mappings;
	size_t irq_index;

	if (!context || !device_pci.context ||
	    device_pci.context->generation != context->generation ||
	    device_pci.context->node_id != context->node_id ||
	    __atomic_load_n(&device_pci.phase, __ATOMIC_ACQUIRE) !=
		    DEVICE_PCI_ACTIVE)
		return -1;
	__atomic_store_n(&device_pci.phase, DEVICE_PCI_QUIESCING,
			 __ATOMIC_RELEASE);
	dma_operations = (const void *)device_pci.resources.dma.operations;
	for (irq_index = 0;
	     irq_index < device_pci.resources.irq_endpoint_count; irq_index++) {
		const struct kobox_linux_irq_endpoint_resource *endpoint =
			&device_pci.resources.irq_endpoints[irq_index];
		const struct kobox_irq_endpoint_resource_operations *operations =
			(const void *)endpoint->binding.operations;

		if (operations->disable_and_synchronize(endpoint->binding.object))
			goto fail;
	}
	if (dma_operations->drain(device_pci.resources.dma.object,
				  &allocations, &mappings) ||
	    allocations || mappings)
		goto fail;
	__atomic_store_n(&device_pci.phase, DEVICE_PCI_QUIESCED,
			 __ATOMIC_RELEASE);
	return 0;

fail:
	__atomic_store_n(&device_pci.phase, DEVICE_PCI_FAULTED,
			 __ATOMIC_RELEASE);
	return -1;
}

int kobox_linux_device_pci_cleanup(const struct kobox_module_context *context)
{
	const struct kobox_dma_domain_resource_operations *dma_operations;
	size_t allocations;
	size_t mappings;
	size_t irq_index;
	uint32_t phase;

	if (!context || !device_pci.context ||
	    device_pci.context->generation != context->generation ||
	    device_pci.context->node_id != context->node_id)
		return -1;
	phase = __atomic_load_n(&device_pci.phase, __ATOMIC_ACQUIRE);
	if (phase == DEVICE_PCI_ACTIVE &&
	    kobox_linux_device_pci_quiesce(context))
		phase = DEVICE_PCI_FAULTED;
	else
		phase = __atomic_load_n(&device_pci.phase, __ATOMIC_ACQUIRE);
	if (phase == DEVICE_PCI_FAULTED) {
		dma_operations =
			(const void *)device_pci.resources.dma.operations;
		for (irq_index = 0;
		     irq_index < device_pci.resources.irq_endpoint_count;
		     irq_index++) {
			const struct kobox_linux_irq_endpoint_resource *endpoint =
				&device_pci.resources.irq_endpoints[irq_index];
			const struct kobox_irq_endpoint_resource_operations *operations =
				(const void *)endpoint->binding.operations;

			if (operations->disable_and_synchronize(
				    endpoint->binding.object))
				return -1;
		}
		if (dma_operations->drain(device_pci.resources.dma.object,
					  &allocations, &mappings) ||
		    allocations || mappings)
			return -1;
		__atomic_store_n(&device_pci.phase, DEVICE_PCI_QUIESCED,
				 __ATOMIC_RELEASE);
	}
	if (__atomic_load_n(&device_pci.phase, __ATOMIC_ACQUIRE) !=
	    DEVICE_PCI_QUIESCED)
		return -1;
	memset(&device_pci, 0, sizeof(device_pci));
	return 0;
}
