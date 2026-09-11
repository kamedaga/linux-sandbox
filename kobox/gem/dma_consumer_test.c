// SPDX-License-Identifier: GPL-2.0-only

#include "dma_consumer_test.h"
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "../../drivers/gpu/drm/virtio/virtgpu_drv.h"
#include "../../mm/slab.h"

struct consumer {
	struct pci_dev *pci;
	void __iomem *bar;
	struct miscdevice misc;
	struct mutex lock;
};

struct consumer_file {
	struct consumer *device;
	struct dma_buf *buffer;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	/* Non-owning identities for the lifetime assertion; never dereferenced
	 * after detach. The fixture keeps virtio_gpu loaded until this ends.
	 */
	struct kmem_cache *object_cache;
	unsigned long object_address, pfns[4];
	gfp_t page_gfp;
};

/* Fixture-only supervision, like the existing native GEM lifetime gate.
 * The boot core supplies its upstream deferred-free/LRU drain sequence;
 * no otherwise-private Linux MM symbol is exported for this test module.
 */
static void (*drain_lifetime)(void);

int kobox_dma_consumer_test_setup(void (*drain)(void));
int kobox_dma_consumer_test_setup(void (*drain)(void))
{
	if (!drain || drain_lifetime)
		return -EINVAL;
	drain_lifetime = drain;
	return 0;
}
EXPORT_SYMBOL_GPL(kobox_dma_consumer_test_setup);

static int remember_backing(struct consumer_file *file)
{
	struct virtio_gpu_object *object;
	unsigned int index;

	if (strcmp(file->buffer->exp_name, "virtio_gpu"))
		return -EINVAL;
	object = gem_to_virtio_gpu_obj(file->buffer->priv);
	if (!object->base.pages || !object->base.base.filp)
		return -EINVAL;
	file->object_address = (unsigned long)object;
	file->object_cache = virt_to_slab(object)->slab_cache;
	file->page_gfp = mapping_gfp_mask(object->base.base.filp->f_mapping);
	for (index = 0; index < 4; index++)
		file->pfns[index] = page_to_pfn(object->base.pages[index]);
	return 0;
}

static void detach_buffer(struct consumer_file *file)
{
	if (!file->buffer)
		return;
	if (file->table)
		dma_buf_unmap_attachment_unlocked(file->attachment, file->table,
						 DMA_BIDIRECTIONAL);
	if (file->attachment)
		dma_buf_detach(file->buffer, file->attachment);
	dma_buf_put(file->buffer);
	file->buffer = NULL;
	file->attachment = NULL;
	file->table = NULL;
}

static int attach_buffer(struct consumer_file *file, void __user *argument)
{
	struct kobox_dma_consumer_attach request;
	int result;

	if (file->buffer)
		return -EBUSY;
	if (copy_from_user(&request, argument, sizeof(request)))
		return -EFAULT;
	file->buffer = dma_buf_get(request.fd);
	if (IS_ERR(file->buffer)) {
		result = PTR_ERR(file->buffer);
		file->buffer = NULL;
		return result;
	}
	if (file->buffer->size != 4 * PAGE_SIZE) {
		result = -EINVAL;
		goto fail;
	}
	file->attachment = dma_buf_attach(file->buffer, &file->device->pci->dev);
	if (IS_ERR(file->attachment)) {
		result = PTR_ERR(file->attachment);
		file->attachment = NULL;
		goto fail;
	}
	file->table = dma_buf_map_attachment_unlocked(file->attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(file->table)) {
		result = PTR_ERR(file->table);
		file->table = NULL;
		goto fail;
	}
	request.segments = file->table->nents;
	result = remember_backing(file);
	if (result)
		goto fail;
	if (copy_to_user(argument, &request, sizeof(request))) {
		result = -EFAULT;
		goto fail;
	}
	return 0;
fail:
	detach_buffer(file);
	return result;
}

static int transfer(struct consumer *device, dma_addr_t address, bool to_ram)
{
	u64 command;
	int result;

	writeq(to_ram ? 0x40000 : address, device->bar + 0x80);
	writeq(to_ram ? address : 0x40000, device->bar + 0x88);
	writeq(64, device->bar + 0x90);
	writeq(1 | (to_ram << 1), device->bar + 0x98);
	result = readq_poll_timeout(device->bar + 0x98, command, !(command & 1),
				   1000, 1000000);
	/* A timeout cannot permit buffer unpin/reuse with DMA still running.
	 * Clearing bus master and resetting the function stops the engine.
	 */
	if (result) {
		pci_clear_master(device->pci);
		if (pci_reset_function(device->pci))
			panic("EDU DMA could not be stopped after timeout\n");
	}
	return result;
}

