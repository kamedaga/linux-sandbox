// SPDX-License-Identifier: GPL-2.0-only

#include "dma_host.h"
#include "diagnostic.h"
#include "../arch/x86_64/host_call.h"

#include <linux/dma-map-ops.h>
#include <linux/hashtable.h>
#include <linux/highmem.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <linux/virtio.h>
/* Upstream virtio_config.h uses a valid partial aggregate initializer. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <linux/virtio_config.h>
#pragma GCC diagnostic pop
#include "../../drivers/iommu/iommu-priv.h"

struct hosted_dma_domain;

#define QUEUE_BOUNCE_SIZE 128U
#define QUEUE_BOUNCE_SLOTS 128U

static const struct queue_bounce_config {
	unsigned int size;
	unsigned int slots;
	enum dma_data_direction direction;
} queue_bounce_configs[] = {
	{ QUEUE_BOUNCE_SIZE, QUEUE_BOUNCE_SLOTS, DMA_TO_DEVICE },
	{ QUEUE_BOUNCE_SIZE, QUEUE_BOUNCE_SLOTS, DMA_FROM_DEVICE },
	/* Bounded command-buffer tier; never retain caller pages when idle. */
	{ 2 * PAGE_SIZE, 32, DMA_TO_DEVICE },
};

struct queue_bounce_slot {
	struct page *page;
	unsigned int offset;
	unsigned int length;
};

/* Only these private pages remain device-visible while slots are idle.
 * Caller RAM is copied, never retained in an idle DMA mapping. Separate
 * pools preserve TO/FROM permissions; bidirectional buffers use the IOMMU.
 */
struct queue_bounce_pool {
	struct page *page;
	dma_addr_t address;
	struct queue_bounce_slot slots[QUEUE_BOUNCE_SLOTS];
};

struct kobox_linux_dma_port {
	struct iommu_device iommu;
	struct device *device;
	struct kobox_linux_dma_host host;
	struct hosted_dma_domain *active;
	atomic_t domains;
	atomic64_t map_attempts;
	atomic64_t map_published;
	atomic64_t map_failures;
	atomic64_t last_failure_bytes;
	atomic_t last_map_error;
	raw_spinlock_t queue_lock;
	DECLARE_HASHTABLE(queue_pages, 6);
	DECLARE_HASHTABLE(queue_iovas, 6);
	struct queue_bounce_pool bounce[ARRAY_SIZE(queue_bounce_configs)];
};

static void record_map_failure(struct kobox_linux_dma_port *port,
			       size_t bytes, int error)
{
	atomic64_inc(&port->map_failures);
	atomic64_set(&port->last_failure_bytes, bytes);
	atomic_set(&port->last_map_error, error);
}

int kobox_linux_dma_snapshot(struct device *device,
			     struct kobox_linux_dma_snapshot *snapshot)
{
	struct kobox_linux_dma_port *port;

	if (!device || !snapshot)
		return -EINVAL;
	port = dev_iommu_priv_get(device);
	if (!port || port->device != device)
		return -ENODEV;
	*snapshot = (struct kobox_linux_dma_snapshot) {
		.attempts = atomic64_read(&port->map_attempts),
		.published = atomic64_read(&port->map_published),
		.failures = atomic64_read(&port->map_failures),
		.last_failure_bytes = atomic64_read(&port->last_failure_bytes),
		.last_error = atomic_read(&port->last_map_error),
	};
	return 0;
}

/* Only simultaneous users share a mapping; zero references always unmap.
 * Coherent, same-direction sub-page buffers already expose this entire page
 * through the IOMMU. Never combine directions or retain an idle mapping.
 */
struct queue_dma_page {
	struct hlist_node physical_node;
	struct hlist_node iova_node;
	struct page *page;
	dma_addr_t address;
	enum dma_data_direction direction;
	unsigned int users;
};

static void queue_bounce_init(struct kobox_linux_dma_port *port)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(port->bounce); i++) {
		struct queue_bounce_pool *pool = &port->bounce[i];
		const struct queue_bounce_config *config = &queue_bounce_configs[i];
		size_t bytes = config->size * config->slots;

		if (pool->page)
			continue;
		pool->page = alloc_pages(GFP_KERNEL | __GFP_ZERO | __GFP_COMP |
					__GFP_NOWARN,
					get_order(bytes));
		if (!pool->page)
			continue;
		pool->address = dma_map_page(port->device, pool->page, 0,
					     bytes, config->direction);
		if (dma_mapping_error(port->device, pool->address)) {
			__free_pages(pool->page, get_order(bytes));
			pool->page = NULL;
		}
	}
}

/* map_page may describe a physically contiguous span crossing page boundaries. */
static void queue_bounce_copy_from(void *target, struct page *page,
				   size_t offset, size_t size)
{
	while (size) {
		size_t in_page = offset_in_page(offset);
		size_t length = min(size, PAGE_SIZE - in_page);

		memcpy_from_page(target,
				 pfn_to_page(page_to_pfn(page) + (offset >> PAGE_SHIFT)),
				 in_page, length);
		target += length;
		offset += length;
		size -= length;
	}
}

