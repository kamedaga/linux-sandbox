// SPDX-License-Identifier: GPL-2.0-only

#include "device_pci_bridge.h"

#include "device_pci_lifecycle.h"

#include <kobox2/dma_domain_layout.h>
#include <kobox2/pci_function_layout.h>

#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/slab.h>

struct kobox_linux_pci_mapping {
	struct list_head node;
	void __iomem *address;
	size_t length;
	unsigned int bar;
	resource_size_t offset;
};

struct kobox_linux_dma_mapping {
	struct list_head node;
	void *allocation;
	void *mapping;
	void *cpu_address;
	dma_addr_t device_address;
	size_t length;
	u32 direction;
};

struct kobox_linux_irq_binding {
	const struct kobox_linux_irq_endpoint_resource *endpoint;
	unsigned int irq;
	u32 registered;
};

struct kobox_linux_pci_bridge {
	const struct kobox_module_context *context;
	struct kobox_linux_device_pci_resources resources;
	struct pci_host_bridge *host_bridge;
	struct pci_dev *device;
	struct resource bar_windows[PCI_STD_NUM_BARS];
	struct list_head mappings;
	struct list_head dma_mappings;
	struct kobox_linux_irq_binding *irq_bindings;
	unsigned int irq_base;
	size_t irq_count;
	spinlock_t mapping_lock;
	size_t mapping_count;
};

static struct kobox_linux_pci_bridge kobox_pci_bridge;
static const struct kobox_module_context *kobox_pci_core_context;

extern initcall_entry_t __kobox_pci_initcall2_start[];
extern initcall_entry_t __kobox_pci_initcall2_end[];
extern int early_irq_init(void);

int arch_early_irq_init(void)
{
	return 0;
}

int arch_probe_nr_irqs(void)
{
	return 0;
}

static const struct kobox_irq_endpoint_resource_operations *
irq_endpoint_operations(const struct kobox_linux_irq_binding *binding)
{
	return binding && binding->endpoint ?
		(const void *)binding->endpoint->binding.operations : NULL;
}

static unsigned int kobox_irq_startup(struct irq_data *data)
{
	struct kobox_linux_irq_binding *binding = irq_data_get_irq_chip_data(data);
	const struct kobox_irq_endpoint_resource_operations *operations =
		irq_endpoint_operations(binding);

	return !operations || operations->enable(binding->endpoint->binding.object) ?
		-EIO : 0;
}

static void kobox_irq_shutdown(struct irq_data *data)
{
	struct kobox_linux_irq_binding *binding = irq_data_get_irq_chip_data(data);
	const struct kobox_irq_endpoint_resource_operations *operations =
		irq_endpoint_operations(binding);

	if (!operations || operations->disable_and_synchronize(
				 binding->endpoint->binding.object))
		WARN_ON_ONCE(1);
}

static void kobox_irq_mask(struct irq_data *data)
{
	kobox_irq_shutdown(data);
}

static void kobox_irq_unmask(struct irq_data *data)
{
	if (kobox_irq_startup(data))
		WARN_ON_ONCE(1);
}

static struct irq_chip kobox_irq_chip = {
	.name = "kobox2-endpoint",
	.irq_startup = kobox_irq_startup,
	.irq_shutdown = kobox_irq_shutdown,
	.irq_mask = kobox_irq_mask,
	.irq_unmask = kobox_irq_unmask,
	.flags = IRQCHIP_ONESHOT_SAFE,
};

static void kobox_irq_deliver(void *argument)
{
	struct kobox_linux_irq_binding *binding = argument;

	if (!binding)
		return;
	if (generic_handle_irq_safe(binding->irq))
		WARN_ON_ONCE(1);
}

