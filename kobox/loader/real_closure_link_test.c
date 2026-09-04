// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "link_plan_loader.h"
#include "../provider/core_lifecycle.h"
#include "../provider/device_pci_lifecycle.h"
#include "../runtime/memory_resource_interface.h"

#include <kobox2/closure_layout.h>
#include <kobox2/dma_domain_layout.h>
#include <kobox2/irq_endpoint_layout.h>
#include <kobox2/memory_arena_layout.h>
#include <kobox2/pci_function_layout.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern const struct kobox_link_plan kobox_generated_link_plan;

#define TEST_GENERATION UINT64_C(71)
#define TEST_MEMORY_OBJECT UINT64_C(200)
#define TEST_PCI_OBJECT UINT64_C(201)
#define TEST_DMA_OBJECT UINT64_C(202)
#define TEST_IRQ_OBJECT UINT64_C(203)
#define TEST_IRQ_COUNT 4u
#define TEST_ARENA_SIZE (64u * 1024u * 1024u)
#define TEST_BAR_SIZE 4096u
#define TEST_BAR_ADDRESS UINT32_C(0x10000000)
#define TEST_VIRTQUEUE_COUNT 2u
#define TEST_VIRTQUEUE_SIZE 256u
#define TEST_VIRTIO_COMMON_OFFSET 0x000u
#define TEST_VIRTIO_NOTIFY_OFFSET 0x100u
#define TEST_VIRTIO_ISR_OFFSET 0x200u
#define TEST_VIRTIO_DEVICE_OFFSET 0x300u

#define TEST_VIRTIO_F_INDIRECT_DESC 28u
#define TEST_VIRTIO_F_EVENT_IDX 29u
#define TEST_VIRTIO_F_VERSION_1 32u
#define TEST_VIRTIO_F_ACCESS_PLATFORM 33u
#define TEST_VIRTQ_DESC_F_NEXT 1u
#define TEST_VIRTQ_DESC_F_WRITE 2u
#define TEST_VIRTQ_DESC_F_INDIRECT 4u
#define TEST_VIRTIO_GPU_CMD_GET_CAPSET_INFO 0x0108u
#define TEST_VIRTIO_GPU_RESP_OK_CAPSET_INFO 0x1102u

#if defined(__clang__)
#define KOBOX_MANUAL_ELF_CALL __attribute__((no_sanitize("function")))
#else
#define KOBOX_MANUAL_ELF_CALL
#endif

struct test_memory_resource {
	void *address;
	size_t length;
};

struct test_dma_allocation {
	void *memory;
	size_t length;
};

struct test_dma_mapping {
	struct test_dma_allocation *allocation;
	void *cpu_address;
	uint64_t device_address;
	size_t length;
	uint32_t direction;
	struct test_dma_mapping *next;
};

struct test_virtqueue {
	uint64_t descriptor_address;
	uint64_t driver_address;
	uint64_t device_address;
	uint16_t enabled;
	uint16_t last_available;
};

struct test_virtq_descriptor {
	uint64_t address;
	uint32_t length;
	uint16_t flags;
	uint16_t next;
};

struct test_virtq_used_element {
	uint32_t id;
	uint32_t length;
};

struct test_virtio_gpu_ctrl_header {
	uint32_t type;
	uint32_t flags;
	uint64_t fence_id;
	uint32_t context_id;
	uint8_t ring_index;
	uint8_t padding[3];
};

struct test_virtio_gpu_capset_response {
	struct test_virtio_gpu_ctrl_header header;
	uint32_t capset_id;
	uint32_t maximum_version;
	uint32_t maximum_size;
	uint32_t padding;
};

struct test_irq_resource {
	uint64_t object_id;
	pthread_mutex_t lock;
	pthread_cond_t condition;
	kobox_irq_endpoint_handler_fn handler;
	void *handler_argument;
	uint32_t enabled;
	uint32_t running;
};

struct test_device_resources {
	uint8_t config[KB2_PCI_FUNCTION_CONFIG_SPACE_SIZE];
	uint8_t bar[TEST_BAR_SIZE];
	struct test_memory_resource memory;
	struct test_irq_resource irqs[TEST_IRQ_COUNT];
	size_t allocations;
	size_t mappings;
	size_t bar_mappings;
	uint32_t probe_registers;
	uint32_t device_feature_select;
	uint32_t driver_feature_select;
	uint32_t driver_features[4];
	uint16_t queue_select;
	uint8_t device_status;
	uint8_t isr_status;
	struct test_virtqueue queues[TEST_VIRTQUEUE_COUNT];
	struct test_dma_mapping *dma_mapping_list;
	uint64_t next_dma_address;
	size_t queue_notifications;
	size_t queue_completions;
	size_t irq_deliveries;
};

static struct test_device_resources test_resources;

static int virtqueue_notify(uint16_t queue_index);

static int memory_mapped_range(void *object, void **address_out,
			       size_t *length_out)
{
	struct test_memory_resource *memory = object;

	if (memory != &test_resources.memory || !address_out || !length_out)
		return -1;
	*address_out = memory->address;
	*length_out = memory->length;
	return 0;
}

static int pci_identity(void *object,
			struct kobox_pci_function_identity *identity_out)
{
	if (object != &test_resources || !identity_out)
		return -1;
	*identity_out = (struct kobox_pci_function_identity){
		.generation = TEST_GENERATION,
		.object_id = TEST_PCI_OBJECT,
		.segment = 0,
		.bus = 2,
		.device = 0,
		.function = 0,
		.revision = 1,
		.vendor_id = 0x1af4,
		.device_id = 0x1050,
		.subsystem_vendor_id = 0x1af4,
		.subsystem_device_id = 16,
		.class_code = 0x038000,
	};
	return 0;
}

static int pci_config_range(uint32_t offset, uint32_t width)
{
	return (width == 1 || width == 2 || width == 4) &&
	       !(offset & (width - 1)) &&
	       offset <= KB2_PCI_FUNCTION_CONFIG_SPACE_SIZE - width;
}

static int pci_probe_register(uint32_t offset)
{
	if (offset >= 0x10 && offset <= 0x24 && !(offset & 3))
		return (int)((offset - 0x10) / 4);
	if (offset == 0x30)
		return 6;
	return -1;
}

static int pci_config_read(void *object, uint32_t offset, uint32_t width,
			   uint32_t *value_out)
{
	uint32_t value = 0;
	int probe;

	if (object != &test_resources || !value_out ||
	    !pci_config_range(offset, width))
		return -1;
	probe = width == 4 ? pci_probe_register(offset) : -1;
	if (probe >= 0 && (test_resources.probe_registers & (1u << probe))) {
		*value_out = probe == 0 ? ~(TEST_BAR_SIZE - 1u) & 0xfffffff0u : 0;
		return 0;
	}
	memcpy(&value, test_resources.config + offset, width);
	*value_out = value;
	return 0;
}