static dma_addr_t queue_bounce_map(struct kobox_linux_dma_port *port,
		struct page *page, unsigned int offset, size_t size,
		enum dma_data_direction direction)
{
	struct queue_bounce_pool *pool;
	const struct queue_bounce_config *config;
	unsigned int i, tier;

	if (!size || offset >= PAGE_SIZE ||
	    (direction != DMA_TO_DEVICE && direction != DMA_FROM_DEVICE))
		return DMA_MAPPING_ERROR;
	/* Do not consume large slots when the small tier is full. */
	tier = size <= QUEUE_BOUNCE_SIZE ? direction - DMA_TO_DEVICE : 2;
	config = &queue_bounce_configs[tier];
	if (size > config->size || direction != config->direction ||
	    (direction == DMA_FROM_DEVICE && size > PAGE_SIZE - offset))
		return DMA_MAPPING_ERROR;
	pool = &port->bounce[tier];
	if (!pool->page)
		return DMA_MAPPING_ERROR;
	for (i = 0; i < config->slots; i++)
		if (!pool->slots[i].page)
			break;
	if (i == config->slots)
		return DMA_MAPPING_ERROR;
	pool->slots[i] = (struct queue_bounce_slot) {
		.page = page, .offset = offset, .length = size,
	};
	/* Preserve bytes the device does not overwrite, including FROM buffers. */
	queue_bounce_copy_from(page_address(pool->page) + i * config->size,
			       page, offset, size);
	return pool->address + i * config->size;
}

enum queue_bounce_operation {
	QUEUE_BOUNCE_FOR_CPU,
	QUEUE_BOUNCE_FOR_DEVICE,
	QUEUE_BOUNCE_RELEASE,
};

/* queue_lock protects slots. The coherent host contract and the caller's
 * DMA completion/publication barriers order the copies with device access.
 */
static bool queue_bounce_sync(struct kobox_linux_dma_port *port,
		dma_addr_t address, size_t size, enum dma_data_direction direction,
		enum queue_bounce_operation operation)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(port->bounce); i++) {
		struct queue_bounce_pool *pool = &port->bounce[i];
		const struct queue_bounce_config *config = &queue_bounce_configs[i];
		struct queue_bounce_slot *slot;
		size_t displacement, offset;
		void *original, *bounce;

		if (!pool->page || address < pool->address ||
		    address - pool->address >= config->size * config->slots)
			continue;
		displacement = address - pool->address;
		offset = displacement % config->size;
		slot = &pool->slots[displacement / config->size];
		if (!slot->page || direction != config->direction ||
		    offset > slot->length || size > slot->length - offset ||
		    (operation == QUEUE_BOUNCE_RELEASE &&
		     (offset || size != slot->length)))
			panic("invalid bounced virtqueue DMA access\n");
		bounce = page_address(pool->page) + displacement;
		if (operation == QUEUE_BOUNCE_FOR_DEVICE) {
			queue_bounce_copy_from(bounce, slot->page,
					       slot->offset + offset, size);
		} else if (direction == DMA_FROM_DEVICE) {
			original = kmap_local_page(slot->page);
			memcpy(original + slot->offset + offset, bounce, size);
			kunmap_local(original);
		}
		if (operation == QUEUE_BOUNCE_RELEASE) {
			memset(bounce, 0, config->size);
			memset(slot, 0, sizeof(*slot));
		}
		return true;
	}
	return false;
}

/* The launch owner has stopped new DMA calls. Do not free any pool while
 * a caller still needs a completion copy. Invalidation precedes RAM reuse.
 */
static int queue_bounce_destroy(struct kobox_linux_dma_port *port)
{
	unsigned int i, slot;

	for (i = 0; i < ARRAY_SIZE(port->bounce); i++)
		for (slot = 0; slot < QUEUE_BOUNCE_SLOTS; slot++)
			if (port->bounce[i].slots[slot].page)
				return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(port->bounce); i++) {
		struct queue_bounce_pool *pool = &port->bounce[i];
		const struct queue_bounce_config *config = &queue_bounce_configs[i];
		size_t bytes = config->size * config->slots;

		if (!pool->page)
			continue;
		dma_unmap_page(port->device, pool->address, bytes, config->direction);
		__free_pages(pool->page, get_order(bytes));
		pool->page = NULL;
	}
	return 0;
}