static void bridge_irqs_remove(void)
{
	size_t index;

	if (!kobox_pci_bridge.irq_bindings)
		return;
	for (index = 0; index < kobox_pci_bridge.irq_count; index++) {
		struct kobox_linux_irq_binding *binding =
			&kobox_pci_bridge.irq_bindings[index];
		const struct kobox_irq_endpoint_resource_operations *operations =
			irq_endpoint_operations(binding);

		if (operations) {
			(void)operations->disable_and_synchronize(
				binding->endpoint->binding.object);
			if (binding->registered)
				(void)operations->handler_unregister(
					binding->endpoint->binding.object,
					kobox_irq_deliver, binding);
		}
		irq_set_status_flags(binding->irq, IRQ_NOREQUEST);
		irq_set_chip_and_handler(binding->irq, NULL, NULL);
		irq_set_chip_data(binding->irq, NULL);
	}
	irq_free_descs(kobox_pci_bridge.irq_base, kobox_pci_bridge.irq_count);
	kfree(kobox_pci_bridge.irq_bindings);
	kobox_pci_bridge.irq_bindings = NULL;
	kobox_pci_bridge.irq_count = 0;
}

static int bridge_irqs_init(struct pci_dev *device)
{
	const struct kobox_linux_device_pci_resources *resources =
		&kobox_pci_bridge.resources;
	struct kobox_linux_irq_binding *bindings;
	int base;
	size_t index;

	if (!device || !resources->irq_endpoint_count ||
	    resources->irq_endpoint_count > UINT_MAX)
		return -EINVAL;
	base = irq_alloc_descs(-1, 1, resources->irq_endpoint_count,
			       NUMA_NO_NODE);
	if (base < 0)
		return base;
	bindings = kcalloc(resources->irq_endpoint_count, sizeof(*bindings),
			   GFP_KERNEL);
	if (!bindings) {
		irq_free_descs(base, resources->irq_endpoint_count);
		return -ENOMEM;
	}
	kobox_pci_bridge.irq_bindings = bindings;
	kobox_pci_bridge.irq_base = base;
	kobox_pci_bridge.irq_count = resources->irq_endpoint_count;
	for (index = 0; index < resources->irq_endpoint_count; index++) {
		const struct kobox_irq_endpoint_resource_operations *operations;
		struct kobox_linux_irq_binding *binding = &bindings[index];

		binding->endpoint = &resources->irq_endpoints[index];
		binding->irq = base + index;
		operations = irq_endpoint_operations(binding);
		irq_set_chip_data(binding->irq, binding);
		irq_set_chip_and_handler(binding->irq, &kobox_irq_chip,
					 handle_simple_irq);
		irq_clear_status_flags(binding->irq, IRQ_NOREQUEST);
		if (!operations || operations->handler_register(
				binding->endpoint->binding.object,
				kobox_irq_deliver, binding)) {
			bridge_irqs_remove();
			return -EIO;
		}
		binding->registered = 1;
	}
	device->irq = base;
	return 0;
}

int kobox_linux_device_pci_core_init(
	const struct kobox_module_context *context)
{
	initcall_entry_t *entry;

	if (!context || kobox_pci_core_context ||
	    (unsigned long)__kobox_pci_initcall2_start ==
		    (unsigned long)__kobox_pci_initcall2_end)
		return -EINVAL;
	if (early_irq_init())
		return -EIO;
	for (entry = __kobox_pci_initcall2_start;
	     entry < __kobox_pci_initcall2_end; entry++) {
		initcall_t initialize = initcall_from_entry(entry);
		int status;

		if (!initialize)
			return -EINVAL;
		status = initialize();
		if (status)
			return status;
	}
	kobox_pci_core_context = context;
	return 0;
}

int kobox_linux_device_pci_core_active(
	const struct kobox_module_context *context)
{
	return context && context == kobox_pci_core_context;
}

static int bridge_context_valid(const struct kobox_module_context *context)
{
	return context && kobox_pci_bridge.context == context &&
	       context->generation ==
		       kobox_pci_bridge.resources.pci_identity.generation;
}