static int pci_config_write(void *object, uint32_t offset, uint32_t width,
			    uint32_t value)
{
	int probe;

	if (object != &test_resources || !pci_config_range(offset, width))
		return -1;
	probe = width == 4 ? pci_probe_register(offset) : -1;
	if (probe >= 0 && value == UINT32_MAX) {
		test_resources.probe_registers |= 1u << probe;
		return 0;
	}
	if (probe >= 0)
		test_resources.probe_registers &= ~(1u << probe);
	memcpy(test_resources.config + offset, &value, width);
	return 0;
}

static int pci_bar_info(void *object, uint32_t bar, uint64_t *length_out,
			uint32_t *flags_out)
{
	if (object != &test_resources || bar || !length_out || !flags_out)
		return -1;
	*length_out = TEST_BAR_SIZE;
	*flags_out = KB2_PCI_FUNCTION_BAR_FLAG_MEMORY;
	return 0;
}

static int pci_bar_map(void *object, uint32_t bar, uint64_t offset,
		       size_t length, uint32_t protection, void **address_out)
{
	if (object != &test_resources || bar || offset > TEST_BAR_SIZE ||
	    !length || length > TEST_BAR_SIZE - offset ||
	    protection != (KB2_PCI_FUNCTION_MAP_PROTECTION_READ |
			   KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE) ||
	    !address_out)
		return -1;
	*address_out = test_resources.bar + offset;
	test_resources.bar_mappings++;
	return 0;
}

static int pci_bar_unmap(void *object, void *address, size_t length)
{
	uintptr_t start = (uintptr_t)test_resources.bar;
	uintptr_t value = (uintptr_t)address;

	if (object != &test_resources || !address || !length ||
	    value < start || value > start + TEST_BAR_SIZE - length ||
	    !test_resources.bar_mappings)
		return -1;
	test_resources.bar_mappings--;
	return 0;
}

static uint32_t virtio_device_features(uint32_t select)
{
	if (select == 0)
		return (UINT32_C(1) << TEST_VIRTIO_F_INDIRECT_DESC) |
		       (UINT32_C(1) << TEST_VIRTIO_F_EVENT_IDX) | UINT32_C(1);
	if (select == 1)
		return (UINT32_C(1) << (TEST_VIRTIO_F_VERSION_1 - 32)) |
		       (UINT32_C(1) << (TEST_VIRTIO_F_ACCESS_PLATFORM - 32));
	return 0;
}

static int pci_bar_range(uint64_t offset, uint32_t width)
{
	return (width == 1 || width == 2 || width == 4) &&
	       !(offset & (width - 1)) && offset <= TEST_BAR_SIZE - width;
}

static int pci_bar_read(void *object, uint32_t bar, uint64_t offset,
			uint32_t width, uint32_t *value_out)
{
	uint32_t value = 0;
	uint64_t common = offset - TEST_VIRTIO_COMMON_OFFSET;
	struct test_virtqueue *queue;

	if (object != &test_resources || bar || !value_out ||
	    !pci_bar_range(offset, width))
		return -1;
	queue = test_resources.queue_select < TEST_VIRTQUEUE_COUNT ?
		&test_resources.queues[test_resources.queue_select] : NULL;
	if (common == 4 && width == 4)
		value = virtio_device_features(test_resources.device_feature_select);
	else if (common == 12 && width == 4 &&
		 test_resources.driver_feature_select < 4)
		value = test_resources.driver_features[
			test_resources.driver_feature_select];
	else if (common == 18 && width == 2)
		value = TEST_VIRTQUEUE_COUNT;
	else if (common == 20 && width == 1)
		value = test_resources.device_status;
	else if (common == 21 && width == 1)
		value = 0;
	else if (common == 24 && width == 2)
		value = queue ? TEST_VIRTQUEUE_SIZE : 0;
	else if (common == 26 && width == 2)
		value = UINT16_MAX;
	else if (common == 28 && width == 2)
		value = queue ? queue->enabled : 0;
	else if (common == 30 && width == 2)
		value = queue ? test_resources.queue_select : 0;
	else if (offset == TEST_VIRTIO_ISR_OFFSET && width == 1) {
		value = test_resources.isr_status;
		test_resources.isr_status = 0;
	} else
		memcpy(&value, test_resources.bar + offset, width);
	*value_out = value;
	return 0;
}

static int pci_bar_write(void *object, uint32_t bar, uint64_t offset,
			 uint32_t width, uint32_t value)
{
	uint64_t common = offset - TEST_VIRTIO_COMMON_OFFSET;
	struct test_virtqueue *queue;

	if (object != &test_resources || bar || !pci_bar_range(offset, width))
		return -1;
	queue = test_resources.queue_select < TEST_VIRTQUEUE_COUNT ?
		&test_resources.queues[test_resources.queue_select] : NULL;
	if (common == 0 && width == 4)
		test_resources.device_feature_select = value;
	else if (common == 8 && width == 4)
		test_resources.driver_feature_select = value;
	else if (common == 12 && width == 4 &&
		 test_resources.driver_feature_select < 4)
		test_resources.driver_features[
			test_resources.driver_feature_select] = value;
	else if (common == 20 && width == 1) {
		test_resources.device_status = value;
		if (!value) {
			size_t index;

			for (index = 0; index < TEST_VIRTQUEUE_COUNT; index++)
				test_resources.queues[index].enabled = 0;
		}
	} else if (common == 22 && width == 2) {
		test_resources.queue_select = value;
	} else if (common == 28 && width == 2 && queue) {
		queue->enabled = value;
	} else if (common >= 32 && common <= 52 && width == 4 && queue) {
		uint64_t *field;

		if (common < 40)
			field = &queue->descriptor_address;
		else if (common < 48)
			field = &queue->driver_address;
		else
			field = &queue->device_address;
		if (common & 4)
			*field = (*field & UINT32_MAX) | ((uint64_t)value << 32);
		else
			*field = (*field & (UINT64_C(0xffffffff) << 32)) | value;
	} else if (offset >= TEST_VIRTIO_NOTIFY_OFFSET &&
		   offset < TEST_VIRTIO_ISR_OFFSET &&
		   (width == 2 || width == 4)) {
		return virtqueue_notify((uint16_t)value);
	} else {
		memcpy(test_resources.bar + offset, &value, width);
	}
	return 0;
}