static dma_addr_t queue_map_page(union virtio_map map, struct page *page,
				 unsigned long offset, size_t size,
				 enum dma_data_direction direction,
				 unsigned long attrs)
{
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(map.dma_dev);
	struct queue_dma_page *entry;
	unsigned long flags;
	dma_addr_t address;

	if (attrs || !size || offset >= PAGE_SIZE)
		return dma_map_page_attrs(map.dma_dev, page, offset, size,
					  direction, attrs);
	raw_spin_lock_irqsave(&port->queue_lock, flags);
	address = queue_bounce_map(port, page, offset, size, direction);
	if (address != DMA_MAPPING_ERROR)
		goto unlock;
	if (size > PAGE_SIZE - offset) {
		raw_spin_unlock_irqrestore(&port->queue_lock, flags);
		return dma_map_page_attrs(map.dma_dev, page, offset, size,
					  direction, attrs);
	}
	hash_for_each_possible(port->queue_pages, entry, physical_node,
			       page_to_pfn(page)) {
		if (entry->page != page || entry->direction != direction)
			continue;
		if (entry->users == UINT_MAX) {
			address = DMA_MAPPING_ERROR;
			goto unlock;
		}
		entry->users++;
		address = entry->address + offset;
		goto unlock;
	}
	entry = kmalloc(sizeof(*entry), GFP_ATOMIC | __GFP_NOWARN);
	if (!entry) {
		raw_spin_unlock_irqrestore(&port->queue_lock, flags);
		return dma_map_page_attrs(map.dma_dev, page, offset, size,
					  direction, attrs);
	}
	address = dma_map_page_attrs(map.dma_dev, page, 0, PAGE_SIZE,
				     direction, 0);
	if (dma_mapping_error(map.dma_dev, address)) {
		kfree(entry);
		goto unlock;
	}
	entry->page = page;
	entry->address = address;
	entry->direction = direction;
	entry->users = 1;
	hash_add(port->queue_pages, &entry->physical_node, page_to_pfn(page));
	hash_add(port->queue_iovas, &entry->iova_node, address >> PAGE_SHIFT);
	address += offset;
unlock:
	raw_spin_unlock_irqrestore(&port->queue_lock, flags);
	return address;
}

static void queue_unmap_page(union virtio_map map, dma_addr_t address,
			     size_t size, enum dma_data_direction direction,
			     unsigned long attrs)
{
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(map.dma_dev);
	struct queue_dma_page *entry;
	unsigned long flags;

	raw_spin_lock_irqsave(&port->queue_lock, flags);
	if (queue_bounce_sync(port, address, size, direction,
			      QUEUE_BOUNCE_RELEASE)) {
		if (attrs)
			panic("invalid bounced virtqueue DMA attributes\n");
		raw_spin_unlock_irqrestore(&port->queue_lock, flags);
		return;
	}
	hash_for_each_possible(port->queue_iovas, entry, iova_node,
			       address >> PAGE_SHIFT) {
		if (entry->address != (address & PAGE_MASK))
			continue;
		if (attrs || !size || size > PAGE_SIZE - offset_in_page(address) ||
		    entry->direction != direction || !entry->users)
			panic("invalid shared virtqueue DMA unmap\n");
		if (!--entry->users) {
			/* Keep serialization through the synchronous invalidation:
			 * neither the IOVA nor its page may be recycled before it.
			 */
			dma_unmap_page_attrs(map.dma_dev, entry->address,
					     PAGE_SIZE, direction, 0);
			hash_del(&entry->physical_node);
			hash_del(&entry->iova_node);
			kfree(entry);
		}
		raw_spin_unlock_irqrestore(&port->queue_lock, flags);
		return;
	}
	raw_spin_unlock_irqrestore(&port->queue_lock, flags);
	dma_unmap_page_attrs(map.dma_dev, address, size, direction, attrs);
}

static void queue_sync_cpu(union virtio_map map, dma_addr_t address,
			   size_t size, enum dma_data_direction direction)
{
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(map.dma_dev);
	unsigned long flags;
	bool bounced;

	raw_spin_lock_irqsave(&port->queue_lock, flags);
	bounced = queue_bounce_sync(port, address, size, direction,
				    QUEUE_BOUNCE_FOR_CPU);
	raw_spin_unlock_irqrestore(&port->queue_lock, flags);
	if (bounced)
		return;
	dma_sync_single_for_cpu(map.dma_dev, address, size, direction);
}

static void queue_sync_device(union virtio_map map, dma_addr_t address,
			      size_t size, enum dma_data_direction direction)
{
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(map.dma_dev);
	unsigned long flags;
	bool bounced;

	raw_spin_lock_irqsave(&port->queue_lock, flags);
	bounced = queue_bounce_sync(port, address, size, direction,
				    QUEUE_BOUNCE_FOR_DEVICE);
	raw_spin_unlock_irqrestore(&port->queue_lock, flags);
	if (bounced)
		return;
	dma_sync_single_for_device(map.dma_dev, address, size, direction);
}

static void *queue_alloc(union virtio_map map, size_t size,
			 dma_addr_t *address, gfp_t gfp)
{
	return dma_alloc_coherent(map.dma_dev, size, address, gfp);
}

static void queue_free(union virtio_map map, size_t size, void *cpu,
			 dma_addr_t address, unsigned long attrs)
{
	dma_free_attrs(map.dma_dev, size, cpu, address, attrs);
}

static bool queue_need_sync(union virtio_map map, dma_addr_t address)
{
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(map.dma_dev);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(port->bounce); i++)
		if (port->bounce[i].page && address >= port->bounce[i].address &&
		    address - port->bounce[i].address <
		    queue_bounce_configs[i].size * queue_bounce_configs[i].slots)
			return true;
	return dma_need_sync(map.dma_dev, address);
}