static int bridge_config_read(struct pci_bus *bus, unsigned int devfn,
			      int offset, int length, u32 *value)
{
	const struct kobox_pci_function_identity *identity =
		&kobox_pci_bridge.resources.pci_identity;
	const struct kobox_pci_function_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.pci.operations;
	kobox_abi_u32 result;

	if (!value || !bus || !bridge_context_valid(
					  kobox_pci_bridge.context) ||
	    bus->number != identity->bus ||
	    devfn != PCI_DEVFN(identity->device, identity->function)) {
		if (value)
			*value = ~0U;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	if (offset < 0 || operations->config_read(
				kobox_pci_bridge.resources.pci.object,
				(kobox_abi_u32)offset,
				(kobox_abi_u32)length, &result)) {
		*value = ~0U;
		return PCIBIOS_SET_FAILED;
	}
	*value = result;
	return PCIBIOS_SUCCESSFUL;
}

static int bridge_config_write(struct pci_bus *bus, unsigned int devfn,
			       int offset, int length, u32 value)
{
	const struct kobox_pci_function_identity *identity =
		&kobox_pci_bridge.resources.pci_identity;
	const struct kobox_pci_function_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.pci.operations;

	if (!bus || !bridge_context_valid(kobox_pci_bridge.context) ||
	    bus->number != identity->bus ||
	    devfn != PCI_DEVFN(identity->device, identity->function))
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (offset < 0 || operations->config_write(
				kobox_pci_bridge.resources.pci.object,
				(kobox_abi_u32)offset,
				(kobox_abi_u32)length, value))
		return PCIBIOS_SET_FAILED;
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops kobox_pci_operations = {
	.read = bridge_config_read,
	.write = bridge_config_write,
};

void __iomem *pci_iomap_range(struct pci_dev *device, int bar,
			      unsigned long offset, unsigned long maximum_length)
{
	const struct kobox_pci_function_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.pci.operations;
	struct kobox_linux_pci_mapping *mapping;
	kobox_abi_u64 bar_length;
	kobox_abi_u32 bar_flags;
	size_t length;
	void *address;

	if (!bridge_context_valid(kobox_pci_bridge.context) ||
	    device != kobox_pci_bridge.device || bar < 0 || bar >= PCI_STD_NUM_BARS ||
	    operations->bar_info(kobox_pci_bridge.resources.pci.object,
				 (kobox_abi_u32)bar, &bar_length, &bar_flags) ||
	    !(bar_flags & KB2_PCI_FUNCTION_BAR_FLAG_MEMORY) ||
	    offset >= bar_length || bar_length - offset > SIZE_MAX)
		return NULL;
	length = (size_t)(bar_length - offset);
	if (maximum_length && maximum_length < length)
		length = maximum_length;
	if (!length || operations->bar_map(
				kobox_pci_bridge.resources.pci.object,
				(kobox_abi_u32)bar, offset, length,
				KB2_PCI_FUNCTION_MAP_PROTECTION_READ |
					KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE,
				&address))
		return NULL;
	mapping = kmalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping) {
		operations->bar_unmap(kobox_pci_bridge.resources.pci.object,
				      address, length);
		return NULL;
	}
	mapping->address = (void __iomem *)address;
	mapping->length = length;
	mapping->bar = bar;
	mapping->offset = offset;
	spin_lock(&kobox_pci_bridge.mapping_lock);
	list_add_tail(&mapping->node, &kobox_pci_bridge.mappings);
	kobox_pci_bridge.mapping_count++;
	spin_unlock(&kobox_pci_bridge.mapping_lock);
	return mapping->address;
}

void pci_iounmap(struct pci_dev *device, void __iomem *address)
{
	const struct kobox_pci_function_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.pci.operations;
	struct kobox_linux_pci_mapping *mapping;
	struct kobox_linux_pci_mapping *found = NULL;

	if (!address || !bridge_context_valid(kobox_pci_bridge.context) ||
	    device != kobox_pci_bridge.device)
		return;
	spin_lock(&kobox_pci_bridge.mapping_lock);
	list_for_each_entry(mapping, &kobox_pci_bridge.mappings, node) {
		if (mapping->address != address)
			continue;
		list_del(&mapping->node);
		kobox_pci_bridge.mapping_count--;
		found = mapping;
		break;
	}
	spin_unlock(&kobox_pci_bridge.mapping_lock);
	if (WARN_ON_ONCE(!found))
		return;
	WARN_ON_ONCE(operations->bar_unmap(
		kobox_pci_bridge.resources.pci.object,
		(void *)found->address, found->length));
	kfree(found);
}

static int bridge_bar_location(const void __iomem *address, size_t width,
			       unsigned int *bar_out,
			       resource_size_t *offset_out)
{
	struct kobox_linux_pci_mapping *mapping;
	uintptr_t target = (uintptr_t)address;
	int found = 0;

	if (!address || !width || !bar_out || !offset_out)
		return -EINVAL;
	spin_lock(&kobox_pci_bridge.mapping_lock);
	list_for_each_entry(mapping, &kobox_pci_bridge.mappings, node) {
		uintptr_t base = (uintptr_t)mapping->address;

		if (target < base || target - base > mapping->length ||
		    width > mapping->length - (target - base))
			continue;
		*bar_out = mapping->bar;
		*offset_out = mapping->offset + (target - base);
		found = 1;
		break;
	}
	spin_unlock(&kobox_pci_bridge.mapping_lock);
	return found ? 0 : -EINVAL;
}

static u32 bridge_bar_read(const void __iomem *address, u32 width)
{
	const struct kobox_pci_function_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.pci.operations;
	resource_size_t offset;
	unsigned int bar;
	u32 value = U32_MAX;

	if (!operations || bridge_bar_location(address, width, &bar, &offset) ||
	    operations->bar_read(kobox_pci_bridge.resources.pci.object, bar,
				 offset, width, &value))
		WARN_ON_ONCE(1);
	return value;
}

static void bridge_bar_write(void __iomem *address, u32 width, u32 value)
{
	const struct kobox_pci_function_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.pci.operations;
	resource_size_t offset;
	unsigned int bar;

	if (!operations || bridge_bar_location(address, width, &bar, &offset) ||
	    operations->bar_write(kobox_pci_bridge.resources.pci.object, bar,
				  offset, width, value))
		WARN_ON_ONCE(1);
}

unsigned int ioread8(const void __iomem *address)
{
	return bridge_bar_read(address, 1) & U8_MAX;
}

unsigned int ioread16(const void __iomem *address)
{
	return bridge_bar_read(address, 2) & U16_MAX;
}

unsigned int ioread32(const void __iomem *address)
{
	return bridge_bar_read(address, 4);
}

void iowrite8(u8 value, void __iomem *address)
{
	bridge_bar_write(address, 1, value);
}

void iowrite16(u16 value, void __iomem *address)
{
	bridge_bar_write(address, 2, value);
}

void iowrite32(u32 value, void __iomem *address)
{
	bridge_bar_write(address, 4, value);
}

static int dma_device_valid(struct device *device)
{
	return bridge_context_valid(kobox_pci_bridge.context) &&
	       kobox_pci_bridge.device &&
	       device == &kobox_pci_bridge.device->dev;
}

static u32 dma_direction(enum dma_data_direction direction)
{
	switch (direction) {
	case DMA_TO_DEVICE:
		return KB2_DMA_DOMAIN_DIRECTION_TO_DEVICE;
	case DMA_FROM_DEVICE:
		return KB2_DMA_DOMAIN_DIRECTION_FROM_DEVICE;
	case DMA_BIDIRECTIONAL:
		return KB2_DMA_DOMAIN_DIRECTION_BIDIRECTIONAL;
	default:
		return 0;
	}
}

void *dma_alloc_attrs(struct device *device, size_t size,
		      dma_addr_t *device_address, gfp_t flags,
		      unsigned long attributes)
{
	const struct kobox_dma_domain_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.dma.operations;
	struct kobox_linux_dma_mapping *record;
	void *allocation = NULL;
	void *mapping = NULL;
	void *cpu_address = NULL;
	u64 address;
	size_t alignment;

	(void)flags;
	(void)attributes;
	if (!dma_device_valid(device) || !size || !device_address || !operations)
		return NULL;
	alignment = kobox_pci_bridge.resources.dma_constraints.minimum_alignment;
	record = kmalloc(sizeof(*record), GFP_KERNEL);
	if (!record || operations->allocation_create(
			kobox_pci_bridge.resources.dma.object, size, alignment,
			KB2_DMA_DOMAIN_ALLOCATION_FLAG_CPU_MAP |
				KB2_DMA_DOMAIN_ALLOCATION_FLAG_ZERO |
				KB2_DMA_DOMAIN_ALLOCATION_FLAG_COHERENT,
			&allocation, &cpu_address) ||
	    operations->mapping_create(kobox_pci_bridge.resources.dma.object,
				       allocation, 0, size,
				       KB2_DMA_DOMAIN_DIRECTION_BIDIRECTIONAL,
				       &mapping, &address)) {
		if (allocation)
			operations->allocation_release(
				kobox_pci_bridge.resources.dma.object, allocation);
		kfree(record);
		return NULL;
	}
	*record = (struct kobox_linux_dma_mapping){
		.allocation = allocation,
		.mapping = mapping,
		.cpu_address = cpu_address,
		.device_address = address,
		.length = size,
		.direction = KB2_DMA_DOMAIN_DIRECTION_BIDIRECTIONAL,
	};
	spin_lock(&kobox_pci_bridge.mapping_lock);
	list_add_tail(&record->node, &kobox_pci_bridge.dma_mappings);
	spin_unlock(&kobox_pci_bridge.mapping_lock);
	*device_address = address;
	return cpu_address;
}

void dma_free_attrs(struct device *device, size_t size, void *cpu_address,
		    dma_addr_t device_address, unsigned long attributes)
{
	const struct kobox_dma_domain_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.dma.operations;
	struct kobox_linux_dma_mapping *record;
	struct kobox_linux_dma_mapping *found = NULL;

	(void)attributes;
	if (!dma_device_valid(device) || !operations)
		BUG();
	spin_lock(&kobox_pci_bridge.mapping_lock);
	list_for_each_entry(record, &kobox_pci_bridge.dma_mappings, node) {
		if (record->cpu_address != cpu_address ||
		    record->device_address != device_address ||
		    record->length != size || !record->allocation)
			continue;
		list_del(&record->node);
		found = record;
		break;
	}
	spin_unlock(&kobox_pci_bridge.mapping_lock);
	if (!found || operations->mapping_release(
			      kobox_pci_bridge.resources.dma.object,
			      found->mapping) ||
	    operations->allocation_release(kobox_pci_bridge.resources.dma.object,
					   found->allocation))
		BUG();
	kfree(found);
}

dma_addr_t dma_map_page_attrs(struct device *device, struct page *page,
			      size_t offset, size_t size,
			      enum dma_data_direction direction,
			      unsigned long attributes)
{
	const struct kobox_dma_domain_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.dma.operations;
	struct kobox_linux_dma_mapping *record;
	void *cpu_address;
	void *mapping;
	u64 address;
	u32 domain_direction = dma_direction(direction);

	if (!dma_device_valid(device) || !page || !size || !domain_direction ||
	    (attributes & DMA_ATTR_MMIO) || !operations)
		return DMA_MAPPING_ERROR;
	cpu_address = (u8 *)page_to_virt(page) + offset;
	record = kmalloc(sizeof(*record), GFP_ATOMIC);
	if (!record || operations->mapping_create_span(
			kobox_pci_bridge.resources.dma.object, cpu_address, size,
			domain_direction, &mapping, &address)) {
		kfree(record);
		return DMA_MAPPING_ERROR;
	}
	*record = (struct kobox_linux_dma_mapping){
		.mapping = mapping,
		.cpu_address = cpu_address,
		.device_address = address,
		.length = size,
		.direction = domain_direction,
	};
	spin_lock(&kobox_pci_bridge.mapping_lock);
	list_add_tail(&record->node, &kobox_pci_bridge.dma_mappings);
	spin_unlock(&kobox_pci_bridge.mapping_lock);
	return address;
}

void dma_unmap_page_attrs(struct device *device, dma_addr_t device_address,
			  size_t size, enum dma_data_direction direction,
			  unsigned long attributes)
{
	const struct kobox_dma_domain_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.dma.operations;
	struct kobox_linux_dma_mapping *record;
	struct kobox_linux_dma_mapping *found = NULL;
	u32 domain_direction = dma_direction(direction);

	(void)attributes;
	if (!dma_device_valid(device) || !operations || !domain_direction)
		BUG();
	spin_lock(&kobox_pci_bridge.mapping_lock);
	list_for_each_entry(record, &kobox_pci_bridge.dma_mappings, node) {
		if (record->device_address != device_address ||
		    record->length != size || record->direction != domain_direction ||
		    record->allocation)
			continue;
		list_del(&record->node);
		found = record;
		break;
	}
	spin_unlock(&kobox_pci_bridge.mapping_lock);
	if (!found || operations->mapping_release(
			      kobox_pci_bridge.resources.dma.object,
			      found->mapping))
		BUG();
	kfree(found);
}

static struct kobox_linux_dma_mapping *dma_mapping_find(dma_addr_t address,
							 size_t size)
{
	struct kobox_linux_dma_mapping *record;

	list_for_each_entry(record, &kobox_pci_bridge.dma_mappings, node) {
		if (address >= record->device_address &&
		    address - record->device_address <= record->length &&
		    size <= record->length - (address - record->device_address))
			return record;
	}
	return NULL;
}

static void dma_sync_one(dma_addr_t address, size_t size, int for_cpu)
{
	const struct kobox_dma_domain_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.dma.operations;
	struct kobox_linux_dma_mapping *record;
	size_t offset;
	int status;

	spin_lock(&kobox_pci_bridge.mapping_lock);
	record = dma_mapping_find(address, size);
	if (!record) {
		spin_unlock(&kobox_pci_bridge.mapping_lock);
		BUG();
	}
	offset = address - record->device_address;
	status = for_cpu ?
		operations->sync_for_cpu(kobox_pci_bridge.resources.dma.object,
					 record->mapping, offset, size) :
		operations->sync_for_device(kobox_pci_bridge.resources.dma.object,
					    record->mapping, offset, size);
	spin_unlock(&kobox_pci_bridge.mapping_lock);
	if (status)
		BUG();
}

void __dma_sync_single_for_cpu(struct device *device, dma_addr_t address,
			       size_t size,
			       enum dma_data_direction direction)
{
	if (!dma_device_valid(device) || !dma_direction(direction))
		BUG();
	dma_sync_one(address, size, 1);
}

void __dma_sync_single_for_device(struct device *device, dma_addr_t address,
				  size_t size,
				  enum dma_data_direction direction)
{
	if (!dma_device_valid(device) || !dma_direction(direction))
		BUG();
	dma_sync_one(address, size, 0);
}

bool __dma_need_sync(struct device *device, dma_addr_t address)
{
	(void)address;
	return dma_device_valid(device) &&
	       !kobox_pci_bridge.resources.dma_constraints.coherent;
}

size_t dma_max_mapping_size(struct device *device)
{
	return dma_device_valid(device) ?
		kobox_pci_bridge.resources.dma_constraints.maximum_segment_length :
		0;
}

static int bridge_identity_valid(
	const struct kobox_linux_device_pci_resources *resources)
{
	const struct kobox_pci_function_identity *identity =
		&resources->pci_identity;
	const struct kobox_pci_function_resource_operations *operations =
		(const void *)resources->pci.operations;
	kobox_abi_u32 class_revision;
	kobox_abi_u32 value;

	if (identity->segment > 0xffff || identity->device > 31 ||
	    identity->function > 7 ||
	    operations->config_read(resources->pci.object, PCI_VENDOR_ID, 2,
				    &value) ||
	    value != identity->vendor_id ||
	    operations->config_read(resources->pci.object, PCI_DEVICE_ID, 2,
				    &value) ||
	    value != identity->device_id ||
	    operations->config_read(resources->pci.object,
				    PCI_SUBSYSTEM_VENDOR_ID, 2, &value) ||
	    value != identity->subsystem_vendor_id ||
	    operations->config_read(resources->pci.object,
				    PCI_SUBSYSTEM_ID, 2, &value) ||
	    value != identity->subsystem_device_id ||
	    operations->config_read(resources->pci.object,
				    PCI_CLASS_REVISION, 4, &class_revision) ||
	    (class_revision >> 8) != identity->class_code ||
	    (class_revision & 0xff) != identity->revision)
		return 0;
	return 1;
}

static int bridge_prepare_windows(struct pci_host_bridge *host_bridge)
{
	const struct kobox_pci_function_resource_operations *operations =
		(const void *)kobox_pci_bridge.resources.pci.operations;
	unsigned int bar;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		struct resource *window = &kobox_pci_bridge.bar_windows[bar];
		kobox_abi_u64 length;
		kobox_abi_u64 start;
		kobox_abi_u32 bar_flags;
		kobox_abi_u32 low;
		unsigned long resource_flags;

		if (operations->bar_info(kobox_pci_bridge.resources.pci.object,
					 (kobox_abi_u32)bar, &length,
					 &bar_flags))
			continue;
		if (!length || (length & (length - 1)) ||
		    operations->config_read(
			    kobox_pci_bridge.resources.pci.object,
			    PCI_BASE_ADDRESS_0 + bar * 4, 4, &low))
			return -EINVAL;
		if (bar_flags & KB2_PCI_FUNCTION_BAR_FLAG_IO) {
			start = low & PCI_BASE_ADDRESS_IO_MASK;
			resource_flags = IORESOURCE_IO;
		} else if (bar_flags & KB2_PCI_FUNCTION_BAR_FLAG_MEMORY) {
			start = low & PCI_BASE_ADDRESS_MEM_MASK;
			resource_flags = IORESOURCE_MEM;
			if (bar_flags & KB2_PCI_FUNCTION_BAR_FLAG_PREFETCHABLE)
				resource_flags |= IORESOURCE_PREFETCH;
			if (bar_flags & KB2_PCI_FUNCTION_BAR_FLAG_64_BIT) {
				kobox_abi_u32 high;

				if (bar + 1 >= PCI_STD_NUM_BARS ||
				    operations->config_read(
					    kobox_pci_bridge.resources.pci.object,
					    PCI_BASE_ADDRESS_0 + (bar + 1) * 4,
					    4, &high))
					return -EINVAL;
				start |= (kobox_abi_u64)high << 32;
			}
		} else {
			return -EINVAL;
		}
		if (!start || start > RESOURCE_SIZE_MAX - (length - 1) ||
		    (start & (length - 1)))
			return -EINVAL;
		*window = (struct resource){
			.name = "kobox-pci-bar",
			.start = start,
			.end = start + length - 1,
			.flags = resource_flags,
		};
		pci_add_resource(&host_bridge->windows, window);
		if (bar_flags & KB2_PCI_FUNCTION_BAR_FLAG_64_BIT)
			bar++;
	}
	return 0;
}