static int transfer_buffer(struct consumer_file *file)
{
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	struct scatterlist *sg;
	u8 expected[4][64];
	unsigned int index;
	size_t offset = 0;
	int result, end;

	if (!file->table)
		return -ENXIO;
	if (!file->table->nents || file->table->nents > 4)
		return -EINVAL;
	if (dma_resv_test_signaled(file->buffer->resv, DMA_RESV_USAGE_WRITE))
		return -EALREADY;
	result = dma_buf_begin_cpu_access(file->buffer, DMA_BIDIRECTIONAL);
	if (result)
		return result;
	if (!dma_resv_test_signaled(file->buffer->resv, DMA_RESV_USAGE_WRITE)) {
		dma_buf_end_cpu_access(file->buffer, DMA_BIDIRECTIONAL);
		return -EBUSY;
	}
	result = dma_buf_vmap_unlocked(file->buffer, &map);
	if (!result) {
		for (index = 0; index < 4; index++)
			iosys_map_memcpy_from(expected[index], &map, index * PAGE_SIZE, 64);
		dma_buf_vunmap_unlocked(file->buffer, &map);
	}
	end = dma_buf_end_cpu_access(file->buffer, DMA_BIDIRECTIONAL);
	if (result || end)
		return result ?: end;
	/* Linux may coalesce physically adjacent pages before mapping. Walk
	 * the returned DMA spans rather than assuming one SG entry per page.
	 * EDU, not the CPU, copies each page's first 64 bytes to offset 128.
	 */
	for_each_sgtable_dma_sg(file->table, sg, index) {
		size_t in_segment, length = sg_dma_len(sg);

		if (!length || !IS_ALIGNED(length, PAGE_SIZE) || !sg_dma_address(sg) ||
		    length > 4 * PAGE_SIZE - offset)
			return -EINVAL;
		for (in_segment = 0; in_segment < length; in_segment += PAGE_SIZE) {
			result = transfer(file->device, sg_dma_address(sg) + in_segment, false);
			if (!result)
				result = transfer(file->device, sg_dma_address(sg) + in_segment + 128, true);
			if (result)
				return result;
		}
		offset += length;
	}
	if (offset != 4 * PAGE_SIZE)
		return -EINVAL;
	offset = 0;
	result = dma_buf_begin_cpu_access(file->buffer, DMA_BIDIRECTIONAL);
	if (result)
		return result;
	result = dma_buf_vmap_unlocked(file->buffer, &map);
	if (!result) {
		u8 actual[64];

		for (index = 0; index < 4; index++, offset += PAGE_SIZE) {
			iosys_map_memcpy_from(actual, &map, offset + 128, sizeof(actual));
			if (memcmp(actual, expected[index], sizeof(actual)))
				result = -EIO;
		}
		dma_buf_vunmap_unlocked(file->buffer, &map);
	}
	end = dma_buf_end_cpu_access(file->buffer, DMA_BIDIRECTIONAL);
	return result ?: end;
}

static int reclaim_backing(struct consumer_file *file, struct kobox_dma_consumer_reclaim *report)
{
	cpumask_t saved = *current->cpus_ptr;
	unsigned int cpu, index, which, count, seen = 0;
	void **probes;
	bool object_seen = false;
	int result = 0;

	if (file->buffer || !file->object_cache || !drain_lifetime)
		return -EBUSY;
	probes = kvcalloc(4096 * num_online_cpus(), sizeof(*probes), GFP_KERNEL);
	if (!probes)
		return -ENOMEM;
	/* Allow the actual RESOURCE_UNREF response and object-free work to
	 * finish. Reuse below, not this delay, is the reclamation assertion.
	 */
	msleep(1000);
	drain_lifetime();
	count = 0;
	for_each_online_cpu(cpu) {
		result = set_cpus_allowed_ptr(current, cpumask_of(cpu));
		if (result)
			break;
		for (index = 0; index < 4096 && !object_seen; index++) {
			void *object = kmem_cache_alloc(file->object_cache, GFP_KERNEL);

			if (!object) {
				result = -ENOMEM;
				break;
			}
			probes[count++] = object;
			object_seen = (unsigned long)object == file->object_address;
		}
		if (result)
			break;
	}
	for (index = 0; index < count; index++)
		kmem_cache_free(file->object_cache, probes[index]);
	if (!result && !object_seen)
		result = -ENOENT;
	/* LRU-add batches retain a folio reference even after the last file
	 * and VMA disappear. Drain the upstream per-CPU batches before proving
	 * buddy reuse; a small allocation probe need not trigger slow reclaim.
	 */
	drain_lifetime();
	count = 0;
	if (!result) {
		for_each_online_cpu(cpu) {
			result = set_cpus_allowed_ptr(current, cpumask_of(cpu));
			if (result)
				break;
			for (index = 0; index < 4096 && seen != 15; index++) {
				struct page *page = alloc_page(file->page_gfp);

				if (!page) {
					result = -ENOMEM;
					break;
				}
				probes[count++] = page;
				for (which = 0; which < 4; which++)
					if (page_to_pfn(page) == file->pfns[which]) {
						seen |= BIT(which);
						memset(page_address(page), 0xa7, PAGE_SIZE);
					}
			}
			if (result)
				break;
		}
	}
	for (index = 0; index < count; index++)
		__free_page(probes[index]);
	report->objects = object_seen;
	report->pages = seen;
	for (index = 0; index < 4; index++) {
		struct page *page = pfn_to_page(file->pfns[index]);
		struct folio *folio = page_folio(page);

		report->references[index] = page_ref_count(page);
		report->mappings[index] = folio_mapcount(folio);
		report->flags[index] = folio_test_slab(folio) |
			(folio_test_swapbacked(folio) << 1) |
			(folio_test_lru(folio) << 2) | (folio_test_anon(folio) << 3);
	}
	if (!result && seen != 15)
		result = -ENODATA;
	if (set_cpus_allowed_ptr(current, &saved))
		result = -EINVAL;
	kvfree(probes);
	return result;
}