static int queue_mapping_error(union virtio_map map, dma_addr_t address)
{
	return dma_mapping_error(map.dma_dev, address);
}

static size_t queue_max_mapping_size(union virtio_map map)
{
	return dma_max_mapping_size(map.dma_dev);
}

static const struct virtio_map_ops queue_map_ops = {
	.map_page = queue_map_page,
	.unmap_page = queue_unmap_page,
	.sync_single_for_cpu = queue_sync_cpu,
	.sync_single_for_device = queue_sync_device,
	.alloc = queue_alloc,
	.free = queue_free,
	.need_sync = queue_need_sync,
	.mapping_error = queue_mapping_error,
	.max_mapping_size = queue_max_mapping_size,
};

int kobox_linux_dma_bind_virtio(struct kobox_linux_dma_port *port,
			       struct virtio_device *device)
{
	if (!port || !device || device->dev.parent != port->device)
		return -EINVAL;
	if (device->map == &queue_map_ops)
		return 0;
	if (device->map || device->dev.driver || !list_empty(&device->vqs))
		return -EBUSY;
	queue_bounce_init(port);
	device->map = &queue_map_ops;
	return 0;
}

struct hosted_dma_domain {
	struct iommu_domain domain;
	struct kobox_linux_dma_port *port;
	struct xarray pages;
	struct xarray states;
	u64 *page_snapshots[NR_CPUS];
	size_t page_capacity;
	raw_spinlock_t lock;
};

/* Ordinary RAM carries a port reference until invalidation. A non-compound
 * high-order allocation has zero-refcount tail pages: its allocation head
 * retains the whole block, while the DMA API caller owns any mapped subset
 * through unmap. SLUB (including large kmalloc) owns frozen pages instead.
 */
#define DMA_PAGE_PINNED XA_MARK_0
#define DMA_MAPPING_START XA_MARK_1
#define DMA_MAPPING_END XA_MARK_2
#define DMA_MAPPING_PUBLISHED 4U

static void *mapping_state(unsigned int protection, bool published)
{
	return xa_mk_value(protection |
			   (published ? DMA_MAPPING_PUBLISHED : 0));
}

static unsigned int mapping_state_value(void *state)
{
	return xa_is_value(state) ? xa_to_value(state) : 0;
}

static void release_hosted_pages(struct hosted_dma_domain *dma,
				 unsigned long iova, size_t count)
{
	unsigned long flags;
	size_t index;

	raw_spin_lock_irqsave(&dma->lock, flags);
	for (index = 0; index < count; index++) {
		unsigned long key = (iova >> PAGE_SHIFT) + index;
		struct page *page = xa_load(&dma->pages, key);

		if (!page)
			continue;
		if (xa_get_mark(&dma->pages, key, DMA_PAGE_PINNED))
			put_page(page);
		xa_erase(&dma->pages, key);
		xa_erase(&dma->states, key);
	}
	raw_spin_unlock_irqrestore(&dma->lock, flags);
}

static struct hosted_dma_domain *hosted(struct iommu_domain *domain)
{
	return container_of(domain, struct hosted_dma_domain, domain);
}