static void bridge_remove_root(void)
{
	struct pci_bus *bus;

	if (!kobox_pci_bridge.host_bridge)
		return;
	bus = kobox_pci_bridge.host_bridge->bus;
	if (kobox_pci_bridge.device) {
		pci_dev_put(kobox_pci_bridge.device);
		kobox_pci_bridge.device = NULL;
	}
	if (bus) {
		pci_lock_rescan_remove();
		pci_stop_root_bus(bus);
		pci_remove_root_bus(bus);
		pci_unlock_rescan_remove();
	} else {
		pci_free_host_bridge(kobox_pci_bridge.host_bridge);
		kobox_pci_bridge.host_bridge = NULL;
		return;
	}
	pci_free_host_bridge(kobox_pci_bridge.host_bridge);
	kobox_pci_bridge.host_bridge = NULL;
}

int kobox_linux_device_pci_bridge_probe(
	const struct kobox_module_context *context)
{
	struct kobox_linux_device_pci_resources resources;
	struct pci_host_bridge *host_bridge;
	struct pci_dev *device;
	int status;

	if (!context || kobox_pci_bridge.context ||
	    !kobox_linux_device_pci_core_active(context) ||
	    kobox_linux_device_pci_probe_resources(context, &resources) ||
	    !bridge_identity_valid(&resources))
		return -EINVAL;
	host_bridge = pci_alloc_host_bridge(0);
	if (!host_bridge)
		return -ENOMEM;
	kobox_pci_bridge.context = context;
	kobox_pci_bridge.resources = resources;
	kobox_pci_bridge.host_bridge = host_bridge;
	INIT_LIST_HEAD(&kobox_pci_bridge.mappings);
	INIT_LIST_HEAD(&kobox_pci_bridge.dma_mappings);
	spin_lock_init(&kobox_pci_bridge.mapping_lock);
	host_bridge->busnr = resources.pci_identity.bus;
	host_bridge->domain_nr = resources.pci_identity.segment;
	host_bridge->ops = &kobox_pci_operations;
	host_bridge->sysdata = &kobox_pci_bridge;
	host_bridge->preserve_config = 1;
	status = bridge_prepare_windows(host_bridge);
	if (status) {
		pci_free_host_bridge(host_bridge);
		kobox_pci_bridge = (struct kobox_linux_pci_bridge){ 0 };
		return status;
	}

	pci_lock_rescan_remove();
	status = pci_scan_root_bus_bridge(host_bridge);
	pci_unlock_rescan_remove();
	if (status) {
		pci_free_host_bridge(host_bridge);
		kobox_pci_bridge = (struct kobox_linux_pci_bridge){ 0 };
		return status;
	}
	device = pci_get_slot(
		host_bridge->bus,
		PCI_DEVFN(resources.pci_identity.device,
			  resources.pci_identity.function));
	if (!device) {
		status = -ENODEV;
		goto fail;
	}
	kobox_pci_bridge.device = device;
	status = bridge_irqs_init(device);
	if (status)
		goto fail;
	pci_lock_rescan_remove();
	pci_bus_claim_resources(host_bridge->bus);
	pci_bus_add_devices(host_bridge->bus);
	pci_unlock_rescan_remove();
	return 0;

fail:
	bridge_remove_root();
	bridge_irqs_remove();
	kobox_pci_bridge = (struct kobox_linux_pci_bridge){ 0 };
	return status;
}

int kobox_linux_device_pci_bridge_bound(
	const struct kobox_module_context *context)
{
	if (!bridge_context_valid(context) || !kobox_pci_bridge.device)
		return -EINVAL;
	return kobox_pci_bridge.device->dev.driver ? 1 : 0;
}

int kobox_linux_device_pci_bridge_remove(
	const struct kobox_module_context *context)
{
	if (!bridge_context_valid(context) || kobox_pci_bridge.mapping_count ||
	    !list_empty(&kobox_pci_bridge.dma_mappings))
		return -EINVAL;
	bridge_remove_root();
	bridge_irqs_remove();
	kobox_pci_bridge = (struct kobox_linux_pci_bridge){ 0 };
	return 0;
}