static long consumer_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct consumer_file *session = file->private_data;
	int result;

	mutex_lock(&session->device->lock);
	switch (command) {
	case KOBOX_DMA_CONSUMER_ATTACH:
		result = attach_buffer(session, (void __user *)argument);
		break;
	case KOBOX_DMA_CONSUMER_TRANSFER:
		result = transfer_buffer(session);
		break;
	case KOBOX_DMA_CONSUMER_DETACH:
		detach_buffer(session);
		result = 0;
		break;
	case KOBOX_DMA_CONSUMER_RECLAIM: {
		struct kobox_dma_consumer_reclaim report = {0};

		result = reclaim_backing(session, &report);
		if (copy_to_user((void __user *)argument, &report, sizeof(report)))
			result = -EFAULT;
		break;
	}
	default:
		result = -ENOTTY;
	}
	mutex_unlock(&session->device->lock);
	return result;
}

static int consumer_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct consumer_file *session = kzalloc(sizeof(*session), GFP_KERNEL);

	if (!session)
		return -ENOMEM;
	session->device = container_of(misc, struct consumer, misc);
	file->private_data = session;
	return nonseekable_open(inode, file);
}

static int consumer_release(struct inode *inode, struct file *file)
{
	struct consumer_file *session = file->private_data;

	detach_buffer(session);
	kfree(session);
	return 0;
}

static const struct file_operations consumer_operations = {
	.owner = THIS_MODULE, .open = consumer_open, .release = consumer_release,
	.unlocked_ioctl = consumer_ioctl,
};

static int consumer_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
	struct consumer *device;
	int result;

	device = devm_kzalloc(&pci->dev, sizeof(*device), GFP_KERNEL);
	if (!device)
		return -ENOMEM;
	device->pci = pci;
	result = pcim_enable_device(pci);
	if (result)
		return result;
	result = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(64));
	if (result)
		return result;
	/* EDU commands carry a DMA address and length, not page-sized SG
	 * descriptors. Do not invent a hardware page segmentation limit.
	 */
	dma_set_max_seg_size(&pci->dev, UINT_MAX);
	device->bar = pcim_iomap_region(pci, 0, "dma-buf consumer test");
	if (IS_ERR(device->bar))
		return PTR_ERR(device->bar);
	mutex_init(&device->lock);
	device->misc = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR, .name = "dma-consumer",
		.fops = &consumer_operations, .parent = &pci->dev,
	};
	result = misc_register(&device->misc);
	if (result)
		return result;
	pci_set_drvdata(pci, device);
	pci_set_master(pci);
	return 0;
}

static void consumer_remove(struct pci_dev *pci)
{
	struct consumer *device = pci_get_drvdata(pci);

	/* The fixture unloads this test module only after every client exits.
	 * Its fops owner prevents module removal with a live consumer FD.
	 */
	misc_deregister(&device->misc);
	pci_clear_master(pci);
}

static const struct pci_device_id consumer_ids[] = {
	{ PCI_DEVICE(0x1234, 0x11e8) }, { }
};
MODULE_DEVICE_TABLE(pci, consumer_ids);

static struct pci_driver consumer_driver = {
	.name = "dma-consumer-test", .id_table = consumer_ids,
	.probe = consumer_probe, .remove = consumer_remove,
};
module_pci_driver(consumer_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Native dma-buf import, EDU transfer and lifetime test");
MODULE_IMPORT_NS("DMA_BUF");