static int map_pages(struct iommu_domain *domain, unsigned long iova,
		     phys_addr_t physical, size_t pgsize, size_t count,
		     int prot, gfp_t gfp, size_t *mapped)
{
	struct hosted_dma_domain *dma = hosted(domain);
	const struct kobox_linux_dma_host *host = &dma->port->host;
	size_t length, index, stored = 0;
	unsigned long flags;
	unsigned int protection = 0;
	u64 end;
	int result = 0;

	*mapped = 0;
	if (pgsize != PAGE_SIZE || !count ||
	    check_mul_overflow(pgsize, count, &length) ||
	    check_add_overflow((u64)iova, (u64)length - 1, &end) ||
	    iova < host->aperture_start || end > host->aperture_end ||
	    check_add_overflow((u64)physical, (u64)length, &end) ||
	    end > (u64)max_pfn << PAGE_SHIFT ||
	    !IS_ALIGNED(iova | physical, PAGE_SIZE) ||
	    prot & ~(IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE | IOMMU_NOEXEC))
		return -EINVAL;
	if (prot & IOMMU_READ)
		protection |= KOBOX_DMA_DEVICE_READ;
	if (prot & IOMMU_WRITE)
		protection |= KOBOX_DMA_DEVICE_WRITE;
	if (!protection)
		return -EINVAL;
	atomic64_inc(&dma->port->map_attempts);
	/* Aggregate hosts publish in iotlb_sync_map(), after iommu_map_sg() has
	 * collected every physical run. Until then these xarray entries are
	 * pending metadata which upstream rollback may discard without host work. */
	for (index = 0; index < count; index++) {
		unsigned long key = (iova >> PAGE_SHIFT) + index;
		unsigned long pfn = PHYS_PFN(physical) + index;
		struct page *page;
		struct folio *folio;
		bool pin;

		if (!pfn_valid(pfn)) {
			result = -EINVAL;
			goto rollback;
		}
		result = xa_reserve(&dma->pages, key, gfp);
		if (result)
			goto rollback;
		if (host->map_page_list) {
			result = xa_reserve(&dma->states, key, gfp);
			if (result) {
				xa_release(&dma->pages, key);
				goto rollback;
			}
		}
		page = pfn_to_page(pfn);
		folio = page_folio(page);
		/* Non-compound high-order tails have no standalone reference.
		 * The owner retains the allocation through dma_unmap_*(). */
		pin = !folio_test_slab(folio) &&
		      !folio_test_large_kmalloc(folio) &&
		      folio_ref_count(folio) != 0;
		raw_spin_lock_irqsave(&dma->lock, flags);
		if (xa_load(&dma->pages, key) ||
		    (host->map_page_list && xa_load(&dma->states, key))) {
			result = -EEXIST;
		} else {
			if (pin && !folio_try_get(folio)) {
				raw_spin_unlock_irqrestore(&dma->lock, flags);
				xa_release(&dma->pages, key);
				result = -EFAULT;
				goto rollback;
			}
			result = xa_err(xa_store(&dma->pages, key, page, GFP_NOWAIT));
			if (!result && host->map_page_list)
				result = xa_err(xa_store(&dma->states, key,
					mapping_state(protection, false), GFP_NOWAIT));
			if (!result && pin)
				xa_set_mark(&dma->pages, key, DMA_PAGE_PINNED);
			if (result) {
				xa_erase(&dma->pages, key);
				xa_erase(&dma->states, key);
				if (pin)
					put_page(page);
			}
		}
		raw_spin_unlock_irqrestore(&dma->lock, flags);
		xa_release(&dma->pages, key);
		if (host->map_page_list)
			xa_release(&dma->states, key);
		if (result)
			goto rollback;
		stored++;
	}
	if (host->map_page_list) {
		*mapped = length;
		return 0;
	}
	result = kobox_host_call(host->map(host->context, iova, physical,
					   length, protection));
	if (result)
		goto rollback;
	atomic64_inc(&dma->port->map_published);
	raw_spin_lock_irqsave(&dma->lock, flags);
	xa_set_mark(&dma->pages, iova >> PAGE_SHIFT, DMA_MAPPING_START);
	xa_set_mark(&dma->pages, (iova >> PAGE_SHIFT) + count - 1,
		    DMA_MAPPING_END);
	raw_spin_unlock_irqrestore(&dma->lock, flags);
	*mapped = length;
	return 0;

rollback:
	record_map_failure(dma->port, length, result);
	release_hosted_pages(dma, iova, stored);
	return result > 0 ? -EIO : result;
}

static int sync_map(struct iommu_domain *domain, unsigned long iova,
		    size_t size)
{
	struct hosted_dma_domain *dma = hosted(domain);
	const struct kobox_linux_dma_host *host = &dma->port->host;
	unsigned int protection = 0;
	unsigned long flags;
	size_t page_count, index;
	u64 *snapshot;
	int cpu, result;
	bool valid = true;

	if (!host->map_page_list)
		return 0;
	if (!size || !IS_ALIGNED(iova | size, PAGE_SIZE) ||
	    size / PAGE_SIZE > dma->page_capacity ||
	    iova < host->aperture_start || iova > host->aperture_end ||
	    size - 1 > host->aperture_end - iova)
		return -EINVAL;
	page_count = size / PAGE_SIZE;
	cpu = get_cpu();
	snapshot = dma->page_snapshots[cpu];
	if (!snapshot) {
		put_cpu();
		return -ENOMEM;
	}

	raw_spin_lock_irqsave(&dma->lock, flags);
	for (index = 0; index < page_count; index++) {
		unsigned long key = (iova >> PAGE_SHIFT) + index;
		struct page *page = xa_load(&dma->pages, key);
		unsigned int state = mapping_state_value(xa_load(&dma->states, key));
		unsigned long pfn;

		if (!page || !state || (state & DMA_MAPPING_PUBLISHED)) {
			valid = false;
			break;
		}
		if (!protection)
			protection = state;
		else if (state != protection) {
			valid = false;
			break;
		}
		pfn = page_to_pfn(page);
		if (pfn >= dma->page_capacity) {
			valid = false;
			break;
		}
		snapshot[index] = pfn;
	}
	raw_spin_unlock_irqrestore(&dma->lock, flags);
	if (!valid) {
		put_cpu();
		record_map_failure(dma->port, size, -EINVAL);
		return -EINVAL;
	}

	local_irq_save(flags);
	result = kobox_host_call(host->map_page_list(host->context, iova,
					 snapshot, page_count, protection));
	local_irq_restore(flags);
	if (result) {
		put_cpu();
		record_map_failure(dma->port, size, result);
		return result > 0 ? -EIO : result;
	}

	/* Nothing may consume the mapping until this callback returns. Recheck
	 * the pending snapshot before making its aggregate boundary visible. */
	raw_spin_lock_irqsave(&dma->lock, flags);
	for (index = 0; index < page_count; index++) {
		unsigned long key = (iova >> PAGE_SHIFT) + index;
		struct page *page = xa_load(&dma->pages, key);
		unsigned int state = mapping_state_value(xa_load(&dma->states, key));

		if (!page || state != protection ||
		    page_to_pfn(page) != snapshot[index]) {
			valid = false;
			break;
		}
	}
	if (valid) {
		for (index = 0; index < page_count; index++) {
			unsigned long key = (iova >> PAGE_SHIFT) + index;

			if (xa_err(xa_store(&dma->states, key,
					    mapping_state(protection, true), GFP_NOWAIT)))
				panic("host DMA state publication failed\n");
		}
		xa_set_mark(&dma->pages, iova >> PAGE_SHIFT, DMA_MAPPING_START);
		xa_set_mark(&dma->pages,
			    (iova >> PAGE_SHIFT) + page_count - 1,
			    DMA_MAPPING_END);
	}
	raw_spin_unlock_irqrestore(&dma->lock, flags);
	if (!valid) {
		local_irq_save(flags);
		result = kobox_host_call(host->unmap(host->context, iova, size));
		local_irq_restore(flags);
		if (result)
			panic("host DMA rollback invalidation failed\n");
		put_cpu();
		record_map_failure(dma->port, size, -EIO);
		return -EIO;
	}
	atomic64_inc(&dma->port->map_published);
	put_cpu();
	return 0;
}

