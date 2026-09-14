// SPDX-License-Identifier: GPL-2.0-only

#include "dma_gate.h"
#include "exception.h"
#include "pressure_gate.h"

#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/iommu.h>
#include <linux/kthread.h>
#include <linux/panic_notifier.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>

static int transfer(const struct kobox_linux_dma_test *test, dma_addr_t iova,
		    u32 *value, bool write)
{
	unsigned long flags;
	int result;

	/* The synchronous conformance engine is a host leaf, not a guest task.
	 * Mask notifications just as for its IOMMU mapping callbacks.
	 */
	local_irq_save(flags);
	result = test->transfer(test->context, iova, value, sizeof(*value), write);
	local_irq_restore(flags);
	return result;
}

static void fail_map(const struct kobox_linux_dma_test *test, unsigned int after)
{
	unsigned long flags;

	local_irq_save(flags);
	test->fail_map(test->context, after);
	local_irq_restore(flags);
}

struct dma_panic_observer {
	struct notifier_block notifier;
	struct page *page;
	const struct kobox_linux_dma_test *test;
};

static int observe_dma_panic(struct notifier_block *notifier,
			     unsigned long event, void *message)
{
	struct dma_panic_observer *observer =
		container_of(notifier, struct dma_panic_observer, notifier);

	/*
	 * Do not lock the IOMMU domain: its unmap failure holds that lock.
	 * The streaming owner remains on the stack throughout panic, so the
	 * expected reference count is owner + the not-yet-released DMA pin.
	 */
	if (strstr(message, "host DMA invalidation failed"))
		observer->test->panic_report(observer->test->context,
					     page_count(observer->page),
					     num_online_cpus());
	return NOTIFY_DONE;
}

static int streaming(struct device *dev, struct kobox_linux_dma_port *port,
		     const struct kobox_linux_dma_test *test)
{
	struct page *page = alloc_page(GFP_KERNEL);
	dma_addr_t address = DMA_MAPPING_ERROR;
	u32 *cpu, value = 0x13572468;
	int references, result = -EINVAL;
	struct dma_panic_observer observer = {
		.notifier = {.notifier_call = observe_dma_panic}, .page = page, .test = test,
	};

	if (!page)
		return -ENOMEM;
	result = atomic_notifier_chain_register(&panic_notifier_list, &observer.notifier);
	if (result) {
		__free_page(page);
		return result;
	}
	result = -EINVAL;
	cpu = page_address(page) + 128;
	references = page_count(page);
	address = dma_map_page(dev, page, 128, 512, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, address))
		goto out;
	if (address == (uintptr_t)cpu || page_count(page) != references + 1 ||
	    iommu_iova_to_phys(iommu_get_domain_for_dev(dev), address) != page_to_phys(page) + 128 ||
	    kobox_linux_dma_detach(port) != -EBUSY || transfer(test, address, &value, true))
		goto out;
	dma_sync_single_for_cpu(dev, address, 512, DMA_BIDIRECTIONAL);
	if (*cpu != value)
		goto out;
	*cpu = 0x24681357;
	dma_sync_single_for_device(dev, address, 512, DMA_BIDIRECTIONAL);
	if (transfer(test, address, &value, false) || value != *cpu)
		goto out;
	dma_unmap_page(dev, address, 512, DMA_BIDIRECTIONAL);
	if (page_count(page) == references &&
	    transfer(test, address, &value, false) == -EFAULT)
		result = 0;
	address = DMA_MAPPING_ERROR;
out:
	if (!dma_mapping_error(dev, address))
		dma_unmap_page(dev, address, 512, DMA_BIDIRECTIONAL);
	atomic_notifier_chain_unregister(&panic_notifier_list, &observer.notifier);
	__free_page(page);
	return result;
}

