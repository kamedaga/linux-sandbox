// SPDX-License-Identifier: GPL-2.0-only

#include "dma_host.h"
#include "../arch/x86_64/host_call.h"

#include <linux/dma-map-ops.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include "../../drivers/iommu/iommu-priv.h"

struct hosted_dma_domain;

struct kobox_linux_dma_port {
	struct iommu_device iommu;
	struct device *device;
	struct kobox_linux_dma_host host;
	struct hosted_dma_domain *active;
	atomic_t domains;
};

struct hosted_dma_domain {
	struct iommu_domain domain;
	struct kobox_linux_dma_port *port;
	struct xarray pages;
	raw_spinlock_t lock;
};

/* Ordinary RAM carries a port reference until invalidation. SLUB (including
 * large kmalloc) owns frozen pages instead: the DMA API caller must retain
 * its allocation until unmap, and the IOMMU must not change SLUB's refcount.
 */
#define DMA_PAGE_PINNED XA_MARK_0
#define DMA_MAPPING_START XA_MARK_1
#define DMA_MAPPING_END XA_MARK_2

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
	/* Publish one host range for the physically contiguous run selected by
	 * iommu_map_nosync(). Per-page capabilities make ordinary GEM objects
	 * exhaust the host descriptor table despite having only a few runs.
	 * The xarray remains page-granular for pins and iova_to_phys().
	 */
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
		page = pfn_to_page(pfn);
		folio = page_folio(page);
		pin = !folio_test_slab(folio) && !folio_test_large_kmalloc(folio);
		raw_spin_lock_irqsave(&dma->lock, flags);
		if (xa_load(&dma->pages, key)) {
			result = -EEXIST;
		} else {
			if (pin && !folio_try_get(folio)) {
				raw_spin_unlock_irqrestore(&dma->lock, flags);
				xa_release(&dma->pages, key);
				result = -EFAULT;
				goto rollback;
			}
			result = xa_err(xa_store(&dma->pages, key, page, GFP_NOWAIT));
			if (!result && pin)
				xa_set_mark(&dma->pages, key, DMA_PAGE_PINNED);
			if (result && pin)
				put_page(page);
		}
		raw_spin_unlock_irqrestore(&dma->lock, flags);
		xa_release(&dma->pages, key);
		if (result)
			goto rollback;
		stored++;
	}
	result = kobox_host_call(host->map(host->context, iova, physical,
					   length, protection));
	if (result)
		goto rollback;
	raw_spin_lock_irqsave(&dma->lock, flags);
	xa_set_mark(&dma->pages, iova >> PAGE_SHIFT, DMA_MAPPING_START);
	xa_set_mark(&dma->pages, (iova >> PAGE_SHIFT) + count - 1,
		    DMA_MAPPING_END);
	raw_spin_unlock_irqrestore(&dma->lock, flags);
	*mapped = length;
	return 0;

rollback:
	release_hosted_pages(dma, iova, stored);
	return result > 0 ? -EIO : result;
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

		raw_spin_lock_irqsave(&dma->lock, flags);
		if (!xa_load(&dma->pages, first) ||
		    !xa_get_mark(&dma->pages, first, DMA_MAPPING_START)) {
			raw_spin_unlock_irqrestore(&dma->lock, flags);
			break;
		}
		for (; last < count; last++) {
			unsigned long key = (iova >> PAGE_SHIFT) + last;

			if (!xa_load(&dma->pages, key) ||
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

	if (!xa_empty(&dma->pages) || dma->port->active == dma)
		panic("free of active host DMA domain\n");
	xa_destroy(&dma->pages);
	atomic_set_release(&dma->port->domains, 0);
	kfree(dma);
}

static const struct iommu_domain_ops domain_ops = {
	.attach_dev = attach_device, .map_pages = map_pages,
	.unmap_pages = unmap_pages, .iova_to_phys = iova_to_phys,
	.free = free_domain,
};

static struct iommu_domain *allocate_domain(struct device *device)
{
	struct kobox_linux_dma_port *port = dev_iommu_priv_get(device);
	struct hosted_dma_domain *dma;

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
	raw_spin_lock_init(&dma->lock);
	return &dma->domain;
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
	if (port->active && !xa_empty(&port->active->pages))
		return -EBUSY;
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