static size_t unmap_pages(struct iommu_domain *domain, unsigned long iova,
			  size_t pgsize, size_t count,
			  struct iommu_iotlb_gather *gather)
{
	struct hosted_dma_domain *dma = hosted(domain);
	const struct kobox_linux_dma_host *host = &dma->port->host;
	unsigned long flags;
	size_t index;

	if (pgsize != PAGE_SIZE || !count || count > SIZE_MAX / PAGE_SIZE ||
	    iova > ULONG_MAX - (count * PAGE_SIZE - 1))
		return 0;
	for (index = 0; index < count;) {
		unsigned long first = (iova >> PAGE_SHIFT) + index;
		size_t last = index;
		bool complete = false;
		unsigned int first_state;

		raw_spin_lock_irqsave(&dma->lock, flags);
		first_state = mapping_state_value(xa_load(&dma->states, first));
		if (host->map_page_list && first_state &&
		    !(first_state & DMA_MAPPING_PUBLISHED)) {
			/* iommu_map_sg() is rolling back a batch which never reached
			 * the host. Pending pages have no device translation to drain. */
			for (; index < count; index++) {
				unsigned long key = (iova >> PAGE_SHIFT) + index;
				struct page *page = xa_load(&dma->pages, key);
				unsigned int state = mapping_state_value(
					xa_load(&dma->states, key));

				if (!page || !state ||
				    (state & DMA_MAPPING_PUBLISHED))
					break;
				if (xa_get_mark(&dma->pages, key, DMA_PAGE_PINNED))
					put_page(page);
				xa_erase(&dma->pages, key);
				xa_erase(&dma->states, key);
			}
			raw_spin_unlock_irqrestore(&dma->lock, flags);
			continue;
		}
		if (!xa_load(&dma->pages, first) ||
		    (host->map_page_list &&
		     !(first_state & DMA_MAPPING_PUBLISHED)) ||
		    !xa_get_mark(&dma->pages, first, DMA_MAPPING_START)) {
			raw_spin_unlock_irqrestore(&dma->lock, flags);
			break;
		}
		for (; last < count; last++) {
			unsigned long key = (iova >> PAGE_SHIFT) + last;

			if (!xa_load(&dma->pages, key) ||
			    (host->map_page_list &&
			     !(mapping_state_value(xa_load(&dma->states, key)) &
			       DMA_MAPPING_PUBLISHED)) ||
			    (last != index && xa_get_mark(&dma->pages, key,
							 DMA_MAPPING_START)))
				break;
			if (xa_get_mark(&dma->pages, key, DMA_MAPPING_END)) {
				complete = true;
				break;
			}
		}
		if (!complete) {
			raw_spin_unlock_irqrestore(&dma->lock, flags);
			break;
		}
		if (kobox_host_call(host->unmap(host->context,
				iova + index * PAGE_SIZE,
				(last - index + 1) * PAGE_SIZE)))
			panic("host DMA invalidation failed; retaining RAM and IOVA\n");
		for (; index <= last; index++) {
			unsigned long key = (iova >> PAGE_SHIFT) + index;
			struct page *page = xa_load(&dma->pages, key);

			if (xa_get_mark(&dma->pages, key, DMA_PAGE_PINNED))
				put_page(page);
			xa_erase(&dma->pages, key);
			xa_erase(&dma->states, key);
		}
		raw_spin_unlock_irqrestore(&dma->lock, flags);
	}
	/* Every host unmap already completed its invalidation. No deferred
	 * gather entries or no-op flush callbacks are needed in strict mode.
	 */
	return index * PAGE_SIZE;
}