static int dma_constraints(
	void *object, struct kobox_dma_domain_constraints *constraints_out)
{
	if (object != &test_resources || !constraints_out)
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
	struct test_dma_allocation *allocation;
	size_t allocated_length;

	if (object != &test_resources || !length || alignment != 4096 ||
	    flags & ~(KB2_DMA_DOMAIN_ALLOCATION_FLAG_CPU_MAP |
		      KB2_DMA_DOMAIN_ALLOCATION_FLAG_ZERO |
		      KB2_DMA_DOMAIN_ALLOCATION_FLAG_COHERENT) ||
	    !allocation_out || !cpu_address_out ||
	    length > SIZE_MAX - (alignment - 1))
		return -1;
	allocation = calloc(1, sizeof(*allocation));
	allocated_length = (length + alignment - 1) & ~(alignment - 1);
	if (!allocation ||
	    !(allocation->memory = aligned_alloc(alignment, allocated_length))) {
		free(allocation);
		return -1;
	}
	if (flags & KB2_DMA_DOMAIN_ALLOCATION_FLAG_ZERO)
		memset(allocation->memory, 0, allocated_length);
	allocation->length = length;
	test_resources.allocations++;
	*allocation_out = allocation;
	*cpu_address_out = allocation->memory;
	return 0;
}

static int dma_allocation_release(void *object, void *opaque)
{
	struct test_dma_allocation *allocation = opaque;

	if (object != &test_resources || !allocation ||
	    !test_resources.allocations)
		return -1;
	free(allocation->memory);
	free(allocation);
	test_resources.allocations--;
	return 0;
}

static int dma_mapping_create(void *object, void *opaque, size_t offset,
			      size_t length, uint32_t direction,
			      void **mapping_out, uint64_t *address_out)
{
	struct test_dma_allocation *allocation = opaque;
	struct test_dma_mapping *mapping;

	if (object != &test_resources || !allocation ||
	    offset > allocation->length || length > allocation->length - offset ||
	    !length || direction < KB2_DMA_DOMAIN_DIRECTION_TO_DEVICE ||
	    direction > KB2_DMA_DOMAIN_DIRECTION_BIDIRECTIONAL || !mapping_out ||
	    !address_out)
		return -1;
	mapping = malloc(sizeof(*mapping));
	if (!mapping)
		return -1;
	*mapping = (struct test_dma_mapping){
		.allocation = allocation,
		.cpu_address = (uint8_t *)allocation->memory + offset,
		.length = length,
		.direction = direction,
	};
	if (test_resources.next_dma_address >
	    UINT64_MAX - ((length + 4095) & ~UINT64_C(4095))) {
		free(mapping);
		return -1;
	}
	mapping->device_address = test_resources.next_dma_address;
	test_resources.next_dma_address +=
		(length + 4095) & ~UINT64_C(4095);
	mapping->next = test_resources.dma_mapping_list;
	test_resources.dma_mapping_list = mapping;
	test_resources.mappings++;
	*mapping_out = mapping;
	*address_out = mapping->device_address;
	return 0;
}

static int dma_mapping_create_span(void *object, void *cpu_address,
				   size_t length, uint32_t direction,
				   void **mapping_out, uint64_t *address_out)
{
	struct test_dma_mapping *mapping;

	if (object != &test_resources || !cpu_address || !length ||
	    direction < KB2_DMA_DOMAIN_DIRECTION_TO_DEVICE ||
	    direction > KB2_DMA_DOMAIN_DIRECTION_BIDIRECTIONAL ||
	    !mapping_out || !address_out ||
	    test_resources.next_dma_address >
		    UINT64_MAX - ((length + 4095) & ~UINT64_C(4095)))
		return -1;
	mapping = calloc(1, sizeof(*mapping));
	if (!mapping)
		return -1;
	*mapping = (struct test_dma_mapping){
		.cpu_address = cpu_address,
		.device_address = test_resources.next_dma_address,
		.length = length,
		.direction = direction,
		.next = test_resources.dma_mapping_list,
	};
	test_resources.next_dma_address +=
		(length + 4095) & ~UINT64_C(4095);
	test_resources.dma_mapping_list = mapping;
	test_resources.mappings++;
	*mapping_out = mapping;
	*address_out = mapping->device_address;
	return 0;
}

static int dma_mapping_release(void *object, void *mapping)
{
	struct test_dma_mapping **link;
	int found = 0;

	if (object != &test_resources || !mapping || !test_resources.mappings)
		return -1;
	for (link = &test_resources.dma_mapping_list; *link;
	     link = &(*link)->next) {
		if (*link != mapping)
			continue;
		*link = (*link)->next;
		found = 1;
		break;
	}
	if (!found)
		return -1;
	free(mapping);
	test_resources.mappings--;
	return 0;
}

static int dma_sync(void *object, void *opaque, size_t offset, size_t length)
{
	struct test_dma_mapping *mapping = opaque;

	return object == &test_resources && mapping &&
	       offset <= mapping->length && length <= mapping->length - offset ?
		       0 : -1;
}

static int dma_drain(void *object, size_t *allocations_out,
		     size_t *mappings_out)
{
	if (object != &test_resources || !allocations_out || !mappings_out)
		return -1;
	*allocations_out = test_resources.allocations;
	*mappings_out = test_resources.mappings;
	return 0;
}

static int irq_identity(void *object, uint64_t *generation_out,
			uint64_t *object_id_out)
{
	struct test_irq_resource *irq = object;

	if (!irq || !generation_out || !object_id_out)
		return -1;
	*generation_out = TEST_GENERATION;
	*object_id_out = irq->object_id;
	return 0;
}

static int irq_raise(size_t index)
{
	struct test_irq_resource *irq;
	kobox_irq_endpoint_handler_fn handler;
	void *handler_argument;

	if (index >= TEST_IRQ_COUNT)
		return -1;
	irq = &test_resources.irqs[index];
	pthread_mutex_lock(&irq->lock);
	if (!irq->enabled || !irq->handler) {
		pthread_mutex_unlock(&irq->lock);
		return -1;
	}
	irq->running++;
	handler = irq->handler;
	handler_argument = irq->handler_argument;
	test_resources.irq_deliveries++;
	pthread_mutex_unlock(&irq->lock);
	handler(handler_argument);
	pthread_mutex_lock(&irq->lock);
	irq->running--;
	pthread_cond_broadcast(&irq->condition);
	pthread_mutex_unlock(&irq->lock);
	return 0;
}