static int coherent(struct device *dev, const struct kobox_linux_dma_test *test)
{
	dma_addr_t address;
	u32 *cpu = dma_alloc_coherent(dev, 2 * PAGE_SIZE, &address, GFP_KERNEL);
	u32 value = 0;
	int result = -EINVAL;

	if (!cpu)
		return -ENOMEM;
	*cpu = 0xdeadbeef;
	dma_wmb();
	if (address != (uintptr_t)cpu && !transfer(test, address, &value, false) &&
	    value == *cpu) {
		value = 0xcafebabe;
		if (!transfer(test, address + PAGE_SIZE, &value, true)) {
			dma_rmb();
			if (cpu[PAGE_SIZE / sizeof(*cpu)] == value)
				result = 0;
		}
	}
	dma_free_coherent(dev, 2 * PAGE_SIZE, cpu, address);
	if (transfer(test, address, &value, true) != -EFAULT)
		result = -EINVAL;
	return result;
}

static int allocated_buffer(struct device *dev,
			    const struct kobox_linux_dma_test *test,
			    void *buffer, size_t size)
{
	struct page *page = virt_to_page(buffer);
	int references = page_count(page);
	dma_addr_t address;
	u32 value = 0x71d2a539;
	int result = -EINVAL;

	address = dma_map_single(dev, buffer, size, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, address))
		return -ENOMEM;
	if (page_count(page) != references ||
	    transfer(test, address + size - sizeof(value), &value, true))
		goto unmap;
	dma_sync_single_for_cpu(dev, address, size, DMA_BIDIRECTIONAL);
	if (*(u32 *)(buffer + size - sizeof(value)) != value)
		goto unmap;
	*(u32 *)buffer = 0x59a3d271;
	dma_sync_single_for_device(dev, address, size, DMA_BIDIRECTIONAL);
	if (!transfer(test, address, &value, false) && value == 0x59a3d271)
		result = 0;
unmap:
	dma_unmap_single(dev, address, size, DMA_BIDIRECTIONAL);
	if (page_count(page) != references ||
	    transfer(test, address, &value, false) != -EFAULT)
		result = -EINVAL;
	return result;
}

/* SLUB owns these pages, including their frozen page reference counts.
 * The DMA caller keeps each allocation alive through dma_unmap_single().
 */
static int slab_buffers(struct device *dev,
			const struct kobox_linux_dma_test *test)
{
	static const size_t sizes[] = {96, 4 * PAGE_SIZE};
	struct kmem_cache *cache;
	void *buffer;
	unsigned int index;
	int result;

	for (index = 0; index < ARRAY_SIZE(sizes); index++) {
		buffer = kmalloc(sizes[index], GFP_KERNEL);
		if (!buffer)
			return -ENOMEM;
		result = allocated_buffer(dev, test, buffer, sizes[index]);
		kfree(buffer);
		if (result)
			return result;
	}
	cache = kmem_cache_create("kobox-dma-buffer", 96, 0, 0, NULL);
	if (!cache)
		return -ENOMEM;
	buffer = kmem_cache_alloc(cache, GFP_KERNEL);
	result = buffer ? allocated_buffer(dev, test, buffer, 96) : -ENOMEM;
	if (buffer)
		kmem_cache_free(cache, buffer);
	kmem_cache_destroy(cache);
	return result;
}

static int coherent_rollback(struct device *dev,
			     const struct kobox_linux_dma_test *test)
{
	unsigned int after;
	dma_addr_t address;
	void *cpu;

	for (after = 1; after <= 1; after++) {
		fail_map(test, after);
		cpu = dma_alloc_coherent(dev, 2 * PAGE_SIZE, &address, GFP_KERNEL);
		fail_map(test, 0);
		if (cpu) {
			dma_free_coherent(dev, 2 * PAGE_SIZE, cpu, address);
			return -EINVAL;
		}
		if (coherent(dev, test))
			return -EINVAL;
	}
	return 0;
}