static phys_addr_t iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	struct hosted_dma_domain *dma = hosted(domain);
	unsigned long flags;
	struct page *page;
	phys_addr_t physical = 0;

	raw_spin_lock_irqsave(&dma->lock, flags);
	page = xa_load(&dma->pages, iova >> PAGE_SHIFT);
	if (page)
		physical = page_to_phys(page) + offset_in_page(iova);
	raw_spin_unlock_irqrestore(&dma->lock, flags);
	return physical;
}

static int attach_device(struct iommu_domain *domain, struct device *device)
{
	struct hosted_dma_domain *dma = hosted(domain);
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(device);
	unsigned long flags;
	int result;

	if (port != dma->port || device != port->device)
		return -EINVAL;
	if (port->active && port->active != dma)
		return -EBUSY;
	local_irq_save(flags);
	result = kobox_host_call(port->host.enable(port->host.context, 1));
	local_irq_restore(flags);
	if (result)
		return result < 0 ? result : -EIO;
	port->active = dma;
	return 0;
}

static void free_domain(struct iommu_domain *domain)
{
	struct hosted_dma_domain *dma = hosted(domain);
	unsigned int cpu;

	if (!xa_empty(&dma->pages) || !xa_empty(&dma->states) ||
	    dma->port->active == dma)
		panic("free of active host DMA domain\n");
	for (cpu = 0; cpu < NR_CPUS; cpu++)
		kvfree(dma->page_snapshots[cpu]);
	xa_destroy(&dma->pages);
	xa_destroy(&dma->states);
	atomic_set_release(&dma->port->domains, 0);
	kfree(dma);
}

static const struct iommu_domain_ops domain_ops = {
	.attach_dev = attach_device, .map_pages = map_pages,
	.unmap_pages = unmap_pages, .iotlb_sync_map = sync_map,
	.iova_to_phys = iova_to_phys,
	.free = free_domain,
};

void kobox_linux_dma_diagnose(struct device *device)
{
	static atomic_t reports = ATOMIC_INIT(0);
	struct iommu_domain *domain = iommu_get_domain_for_dev(device);
	struct hosted_dma_domain *dma;
	unsigned long first, last, next, index, flags;
	unsigned long pages = 0, ranges = 0, pending = 0, gap = 0;
	struct page *page;

	if (!domain || domain->ops != &domain_ops ||
	    !atomic_add_unless(&reports, 1, 8))
		return;
	dma = hosted(domain);
	first = domain->geometry.aperture_start >> PAGE_SHIFT;
	last = domain->geometry.aperture_end >> PAGE_SHIFT;
	next = first;
	/* Snapshot translations, not the upstream IOVA allocator's private
	 * reservation/cache tree. An unmapped gap is only an upper bound on
	 * allocatable space. Never reclaim or retry allocations for diagnosis.
	 */
	raw_spin_lock_irqsave(&dma->lock, flags);
	xa_for_each_range(&dma->pages, index, page, first, last) {
		gap = max(gap, index - next);
		next = index + 1;
		pages++;
		if (xa_get_mark(&dma->pages, index, DMA_MAPPING_START))
			ranges++;
		if (dma->port->host.map_page_list &&
		    !(mapping_state_value(xa_load(&dma->states, index)) &
		      DMA_MAPPING_PUBLISHED))
			pending++;
	}
	gap = max(gap, last + 1 - next);
	raw_spin_unlock_irqrestore(&dma->lock, flags);
	kobox_linux_boot_diagnostic("kobox-dma: failure snapshot aperture-pages=%lu mapped-pages=%lu ranges=%lu pending-pages=%lu largest-unmapped-gap-pages=%lu\n",
				    last - first + 1, pages, ranges, pending, gap);
}

static struct iommu_domain *allocate_domain(struct device *device)
{
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(device);
	struct hosted_dma_domain *dma;
	unsigned int cpu;

	/* One host grant names one translation namespace. Do not alias another
	 * Linux domain onto the same host IOVA table.
	 */
	if (atomic_cmpxchg_acquire(&port->domains, 0, 1))
		return ERR_PTR(-EOPNOTSUPP);
	dma = kzalloc(sizeof(*dma), GFP_KERNEL);
	if (!dma) {
		atomic_set_release(&port->domains, 0);
		return ERR_PTR(-ENOMEM);
	}
	dma->port = port;
	dma->domain.pgsize_bitmap = PAGE_SIZE;
	dma->domain.geometry = (struct iommu_domain_geometry) {
		.aperture_start = port->host.aperture_start,
		.aperture_end = port->host.aperture_end, .force_aperture = true,
	};
	xa_init(&dma->pages);
	xa_init(&dma->states);
	if (port->host.map_page_list) {
		dma->page_capacity = port->host.ram_size / PAGE_SIZE;
		for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
			dma->page_snapshots[cpu] = kvmalloc_array(
				dma->page_capacity, sizeof(u64), GFP_KERNEL);
			if (!dma->page_snapshots[cpu])
				goto free_snapshots;
		}
	}
	raw_spin_lock_init(&dma->lock);
	return &dma->domain;

free_snapshots:
	while (cpu)
		kvfree(dma->page_snapshots[--cpu]);
	xa_destroy(&dma->states);
	xa_destroy(&dma->pages);
	kfree(dma);
	atomic_set_release(&port->domains, 0);
	return ERR_PTR(-ENOMEM);
}