static void *dma_translate(uint64_t address, size_t length, int device_writes)
{
	struct test_dma_mapping *mapping;

	for (mapping = test_resources.dma_mapping_list; mapping;
	     mapping = mapping->next) {
		if (address < mapping->device_address ||
		    address - mapping->device_address > mapping->length ||
		    length > mapping->length -
			      (address - mapping->device_address) ||
		    (device_writes &&
		     mapping->direction == KB2_DMA_DOMAIN_DIRECTION_TO_DEVICE) ||
		    (!device_writes &&
		     mapping->direction == KB2_DMA_DOMAIN_DIRECTION_FROM_DEVICE))
			continue;
		return (uint8_t *)mapping->cpu_address +
		       (address - mapping->device_address);
	}
	return NULL;
}

static int virtqueue_descriptor_buffers(
	const struct test_virtq_descriptor *table, size_t table_count,
	uint16_t head, void **request_out, size_t *request_length_out,
	void **response_out, size_t *response_length_out)
{
	uint16_t index = head;
	size_t visited;

	*request_out = NULL;
	*response_out = NULL;
	for (visited = 0; visited < table_count; visited++) {
		const struct test_virtq_descriptor *descriptor;
		void *buffer;

		if (index >= table_count)
			return -1;
		descriptor = &table[index];
		if (!descriptor->length ||
		    (descriptor->flags & TEST_VIRTQ_DESC_F_INDIRECT))
			return -1;
		buffer = dma_translate(descriptor->address, descriptor->length,
				       descriptor->flags &
					       TEST_VIRTQ_DESC_F_WRITE);
		if (!buffer)
			return -1;
		if (descriptor->flags & TEST_VIRTQ_DESC_F_WRITE) {
			if (!*response_out) {
				*response_out = buffer;
				*response_length_out = descriptor->length;
			}
		} else if (!*request_out) {
			*request_out = buffer;
			*request_length_out = descriptor->length;
		}
		if (!(descriptor->flags & TEST_VIRTQ_DESC_F_NEXT))
			return *request_out && *response_out ? 0 : -1;
		index = descriptor->next;
	}
	return -1;
}

static int virtqueue_process_descriptor(struct test_virtqueue *queue,
					uint16_t head, uint32_t *used_length_out)
{
	struct test_virtq_descriptor *table;
	const struct test_virtq_descriptor *chain;
	size_t chain_count;
	void *request;
	void *response;
	size_t request_length = 0;
	size_t response_length = 0;
	uint32_t type;

	table = dma_translate(queue->descriptor_address,
			      TEST_VIRTQUEUE_SIZE * sizeof(*table), 0);
	if (!table || head >= TEST_VIRTQUEUE_SIZE)
		return -1;
	chain = table;
	chain_count = TEST_VIRTQUEUE_SIZE;
	if (table[head].flags & TEST_VIRTQ_DESC_F_INDIRECT) {
		if (!table[head].length || table[head].length % sizeof(*table))
			return -1;
		chain = dma_translate(table[head].address, table[head].length, 0);
		chain_count = table[head].length / sizeof(*table);
		head = 0;
		if (!chain)
			return -1;
	}
	if (virtqueue_descriptor_buffers(chain, chain_count, head, &request,
					 &request_length, &response,
					 &response_length) ||
	    request_length < sizeof(struct test_virtio_gpu_ctrl_header) ||
	    response_length < sizeof(struct test_virtio_gpu_capset_response))
		return -1;
	memcpy(&type, request, sizeof(type));
	if (type != TEST_VIRTIO_GPU_CMD_GET_CAPSET_INFO)
		return -1;
	{
		struct test_virtio_gpu_capset_response capset = {
			.header.type = TEST_VIRTIO_GPU_RESP_OK_CAPSET_INFO,
			.capset_id = 1,
			.maximum_version = 1,
			.maximum_size = 4096,
		};

		memcpy(response, &capset, sizeof(capset));
		*used_length_out = sizeof(capset);
	}
	return 0;
}

static int virtqueue_notify(uint16_t queue_index)
{
	struct test_virtqueue *queue;
	uint8_t *available;
	uint8_t *used;
	uint16_t available_index;

	if (queue_index >= TEST_VIRTQUEUE_COUNT)
		return -1;
	queue = &test_resources.queues[queue_index];
	if (!queue->enabled || !queue->descriptor_address ||
	    !queue->driver_address || !queue->device_address)
		return -1;
	available = dma_translate(queue->driver_address,
				 4 + TEST_VIRTQUEUE_SIZE * sizeof(uint16_t) +
					 sizeof(uint16_t), 0);
	used = dma_translate(queue->device_address,
			     4 + TEST_VIRTQUEUE_SIZE *
					     sizeof(struct test_virtq_used_element) +
				     sizeof(uint16_t), 1);
	if (!available || !used)
		return -1;
	atomic_thread_fence(memory_order_acquire);
	memcpy(&available_index, available + 2, sizeof(available_index));
	test_resources.queue_notifications++;
	while (queue->last_available != available_index) {
		struct test_virtq_used_element element;
		uint16_t head;
		uint16_t used_index;

		memcpy(&head,
		       available + 4 +
			       (queue->last_available % TEST_VIRTQUEUE_SIZE) *
				       sizeof(head),
		       sizeof(head));
		element.id = head;
		if (virtqueue_process_descriptor(queue, head, &element.length))
			return -1;
		memcpy(&used_index, used + 2, sizeof(used_index));
		memcpy(used + 4 +
			       (used_index % TEST_VIRTQUEUE_SIZE) * sizeof(element),
		       &element, sizeof(element));
		used_index++;
		atomic_thread_fence(memory_order_release);
		memcpy(used + 2, &used_index, sizeof(used_index));
		queue->last_available++;
		test_resources.queue_completions++;
	}
	test_resources.isr_status |= 1;
	return irq_raise(0);
}

static int irq_handler_register(void *object,
				kobox_irq_endpoint_handler_fn handler,
				void *argument)
{
	struct test_irq_resource *irq = object;

	if (!irq || !handler || irq->handler)
		return -1;
	pthread_mutex_lock(&irq->lock);
	irq->handler = handler;
	irq->handler_argument = argument;
	pthread_mutex_unlock(&irq->lock);
	return 0;
}

static int irq_handler_unregister(void *object,
				  kobox_irq_endpoint_handler_fn handler,
				  void *argument)
{
	struct test_irq_resource *irq = object;

	if (!irq)
		return -1;
	pthread_mutex_lock(&irq->lock);
	if (irq->enabled || irq->running || irq->handler != handler ||
	    irq->handler_argument != argument) {
		pthread_mutex_unlock(&irq->lock);
		return -1;
	}
	irq->handler = NULL;
	irq->handler_argument = NULL;
	pthread_mutex_unlock(&irq->lock);
	return 0;
}