static int scatter_gather(struct device *dev, const struct kobox_linux_dma_test *test)
{
	struct page *pages[3] = {0};
	struct scatterlist sg[3];
	int i, after, mapped = 0, result = -EINVAL;
	u32 value;

	sg_init_table(sg, ARRAY_SIZE(sg));
	for (i = 0; i < ARRAY_SIZE(sg); i++) {
		/* Deliberate physical gaps: contiguous IOVA must not be mistaken
		 * for contiguous RAM when the device crosses SG elements.
		 */
		pages[i] = alloc_pages(GFP_KERNEL, 1);
		if (!pages[i])
			goto out;
		sg_set_page(&sg[i], pages[i], PAGE_SIZE, 0);
		*(u32 *)page_address(pages[i]) = 0x9000 + i;
	}
	for (after = 1; after <= ARRAY_SIZE(sg); after++) {
		fail_map(test, after);
		mapped = dma_map_sg(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
		if (mapped)
			goto out;
		for (i = 0; i < ARRAY_SIZE(sg); i++)
			if (page_count(pages[i]) != 1 ||
			    sg[i].length != PAGE_SIZE || sg[i].offset)
				goto out;
	}
	mapped = dma_map_sg(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
	if (mapped != ARRAY_SIZE(sg))
		goto out;
	for (i = 0; i < mapped; i++) {
		if (sg_dma_len(&sg[i]) != PAGE_SIZE ||
		    (sg_dma_address(&sg[i]) & ~dma_get_mask(dev)) ||
		    page_count(pages[i]) != 2 ||
		    transfer(test, sg_dma_address(&sg[i]), &value, false) || value != 0x9000 + i)
			goto out;
	}
	dma_sync_sg_for_cpu(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
	dma_sync_sg_for_device(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
	value = 0;
	{
		dma_addr_t retired = sg_dma_address(sg);

		/* unmap takes original nents, not the coalesced DMA count. */
		dma_unmap_sg(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
		mapped = 0;
		if (transfer(test, retired, &value, false) != -EFAULT)
			goto out;
	}
	for (i = 0; i < ARRAY_SIZE(sg); i++)
		if (page_count(pages[i]) != 1)
			goto out;
	dma_set_max_seg_size(dev, 3 * PAGE_SIZE);
	/* Segment boundary must split even when max_segment_size permits
	 * all three original entries to merge. Upstream owns that decision.
	 */
	mapped = dma_map_sg(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
	if (mapped != 2 || sg_dma_len(sg) != 2 * PAGE_SIZE ||
	    sg_dma_len(sg + 1) != PAGE_SIZE)
		goto out;
	for (i = 0; i < mapped; i++) {
		dma_addr_t start = sg_dma_address(sg + i);
		dma_addr_t end = start + sg_dma_len(sg + i) - 1;

		if ((start ^ end) & ~dma_get_seg_boundary(dev))
			goto out;
	}
	for (i = 0; i < ARRAY_SIZE(sg); i++)
		if (transfer(test, sg_dma_address(sg) + i * PAGE_SIZE,
			     &value, false) || value != 0x9000 + i)
			goto out;
	dma_unmap_sg(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
	mapped = 0;
	for (i = 0; i < ARRAY_SIZE(sg); i++)
		if (page_count(pages[i]) != 1)
			goto out;
	dma_set_seg_boundary(dev, U32_MAX);
	mapped = dma_map_sg(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
	if (mapped != 1 || sg_dma_len(sg) != 3 * PAGE_SIZE)
		goto out;
	for (i = 0; i < ARRAY_SIZE(sg); i++)
		if (transfer(test, sg_dma_address(sg) + i * PAGE_SIZE, &value, false) ||
		    value != 0x9000 + i)
			goto out;
	dma_unmap_sg(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
	mapped = 0;
	for (i = 0; i < ARRAY_SIZE(sg); i++)
		if (page_count(pages[i]) != 1)
			goto out;
	result = 0;
out:
	fail_map(test, 0);
	if (mapped)
		dma_unmap_sg(dev, sg, ARRAY_SIZE(sg), DMA_BIDIRECTIONAL);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		if (pages[i])
			__free_pages(pages[i], 1);
	return result;
}

static int constraints(struct device *dev, const struct kobox_linux_dma_test *test,
		       struct kobox_linux_dma_report *report)
{
	struct page *page = alloc_page(GFP_KERNEL);
	dma_addr_t address = DMA_MAPPING_ERROR;
	u32 value = 0x1234;
	int result = -EINVAL;

	if (!page)
		return -ENOMEM;
	*(u32 *)page_address(page) = value;
	report->phase = 64;
	address = dma_map_page(dev, page, 0, PAGE_SIZE, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, address))
		goto out;
	report->phase = 65;
	if (transfer(test, address, &value, false) || value != 0x1234 ||
	    transfer(test, address, &value, true) != -EACCES)
		goto unmap;
	dma_unmap_page(dev, address, PAGE_SIZE, DMA_TO_DEVICE);
	report->phase = 66;
	address = dma_map_page(dev, page, 0, PAGE_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, address))
		goto out;
	value = 0x5678;
	report->phase = 67;
	if (!transfer(test, address, &value, true) &&
	    transfer(test, address, &value, false) == -EACCES) {
		dma_sync_single_for_cpu(dev, address, PAGE_SIZE, DMA_FROM_DEVICE);
		if (*(u32 *)page_address(page) == 0x5678)
			result = 0;
	}
	dma_unmap_page(dev, address, PAGE_SIZE, DMA_FROM_DEVICE);
	address = DMA_MAPPING_ERROR;
unmap:
	if (!dma_mapping_error(dev, address))
		dma_unmap_page(dev, address, PAGE_SIZE, DMA_TO_DEVICE);
out:
	__free_page(page);
	return result;
}

static int mask_profiles(struct device *dev, struct kobox_linux_dma_port **port,
			 const struct kobox_linux_dma_test *test,
			 struct kobox_linux_dma_report *report)
{
	struct page *page = alloc_page(GFP_KERNEL);
	dma_addr_t address;
	int result = -EINVAL;

	if (!page)
		return -ENOMEM;
	report->phase = 71;
	if (dma_set_mask(dev, DMA_BIT_MASK(16)))
		goto out;
	address = dma_map_page(dev, page, 0, PAGE_SIZE, DMA_TO_DEVICE);
	if (!dma_mapping_error(dev, address)) {
		dma_unmap_page(dev, address, PAGE_SIZE, DMA_TO_DEVICE);
		goto out;
	}
	if (page_count(page) != 1)
		goto out;
	/*
	 * Pinned upstream IOVA caches this below-aperture allocation failure.
	 * Widening dma_mask alone does not reset that cache. Test independent
	 * device configurations through domain teardown/reprobe, not by editing
	 * the allocator's private state or claiming same-domain mask recovery.
	 */
	report->phase = 72;
	result = kobox_linux_dma_detach(*port);
	if (result)
		goto out;
	*port = NULL;
	result = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (result)
		goto out;
	result = kobox_linux_dma_attach(dev, &test->host, port);
	if (result)
		goto out;
	report->phase = 73;
	result = streaming(dev, *port, test);
out:
	__free_page(page);
	return result;
}

#define PARALLEL_ROUNDS 16

struct dma_worker {
	struct task_struct *task;
	struct device *device;
	const struct kobox_linux_dma_test *test;
	struct completion start, mapped, release, done;
	struct page *head;
	dma_addr_t address;
	unsigned int cpu, round;
	bool finish;
	int result;
};

static void parallel_map(struct dma_worker *worker)
{
	struct page *head = alloc_pages(GFP_KERNEL | __GFP_COMP, 1);
	u32 *cpu;

	worker->head = head;
	worker->address = DMA_MAPPING_ERROR;
	worker->result = -ENOMEM;
	if (!head)
		return;
	cpu = page_address(head + 1);
	*cpu = 0xd000 + worker->cpu * 256 + worker->round;
	worker->address = dma_map_page(worker->device, head + 1, 0,
				       PAGE_SIZE, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(worker->device, worker->address)) {
		put_page(head);
		worker->head = NULL;
		return;
	}
	worker->result = page_count(head) == 2 &&
		worker->address != (uintptr_t)cpu ? 0 : -EINVAL;
	/* Only the IOMMU port's pin remains. Mapping a compound tail must
	 * retain its head and the entire allocation until invalidation.
	 */
	put_page(head);
}

static int parallel_transfer(struct dma_worker *worker)
{
	u32 *cpu = page_address(worker->head + 1);
	u32 expected = 0xd000 + worker->cpu * 256 + worker->round;
	u32 value = 0;

	if (raw_smp_processor_id() != worker->cpu || current != worker->task ||
	    page_count(worker->head) != 1)
		return -EINVAL;
	dma_sync_single_for_device(worker->device, worker->address,
				   PAGE_SIZE, DMA_BIDIRECTIONAL);
	if (transfer(worker->test, worker->address, &value, false) || value != expected)
		return -EIO;
	value = expected ^ 0x8888;
	if (transfer(worker->test, worker->address, &value, true))
		return -EIO;
	dma_sync_single_for_cpu(worker->device, worker->address,
				PAGE_SIZE, DMA_BIDIRECTIONAL);
	return *cpu == value ? 0 : -EIO;
}

static int parallel_worker(void *argument)
{
	struct dma_worker *worker = argument;

	for (;;) {
		wait_for_completion(&worker->start);
		if (READ_ONCE(worker->finish))
			break;
		parallel_map(worker);
		complete(&worker->mapped);
		wait_for_completion(&worker->release);
		if (!worker->result)
			worker->result = parallel_transfer(worker);
		if (!dma_mapping_error(worker->device, worker->address))
			dma_unmap_page(worker->device, worker->address,
				       PAGE_SIZE, DMA_BIDIRECTIONAL);
		/* head is now freed: do not inspect its metadata after unmap. */
		worker->head = NULL;
		complete(&worker->done);
	}
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int inspect_dma_pins(void *argument)
{
	struct dma_worker *workers = argument;
	unsigned int cpu;

	for (cpu = 0; cpu < 2; cpu++) {
		struct dma_worker *worker = &workers[cpu];
		u32 value = 0, expected = 0xd000 + cpu * 256 + worker->round;

		if (page_count(worker->head) != 1 ||
		    transfer(worker->test, worker->address, &value, false) || value != expected)
			return -EIO;
	}
	return 0;
}

static int parallel_dma(struct device *dev, const struct kobox_linux_dma_test *test,
			struct kobox_linux_dma_report *report)
{
	struct dma_worker workers[2] = {0};
	struct page *churn[32] = {0};
	unsigned int cpu, round, index;
	u32 value = 0;
	int result = -EINVAL;

	if (num_online_cpus() != 2 || !cpu_online(0) || !cpu_online(1))
		return -EINVAL;
	for (cpu = 0; cpu < ARRAY_SIZE(workers); cpu++) {
		struct dma_worker *worker = &workers[cpu];

		worker->cpu = cpu;
		worker->device = dev;
		worker->test = test;
		init_completion(&worker->start);
		init_completion(&worker->mapped);
		init_completion(&worker->release);
		init_completion(&worker->done);
		worker->task = kthread_create(parallel_worker, worker, "dma-gate/%u", cpu);
		if (IS_ERR(worker->task)) {
			result = PTR_ERR(worker->task);
			worker->task = NULL;
			goto out;
		}
		kthread_bind(worker->task, cpu);
		wake_up_process(worker->task);
	}
	for (round = 0; round < PARALLEL_ROUNDS; round++) {
		for (cpu = 0; cpu < ARRAY_SIZE(workers); cpu++) {
			workers[cpu].round = round;
			complete(&workers[cpu].start);
		}
		for (cpu = 0; cpu < ARRAY_SIZE(workers); cpu++)
			if (!wait_for_completion_timeout(&workers[cpu].mapped, 5 * HZ) ||
			    workers[cpu].result || page_count(workers[cpu].head) != 1)
				goto out;
		if (workers[0].address == workers[1].address || workers[0].head == workers[1].head)
			goto out;
		if (!round) {
			struct kobox_linux_pressure_report pressure = {.size = sizeof(pressure)};

			/* Reuse the full RAM exhaustion/reclaim Gate. Its inspection
			 * runs at allocation failure, before pressure pages are freed.
			 */
			result = kobox_linux_pressure_inspect(&pressure, inspect_dma_pins, workers);
			report->pressure_pages = pressure.pressure_pages;
			report->pressure_reclaimed = pressure.direct_freed + pressure.kswapd_freed;
			report->pressure_line = pressure.line;
			if (result)
				goto out;
			result = -EINVAL;
		}
		/* Exercise the same buddy allocation size while both original
		 * owners are gone and both DMA mappings are still live.
		 */
		for (index = 0; index < ARRAY_SIZE(churn); index++) {
			churn[index] = alloc_pages(GFP_KERNEL | __GFP_COMP | __GFP_ZERO, 1);
			if (!churn[index]) {
				result = -ENOMEM;
				goto out;
			}
			if (churn[index] == workers[0].head || churn[index] == workers[1].head)
				goto out;
		}
		for (cpu = 0; cpu < ARRAY_SIZE(workers); cpu++)
			complete(&workers[cpu].release);
		for (cpu = 0; cpu < ARRAY_SIZE(workers); cpu++)
			if (!wait_for_completion_timeout(&workers[cpu].done, 5 * HZ) || workers[cpu].result)
				goto out;
		/* No new map is started before these probes: a legally reused
		 * IOVA must not be mistaken for a stale translation.
		 */
		for (cpu = 0; cpu < ARRAY_SIZE(workers); cpu++) {
			if (transfer(test, workers[cpu].address, &value, false) != -EFAULT)
				goto out;
			report->cpu_mask |= BIT(cpu);
			report->sole_pins++;
		}
		for (index = 0; index < ARRAY_SIZE(churn); index++) {
			put_page(churn[index]);
			churn[index] = NULL;
		}
		report->parallel_rounds++;
	}
	result = 0;
out:
	for (cpu = 0; cpu < ARRAY_SIZE(workers); cpu++) {
		struct dma_worker *worker = &workers[cpu];

		if (!worker->task)
			continue;
		WRITE_ONCE(worker->finish, true);
		complete(&worker->release);
		complete(&worker->start);
		if (kthread_stop(worker->task))
			result = -EINVAL;
	}
	for (index = 0; index < ARRAY_SIZE(churn); index++)
		if (churn[index])
			put_page(churn[index]);
	return result;
}

int kobox_linux_dma_verify(const struct kobox_linux_pci_host *pci,
			   const struct kobox_linux_dma_test *test,
			   struct kobox_linux_dma_report *report)
{
	struct pci_host_bridge *bridge = NULL;
	struct pci_dev *device = NULL;
	struct kobox_linux_dma_port *port = NULL;
	int result;

	if (!test || !test->context || !test->transfer || !test->fail_map || !test->panic_report ||
	    !report || report->size != sizeof(*report))
		return -EINVAL;
	report->phase = 1;
	result = kobox_linux_pci_scan(pci, &bridge);
	if (result)
		goto out;
	device = pci_get_slot(bridge->bus, pci->devfn);
	result = -ENODEV;
	if (!device || device->driver || device->dev.driver)
		goto out;
	report->phase = 2;
	result = kobox_linux_dma_attach(&device->dev, &test->host, &port);
	if (result)
		goto out;
	result = dma_set_mask_and_coherent(&device->dev, DMA_BIT_MASK(32));
	if (result)
		goto out;
	dma_set_max_seg_size(&device->dev, PAGE_SIZE);
	dma_set_seg_boundary(&device->dev, 2 * PAGE_SIZE - 1);
	report->phase = 3;
	result = streaming(&device->dev, port, test);
	if (result)
		goto out;
	report->cases++;
	report->phase = 10;
	result = slab_buffers(&device->dev, test);
	if (result)
		goto out;
	report->cases++;
	report->phase = 4;
	result = coherent(&device->dev, test);
	if (result)
		goto out;
	report->cases++;
	report->phase = 5;
	result = scatter_gather(&device->dev, test);
	if (result)
		goto out;
	report->cases++;
	report->phase = 6;
	result = constraints(&device->dev, test, report);
	if (result)
		goto out;
	report->cases++;
	report->phase = 7;
	result = mask_profiles(&device->dev, &port, test, report);
	if (result)
		goto out;
	report->cases++;
	report->phase = 8;
	result = coherent_rollback(&device->dev, test);
	if (result)
		goto out;
	report->cases++;
	report->phase = 9;
	result = parallel_dma(&device->dev, test, report);
	if (!result)
		report->cases++;
out:
	if (port && kobox_linux_dma_detach(port))
		return -EBUSY;
	pci_dev_put(device);
	if (bridge && kobox_linux_pci_remove(bridge))
		return -EBUSY;
	rcu_barrier();
	report->drained = 1;
	report->warnings = kobox_linux_exception_warnings();
	if (!result && report->warnings)
		result = -EINVAL;
	report->result = result;
	return result;
}