static struct iommu_device *probe_device(struct device *device)
{
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(device);

	if (!port || port->device != device)
		return ERR_PTR(-ENODEV);
	return &port->iommu;
}

static void release_device(struct device *device)
{
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(device);
	unsigned long flags;

	local_irq_save(flags);
	if (kobox_host_call(port->host.enable(port->host.context, 0)))
		panic("host DMA device could not be blocked\n");
	local_irq_restore(flags);
	port->active = NULL;
}

static bool host_dma_capable(struct device *device, enum iommu_cap cap)
{
	return cap == IOMMU_CAP_CACHE_COHERENCY;
}

static const struct iommu_ops iommu_ops = {
	.capable = host_dma_capable, .domain_alloc_paging = allocate_domain,
	.probe_device = probe_device, .release_device = release_device,
	.device_group = generic_device_group, .default_domain_ops = &domain_ops,
};

int kobox_linux_dma_attach(struct device *device,
			   const struct kobox_linux_dma_host *host,
			   struct kobox_linux_dma_port **out)
{
	struct kobox_linux_dma_port *port;
	int result;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!device || device->driver || device->iommu || device->iommu_group ||
	    !host || host->size != sizeof(*host) || !host->context ||
	    !host->map || !host->unmap || !host->enable ||
	    host->aperture_start > host->aperture_end ||
	    !IS_ALIGNED(host->aperture_start, PAGE_SIZE) ||
	    (host->aperture_end & (PAGE_SIZE - 1)) != PAGE_SIZE - 1)
		return -EINVAL;
	if (host->map_page_list &&
	    (!host->ram_size || !IS_ALIGNED(host->ram_size, PAGE_SIZE) ||
	     host->ram_size / PAGE_SIZE > ULONG_MAX ||
	     host->ram_size - 1 > host->aperture_end - host->aperture_start))
		return -EINVAL;
	/* x86's current machine configuration is coherent. Reject an unknown
	 * cache-maintenance contract rather than silently skipping its syncs.
	 */
	if (host->coherent != 1)
		return -EOPNOTSUPP;
	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	port->device = get_device(device);
	port->host = *host;
	atomic_set(&port->domains, 0);
	raw_spin_lock_init(&port->queue_lock);
	hash_init(port->queue_pages);
	hash_init(port->queue_iovas);
	port->iommu.fwnode = fwnode_create_software_node(NULL, NULL);
	if (IS_ERR(port->iommu.fwnode)) {
		result = PTR_ERR(port->iommu.fwnode);
		goto free;
	}
	/* The host IOMMU is not a child of its DMA client: such parenting
	 * creates a class directory that collides with the client's iommu link.
	 */
	result = iommu_device_sysfs_add(&port->iommu, NULL, NULL,
				       "host-dma-%s", dev_name(device));
	if (result)
		goto remove_fwnode;
	result = iommu_device_register(&port->iommu, &iommu_ops, NULL);
	if (result)
		goto remove_sysfs;
	mutex_lock(&iommu_probe_device_lock);
	result = iommu_fwspec_init(device, port->iommu.fwnode);
	if (!result)
		dev_iommu_priv_set(device, port);
	mutex_unlock(&iommu_probe_device_lock);
	if (!result)
		result = iommu_probe_device(device);
	if (!result && (!device->dma_iommu || !port->active))
		result = -ENODEV;
	if (!result) {
		*out = port;
		return 0;
	}
	iommu_device_unregister(&port->iommu);
	if (device->iommu)
		dev_iommu_free(device);
remove_sysfs:
	iommu_device_sysfs_remove(&port->iommu);
remove_fwnode:
	fwnode_remove_software_node(port->iommu.fwnode);
free:
	put_device(device);
	kfree(port);
	return result;
}

int kobox_linux_dma_detach(struct kobox_linux_dma_port *port)
{
	if (!port)
		return -EINVAL;
	if (!hash_empty(port->queue_pages) || !hash_empty(port->queue_iovas)) {
		kobox_linux_boot_diagnostic(
			"kobox-dma: detach pending queue pages=%u iovas=%u\n",
			!hash_empty(port->queue_pages),
			!hash_empty(port->queue_iovas));
		return -EBUSY;
	}
	if (queue_bounce_destroy(port)) {
		kobox_linux_boot_diagnostic(
			"kobox-dma: detach pending bounce slots\n");
		return -EBUSY;
	}
	if (port->active && !xa_empty(&port->active->pages)) {
		kobox_linux_boot_diagnostic(
			"kobox-dma: detach pending IOMMU mappings\n");
		return -EBUSY;
	}
	iommu_device_unregister(&port->iommu);
	if (atomic_read(&port->domains) || port->active)
		panic("host DMA domain survived unregister\n");
	port->device->dma_iommu = false;
	iommu_device_sysfs_remove(&port->iommu);
	fwnode_remove_software_node(port->iommu.fwnode);
	put_device(port->device);
	kfree(port);
	return 0;
}