static int irq_enable(void *object)
{
	struct test_irq_resource *irq = object;

	if (!irq)
		return -1;
	pthread_mutex_lock(&irq->lock);
	if (!irq->handler) {
		pthread_mutex_unlock(&irq->lock);
		return -1;
	}
	irq->enabled = 1;
	pthread_cond_signal(&irq->condition);
	pthread_mutex_unlock(&irq->lock);
	return 0;
}

static int irq_disable(void *object)
{
	struct test_irq_resource *irq = object;

	if (!irq)
		return -1;
	pthread_mutex_lock(&irq->lock);
	irq->enabled = 0;
	while (irq->running)
		pthread_cond_wait(&irq->condition, &irq->lock);
	pthread_mutex_unlock(&irq->lock);
	return 0;
}

static const struct kobox_memory_arena_resource_operations memory_operations = {
	.base = {
		.size = sizeof(memory_operations),
		.identity = KB2_MEMORY_ARENA_ABI_IDENTITY_BYTES,
	},
	.mapped_range = memory_mapped_range,
};

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
	.disable_and_synchronize = irq_disable,
};

static int resource_count(const struct kobox_module_context *context,
			  uint32_t slot_id, uint32_t *state_out,
			  size_t *count_out)
{
	if (!context || !state_out || !count_out ||
	    (context->node_id == 1 &&
	     slot_id != KOBOX_LINUX_CORE_MEMORY_SLOT_ID) ||
	    (context->node_id == 2 && (slot_id < 1 || slot_id > 3)) ||
	    (context->node_id != 1 && context->node_id != 2))
		return KOBOX_MODULE_RESOURCE_NOT_VISIBLE;
	*state_out = KOBOX_MODULE_RESOURCE_PRESENT_STATE;
	*count_out = context->node_id == 2 &&
			     slot_id == KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID ?
			     TEST_IRQ_COUNT : 1;
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_acquire(
	const struct kobox_module_context *context, uint32_t slot_id,
	size_t object_index, uint64_t required_rights,
	struct kobox_module_resource_handle *handle_out)
{
	static const uint64_t device_rights[] = {
		0,
		KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
		KB2_DMA_DOMAIN_REQUIRED_RIGHTS,
		KB2_IRQ_ENDPOINT_REQUIRED_RIGHTS,
	};
	static const uint64_t device_objects[] = {
		0, TEST_PCI_OBJECT, TEST_DMA_OBJECT,
		TEST_IRQ_OBJECT,
	};

	if (!context || !handle_out ||
	    (context->node_id == 1 &&
	     (slot_id != KOBOX_LINUX_CORE_MEMORY_SLOT_ID || object_index ||
	      required_rights != KB2_MEMORY_ARENA_REQUIRED_RIGHTS)) ||
	    (context->node_id == 2 &&
	     (slot_id < 1 || slot_id > 3 ||
	      required_rights != device_rights[slot_id])) ||
	    (context->node_id != 1 && context->node_id != 2) ||
	    (slot_id != KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID && object_index) ||
	    (slot_id == KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID &&
	     object_index >= TEST_IRQ_COUNT))
		return KOBOX_MODULE_RESOURCE_INVALID_ARGUMENT;
	*handle_out = (struct kobox_module_resource_handle){
		.generation = context->generation,
		.object_id = context->node_id == 1 ? TEST_MEMORY_OBJECT :
			     device_objects[slot_id] +
			     (slot_id == KOBOX_DEVICE_PCI_IRQ_ENDPOINT_SLOT_ID ?
				      object_index : 0),
	};
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_info(
	const struct kobox_module_context *context,
	struct kobox_module_resource_handle handle,
	struct kobox_module_resource_info *info_out)
{
	if (!context || handle.generation != context->generation || !info_out ||
	    handle.object_id < TEST_MEMORY_OBJECT ||
	    handle.object_id >= TEST_IRQ_OBJECT + TEST_IRQ_COUNT)
		return KOBOX_MODULE_RESOURCE_STALE;
	if (handle.object_id == TEST_MEMORY_OBJECT) {
		*info_out = (struct kobox_module_resource_info){
			.resource_type = KB2_CLOSURE_RESOURCE_MEMORY,
			.granted_rights = KB2_MEMORY_ARENA_REQUIRED_RIGHTS,
		};
	} else if (handle.object_id == TEST_PCI_OBJECT) {
		*info_out = (struct kobox_module_resource_info){
			.resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
			.granted_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
		};
	} else if (handle.object_id == TEST_DMA_OBJECT) {
		*info_out = (struct kobox_module_resource_info){
			.resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
			.granted_rights = KB2_DMA_DOMAIN_REQUIRED_RIGHTS,
		};
	} else {
		*info_out = (struct kobox_module_resource_info){
			.resource_type = KB2_CLOSURE_RESOURCE_NOTIFICATION,
			.granted_rights = KB2_IRQ_ENDPOINT_REQUIRED_RIGHTS,
		};
	}
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_bind(
	const struct kobox_module_context *context,
	struct kobox_module_resource_handle handle,
	const uint8_t digest[KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE],
	struct kobox_module_resource_binding *binding_out)
{
	static const uint8_t memory_digest[KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE] =
		KB2_MEMORY_ARENA_SCHEMA_SHA256_BYTES;
	static const uint8_t pci_digest[KB2_PCI_FUNCTION_SCHEMA_DIGEST_SIZE] =
		KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;
	static const uint8_t dma_digest[KB2_DMA_DOMAIN_SCHEMA_DIGEST_SIZE] =
		KB2_DMA_DOMAIN_SCHEMA_SHA256_BYTES;
	static const uint8_t irq_digest[KB2_IRQ_ENDPOINT_SCHEMA_DIGEST_SIZE] =
		KB2_IRQ_ENDPOINT_SCHEMA_SHA256_BYTES;
	const struct kobox_resource_interface_operations *operations;
	const uint8_t *expected;
	void *object;

	if (!context || handle.generation != context->generation || !digest ||
	    !binding_out)
		return KOBOX_MODULE_RESOURCE_STALE;
	if (handle.object_id == TEST_MEMORY_OBJECT) {
		expected = memory_digest;
		operations = &memory_operations.base;
		object = &test_resources.memory;
	} else if (handle.object_id == TEST_PCI_OBJECT) {
		expected = pci_digest;
		operations = &pci_operations.base;
		object = &test_resources;
	} else if (handle.object_id == TEST_DMA_OBJECT) {
		expected = dma_digest;
		operations = &dma_operations.base;
		object = &test_resources;
	} else if (handle.object_id >= TEST_IRQ_OBJECT &&
		   handle.object_id < TEST_IRQ_OBJECT + TEST_IRQ_COUNT) {
		expected = irq_digest;
		operations = &irq_operations.base;
		object = &test_resources.irqs[handle.object_id - TEST_IRQ_OBJECT];
	} else {
		return KOBOX_MODULE_RESOURCE_NOT_VISIBLE;
	}
	if (memcmp(digest, expected,
		   KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE))
		return KOBOX_MODULE_RESOURCE_INTERFACE;
	*binding_out = (struct kobox_module_resource_binding){
		.operations = operations,
		.object = object,
	};
	return KOBOX_MODULE_RESOURCE_OK;
}

static const struct kobox_module_runtime_operations runtime_operations = {
	.size = sizeof(runtime_operations),
	.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
	.resource_count = resource_count,
	.resource_acquire = resource_acquire,
	.resource_bind = resource_bind,
	.resource_info = resource_info,
};

static void config_store(uint32_t offset, uint32_t value, size_t width)
{
	memcpy(test_resources.config + offset, &value, width);
}

static void capability_store(uint32_t offset, uint8_t next, uint8_t type,
			     uint32_t bar_offset, uint32_t length)
{
	test_resources.config[offset] = 0x09;
	test_resources.config[offset + 1] = next;
	test_resources.config[offset + 2] = 16;
	test_resources.config[offset + 3] = type;
	test_resources.config[offset + 4] = 0;
	config_store(offset + 8, bar_offset, 4);
	config_store(offset + 12, length, 4);
}

static int test_resources_init(void)
{
	uint32_t capset_count = 1;
	size_t index;

	memset(&test_resources, 0, sizeof(test_resources));
	test_resources.next_dma_address = UINT64_C(0x20000000);
	test_resources.memory.length = TEST_ARENA_SIZE;
	test_resources.memory.address = mmap(
		NULL, TEST_ARENA_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (test_resources.memory.address == MAP_FAILED)
		return -1;
	for (index = 0; index < TEST_IRQ_COUNT; index++)
	{
		struct test_irq_resource *irq = &test_resources.irqs[index];

		irq->object_id = TEST_IRQ_OBJECT + index;
		if (pthread_mutex_init(&irq->lock, NULL) ||
		    pthread_cond_init(&irq->condition, NULL))
			return -1;
	}
	config_store(0x00, 0x10501af4, 4);
	config_store(0x04, 0x00100000, 4);
	config_store(0x08, 0x03800001, 4);
	config_store(0x10, TEST_BAR_ADDRESS, 4);
	config_store(0x2c, 0x00101af4, 4);
	test_resources.config[0x3c] = 1;
	test_resources.config[0x34] = 0x40;
	capability_store(0x40, 0x50, 1, 0x000, 0x100);
	capability_store(0x50, 0x64, 2, 0x100, 0x100);
	config_store(0x60, 4, 4);
	capability_store(0x64, 0x74, 3, 0x200, 1);
	capability_store(0x74, 0, 4, 0x300, 0x100);
	memcpy(test_resources.bar + TEST_VIRTIO_DEVICE_OFFSET + 12,
	       &capset_count, sizeof(capset_count));
	return 0;
}

static int node_has_export(const struct kobox_link_plan_node *node,
			   const char *name)
{
	size_t index;

	for (index = 0; index < node->export_count; index++) {
		if (!strcmp(node->exports[index].name, name))
			return 1;
	}
	return 0;
}

static int device_pci_probe_gate_present(const struct kobox_link_plan *plan)
{
	static const char *const required[] = {
		"kobox_linux_device_pci_bridge_bound",
		"kobox_linux_device_pci_bridge_probe",
		"kobox_linux_device_pci_bridge_remove",
		"kobox_linux_device_pci_cleanup",
		"kobox_linux_device_pci_core_active",
		"kobox_linux_device_pci_core_init",
		"kobox_linux_device_pci_init",
		"kobox_linux_device_pci_probe_resources",
		"kobox_linux_device_pci_quiesce",
	};
	size_t node_index;
	size_t export_index;

	for (node_index = 0; node_index < plan->node_count; node_index++) {
		const struct kobox_link_plan_node *node =
			&plan->nodes[node_index];

		if (strcmp(node->name, "device-pci.so"))
			continue;
		for (export_index = 0; export_index <
					 sizeof(required) / sizeof(required[0]);
		     export_index++) {
			if (!node_has_export(node, required[export_index]))
				return 0;
		}
		return 1;
	}
	return 0;
}

static int module_lifecycle_plan_valid(const struct kobox_link_plan *plan)
{
	size_t lifecycle_count = 0;
	int virtio_pci_found = 0;
	size_t node_index;

	for (node_index = 0; node_index < plan->node_count; node_index++) {
		const struct kobox_link_plan_node *node =
			&plan->nodes[node_index];

		if (node->kind != KOBOX_LINK_PLAN_RELOCATABLE_MODULE)
			continue;
		if (!!node->init_symbol != !!node->cleanup_symbol)
			return 0;
		if (!node->init_symbol)
			continue;
		if (strcmp(node->init_symbol, "init_module") ||
		    strcmp(node->cleanup_symbol, "cleanup_module") ||
		    !node_has_export(node, node->init_symbol) ||
		    !node_has_export(node, node->cleanup_symbol))
			return 0;
		lifecycle_count++;
		if (!strcmp(node->name, "drivers/virtio/virtio_pci.ko"))
			virtio_pci_found = 1;
	}
	return lifecycle_count == 5 && virtio_pci_found;
}

static int copy_all(int destination, int source)
{
	uint8_t buffer[64 * 1024];

	for (;;) {
		ssize_t received;

		do {
			received = read(source, buffer, sizeof(buffer));
		} while (received < 0 && errno == EINTR);
		if (received < 0)
			return -1;
		if (!received)
			return 0;
		{
			size_t offset = 0;

			while (offset < (size_t)received) {
				ssize_t written;

				do {
					written = write(destination, buffer + offset,
							(size_t)received - offset);
				} while (written < 0 && errno == EINTR);
				if (written <= 0)
					return -1;
				offset += (size_t)written;
			}
		}
	}
}

static int open_artifact(const char *root,
			 const struct kobox_link_plan_node *node)
{
	char *path;
	struct stat status;
	size_t root_length = strlen(root);
	size_t name_length = strlen(node->name);
	int destination = -1;
	int source = -1;

	if (root_length > SIZE_MAX - name_length - 2)
		return -1;
	path = malloc(root_length + name_length + 2);
	if (!path)
		return -1;
	memcpy(path, root, root_length);
	path[root_length] = '/';
	memcpy(path + root_length + 1, node->name, name_length + 1);
	source = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	free(path);
	if (source < 0 || fstat(source, &status) || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0)
		goto error;
	destination = memfd_create(node->name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (destination < 0 || copy_all(destination, source) ||
	    lseek(destination, 0, SEEK_SET) != 0 ||
	    fcntl(destination, F_ADD_SEALS,
		  F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE))
		goto error;
	close(source);
	return destination;

error:
	if (source >= 0)
		close(source);
	if (destination >= 0)
		close(destination);
	return -1;
}

static void close_artifacts(int *descriptors, size_t count)
{
	size_t index;

	for (index = 0; index < count; index++) {
		if (descriptors[index] >= 0)
			close(descriptors[index]);
	}
}

static void report_error(enum kobox_link_plan_status status,
			 const struct kobox_link_plan_error *error)
{
	fprintf(stderr, "real closure link failed: status=%u phase=%u",
		(unsigned int)status, (unsigned int)error->phase);
	if (error->node_name)
		fprintf(stderr, " node=%s", error->node_name);
	if (error->symbol_name)
		fprintf(stderr, " symbol=%s", error->symbol_name);
	fputc('\n', stderr);
}

static int loader_address(struct kobox_link_plan_loader *loader,
			  const char *node_name, const char *symbol_name,
			  uint32_t kind, uintptr_t *address_out)
{
	size_t node;

	return kobox_link_plan_loader_find_node(loader, node_name, &node) !=
		       KOBOX_LINK_PLAN_OK ||
	       kobox_link_plan_loader_export(loader, node, symbol_name, kind,
					     address_out) != KOBOX_LINK_PLAN_OK;
}

static int function_pointer(uintptr_t address, void *function_out,
			    size_t function_size)
{
	if (!address || function_size != sizeof(address))
		return -1;
	memcpy(function_out, &address, sizeof(address));
	return 0;
}

KOBOX_MANUAL_ELF_CALL static int run_virtio_gpu_probe_gate(
	struct kobox_link_plan_loader *loader)
{
	typedef int (*context_entry)(const struct kobox_module_context *context);
	typedef int (*module_init_entry)(void);
	typedef void (*module_cleanup_entry)(void);
	context_entry core_init;
	context_entry kernel_init;
	context_entry kernel_active;
	context_entry pci_core_init;
	context_entry pci_core_active;
	context_entry device_init;
	context_entry bridge_probe;
	context_entry bridge_bound;
	context_entry bridge_remove;
	context_entry device_quiesce;
	context_entry device_cleanup;
	module_init_entry virtio_init;
	module_cleanup_entry virtio_cleanup;
	module_init_entry virtio_pci_init;
	module_cleanup_entry virtio_pci_cleanup;
	module_init_entry i2c_init;
	module_cleanup_entry i2c_cleanup;
	module_init_entry drm_init;
	module_cleanup_entry drm_cleanup;
	module_init_entry virtio_gpu_init;
	module_cleanup_entry virtio_gpu_cleanup;
	const struct kb2_core_directory *directory;
	struct kobox_module_context core_context;
	struct kobox_module_context pci_context;
	uintptr_t address;
	int status;

#define LOAD_ENTRY(node_name, symbol_name, kind, target)                    \
	do {                                                                   \
		if (loader_address(loader, node_name, symbol_name, kind,         \
				   &address) ||                                  \
		    function_pointer(address, &(target), sizeof(target)))        \
			return -1;                                                 \
	} while (0)

	LOAD_ENTRY("core/primitive.so", "kobox_linux_core_directory",
		   KOBOX_LINK_PLAN_SYMBOL_OBJECT, directory);
	LOAD_ENTRY("core/primitive.so", "kobox_linux_core_init",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, core_init);
	LOAD_ENTRY("core/primitive.so", "kobox_linux_core_kernel_init",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, kernel_init);
	LOAD_ENTRY("core/primitive.so", "kobox_linux_core_kernel_active",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, kernel_active);
	LOAD_ENTRY("device-pci.so", "kobox_linux_device_pci_core_init",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, pci_core_init);
	LOAD_ENTRY("device-pci.so", "kobox_linux_device_pci_core_active",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, pci_core_active);
	LOAD_ENTRY("device-pci.so", "kobox_linux_device_pci_init",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, device_init);
	LOAD_ENTRY("device-pci.so", "kobox_linux_device_pci_bridge_probe",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, bridge_probe);
	LOAD_ENTRY("device-pci.so", "kobox_linux_device_pci_bridge_bound",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, bridge_bound);
	LOAD_ENTRY("device-pci.so", "kobox_linux_device_pci_bridge_remove",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, bridge_remove);
	LOAD_ENTRY("device-pci.so", "kobox_linux_device_pci_quiesce",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, device_quiesce);
	LOAD_ENTRY("device-pci.so", "kobox_linux_device_pci_cleanup",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, device_cleanup);
	LOAD_ENTRY("drivers/virtio/virtio.ko", "init_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, virtio_init);
	LOAD_ENTRY("drivers/virtio/virtio.ko", "cleanup_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, virtio_cleanup);
	LOAD_ENTRY("drivers/virtio/virtio_pci.ko", "init_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, virtio_pci_init);
	LOAD_ENTRY("drivers/virtio/virtio_pci.ko", "cleanup_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, virtio_pci_cleanup);
	LOAD_ENTRY("drivers/i2c/i2c-core.ko", "init_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, i2c_init);
	LOAD_ENTRY("drivers/i2c/i2c-core.ko", "cleanup_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, i2c_cleanup);
	LOAD_ENTRY("drivers/gpu/drm/drm.ko", "init_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, drm_init);
	LOAD_ENTRY("drivers/gpu/drm/drm.ko", "cleanup_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, drm_cleanup);
	LOAD_ENTRY("drivers/gpu/drm/virtio/virtio-gpu.ko", "init_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, virtio_gpu_init);
	LOAD_ENTRY("drivers/gpu/drm/virtio/virtio-gpu.ko", "cleanup_module",
		   KOBOX_LINK_PLAN_SYMBOL_FUNCTION, virtio_gpu_cleanup);
#undef LOAD_ENTRY

	core_context = (struct kobox_module_context){
		.size = sizeof(core_context),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
		.generation = TEST_GENERATION,
		.node_id = 1,
		.resource_view = &test_resources,
		.runtime_operations = &runtime_operations,
		.core_operations = directory,
		.logical_cpu_count = 2,
	};
	pci_context = (struct kobox_module_context){
		.size = sizeof(pci_context),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
		.generation = TEST_GENERATION,
		.node_id = 2,
		.resource_view = &test_resources,
		.runtime_operations = &runtime_operations,
		.logical_cpu_count = 2,
	};
	status = core_init(&core_context);
	if (status) {
		fprintf(stderr, "core provider init failed: %d\n", status);
		return -1;
	}
	status = kernel_init(&core_context);
	if (status || kernel_active(&core_context) != 1) {
		fprintf(stderr, "Linux device core init failed: %d\n", status);
		return -1;
	}
	status = pci_core_init(&pci_context);
	if (status || pci_core_active(&pci_context) != 1) {
		fprintf(stderr, "Linux PCI core init failed: %d\n", status);
		return -1;
	}
	status = device_init(&pci_context);
	if (status) {
		fprintf(stderr, "PCI resources init failed: %d\n", status);
		return -1;
	}
	status = bridge_probe(&pci_context);
	if (status || bridge_bound(&pci_context) != 0) {
		fprintf(stderr, "PCI bridge probe failed: %d\n", status);
		return -1;
	}
	status = i2c_init();
	if (status) {
		fprintf(stderr, "i2c-core.ko init failed: %d\n", status);
		return -1;
	}
	status = drm_init();
	if (status) {
		fprintf(stderr, "drm.ko init failed: %d\n", status);
		return -1;
	}
	status = virtio_init();
	if (status) {
		fprintf(stderr, "virtio.ko init failed: %d\n", status);
		return -1;
	}
	status = virtio_pci_init();
	if (status || bridge_bound(&pci_context) != 1) {
		fprintf(stderr, "virtio_pci.ko probe/bind failed: init=%d bound=%d\n",
			status, bridge_bound(&pci_context));
		return -1;
	}
	status = virtio_gpu_init();
	if (status || test_resources.queue_notifications != 1 ||
	    test_resources.queue_completions != 1 ||
	    !test_resources.irq_deliveries ||
	    !(test_resources.device_status & 4)) {
		fprintf(stderr,
			"virtio-gpu.ko probe failed: init=%d notify=%zu complete=%zu irq=%zu status=%#x\n",
			status, test_resources.queue_notifications,
			test_resources.queue_completions,
			test_resources.irq_deliveries,
			test_resources.device_status);
		return -1;
	}
	virtio_gpu_cleanup();
	virtio_pci_cleanup();
	if (bridge_bound(&pci_context) != 0) {
		fprintf(stderr, "virtio_pci.ko cleanup left the device bound\n");
		return -1;
	}
	virtio_cleanup();
	drm_cleanup();
	i2c_cleanup();
	if (bridge_remove(&pci_context) || test_resources.bar_mappings ||
	    device_quiesce(&pci_context) || device_cleanup(&pci_context) ||
	    test_resources.allocations || test_resources.mappings) {
		fprintf(stderr, "reverse PCI cleanup failed\n");
		return -1;
	}
	return 0;
}

int main(int argument_count, char **arguments)
{
	const struct kobox_link_plan *plan = &kobox_generated_link_plan;
	struct kobox_link_plan_node *invalid_nodes = NULL;
	struct kobox_link_plan invalid_plan;
	struct kobox_link_plan_loader_config config;
	struct kobox_link_plan_loader *loader = NULL;
	struct kobox_link_plan_error error = { 0 };
	uintptr_t bridge_address;
	size_t device_pci_node;
	int *descriptors;
	size_t index;
	int result = 1;
	int runtime_started = 0;
	if (argument_count != 3) {
		fprintf(stderr, "usage: %s PROVIDER_DIR LINUX_BUILD_DIR\n",
			arguments[0]);
		return 2;
	}
	if (!device_pci_probe_gate_present(plan)) {
		fprintf(stderr, "device-pci probe resource gate is missing\n");
		return 1;
	}
	if (!module_lifecycle_plan_valid(plan)) {
		fprintf(stderr, "module lifecycle plan is invalid\n");
		return 1;
	}
	if (test_resources_init()) {
		fprintf(stderr, "cannot create test resources\n");
		return 1;
	}
	descriptors = malloc(plan->node_count * sizeof(*descriptors));
	if (!descriptors)
		return 1;
	for (index = 0; index < plan->node_count; index++)
		descriptors[index] = -1;
	for (index = 0; index < plan->node_count; index++) {
		const struct kobox_link_plan_node *node = &plan->nodes[index];
		const char *root = node->kind == KOBOX_LINK_PLAN_SHARED_PROVIDER ?
			arguments[1] : arguments[2];

		descriptors[index] = open_artifact(root, node);
		if (descriptors[index] < 0) {
			fprintf(stderr, "cannot seal artifact: %s\n", node->name);
			goto out;
		}
	}
	config = (struct kobox_link_plan_loader_config){
		.plan = plan,
		.artifact_descriptors = descriptors,
		.artifact_count = plan->node_count,
		.error = &error,
	};
	invalid_nodes = malloc(plan->node_count * sizeof(*invalid_nodes));
	if (!invalid_nodes)
		goto out;
	memcpy(invalid_nodes, plan->nodes,
	       plan->node_count * sizeof(*invalid_nodes));
	invalid_nodes[0].content_digest[0] ^= 1u;
	invalid_plan = *plan;
	invalid_plan.nodes = invalid_nodes;
	config.plan = &invalid_plan;
	if (kobox_link_plan_loader_open(&config, &loader) !=
		    KOBOX_LINK_PLAN_ARTIFACT_FAILURE ||
	    loader || error.phase != KOBOX_LINK_PLAN_PHASE_VALIDATE ||
	    error.node_index != 0)
		goto out;
	config.plan = plan;
	{
		enum kobox_link_plan_status status =
			kobox_link_plan_loader_open(&config, &loader);

		if (status != KOBOX_LINK_PLAN_OK) {
			report_error(status, &error);
			goto out;
		}
	}
	if (kobox_link_plan_loader_count(loader) != plan->node_count ||
	    kobox_link_plan_loader_find_node(
		    loader, "device-pci.so", &device_pci_node) !=
		    KOBOX_LINK_PLAN_OK ||
	    kobox_link_plan_loader_export(
		    loader, device_pci_node,
		    "kobox_linux_device_pci_bridge_probe",
		    KOBOX_LINK_PLAN_SYMBOL_FUNCTION, &bridge_address) !=
		    KOBOX_LINK_PLAN_OK ||
	    !bridge_address)
		goto out;
	runtime_started = 1;
	if (run_virtio_gpu_probe_gate(loader))
		goto out;
	result = 0;

out:
	if (loader && !runtime_started)
		kobox_link_plan_loader_close(&loader);
	close_artifacts(descriptors, plan->node_count);
	free(invalid_nodes);
	free(descriptors);
	if (!runtime_started && test_resources.memory.address &&
	    test_resources.memory.address != MAP_FAILED)
		munmap(test_resources.memory.address, test_resources.memory.length);
	return result;
}
