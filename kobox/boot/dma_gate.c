// SPDX-License-Identifier: GPL-2.0-only

#include "dma_gate.h"
#include "exception.h"
#include "pressure_gate.h"
#include "allocation_gate.h"

#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/iommu.h>
#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/panic_notifier.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/virtio.h>
#include <linux/xarray.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <linux/virtio_config.h>
#pragma GCC diagnostic pop

#define DMA_XARRAY_WAIT HZ
#define DMA_XARRAY_VICTIM 7UL
#define DMA_XARRAY_TARGET (ULONG_MAX >> 1)

struct dma_xarray_gate {
	struct xarray array;
	struct irq_work irq;
	struct completion done;
	unsigned int cpu, hooks, callbacks;
	int error;
	bool armed, releasing, queued;
};

/* A timed-out IRQ still owns this state. Static storage avoids freeing its
 * array or callback argument merely because the bounded test wait expired.
 * The boot gate is a single caller; matching an armed array excludes normal
 * DMA activity on either CPU from the injection hook.
 */
static struct dma_xarray_gate xarray_gate;

static void dma_xarray_irq(struct irq_work *work)
{
	struct dma_xarray_gate *gate = container_of(work,
		struct dma_xarray_gate, irq);

	if (!in_hardirq() || !irqs_disabled() ||
	    raw_smp_processor_id() != gate->cpu ||
	    spin_is_locked(&gate->array.xa_lock)) {
		WRITE_ONCE(gate->error, -EDEADLK);
	} else if (xa_erase(&gate->array, DMA_XARRAY_VICTIM) !=
		   xa_mk_value(1)) {
		WRITE_ONCE(gate->error, -EINVAL);
	}
	WRITE_ONCE(gate->callbacks, READ_ONCE(gate->callbacks) + 1);
	complete(&gate->done);
}

void kobox_linux_dma_gate_xarray_locked(struct xarray *xa,
	unsigned long index, bool releasing)
{
	struct dma_xarray_gate *gate = &xarray_gate;

	if (!READ_ONCE(gate->armed) || xa != &gate->array ||
	    index != DMA_XARRAY_TARGET || releasing != gate->releasing)
		return;
	gate->hooks++;
	/* Detect the old ordinary xa_lock path before scheduling an IRQ that
	 * would otherwise spin forever behind its interrupted lock owner.
	 */
	if (!irqs_disabled() || !spin_is_locked(&xa->xa_lock)) {
		gate->error = -EACCES;
		return;
	}
	if (gate->queued || READ_ONCE(gate->callbacks) ||
	    !irq_work_queue(&gate->irq)) {
		gate->error = -EINVAL;
		return;
	}
	gate->queued = true;
	if (READ_ONCE(gate->callbacks))
		gate->error = -EDEADLK;
}

static int dma_xarray_case(gfp_t gfp, bool releasing, bool disabled)
{
	struct dma_xarray_gate *gate = &xarray_gate;
	unsigned long flags = 0;
	int result;

	memset(gate, 0, sizeof(*gate));
	xa_init_flags(&gate->array, XA_FLAGS_LOCK_IRQ);
	init_completion(&gate->done);
	gate->irq = IRQ_WORK_INIT_HARD(dma_xarray_irq);
	gate->releasing = releasing;
	result = xa_err(xa_store_irq(&gate->array, DMA_XARRAY_VICTIM,
				   xa_mk_value(1), GFP_KERNEL));
	if (result)
		goto out_array;
	if (releasing) {
		result = kobox_linux_dma_gate_xa_reserve(&gate->array,
			DMA_XARRAY_TARGET, GFP_KERNEL);
		if (result)
			goto out_array;
	}
	/* Unlike get_cpu(), migration pinning permits the legitimate
	 * GFP_KERNEL slow allocation path to sleep and release the XA lock.
	 */
	migrate_disable();
	gate->cpu = raw_smp_processor_id();
	WRITE_ONCE(gate->armed, true);
	if (disabled)
		local_irq_save(flags);
	if (releasing) {
		kobox_linux_dma_gate_xa_release(&gate->array, DMA_XARRAY_TARGET);
		result = 0;
	} else {
		result = kobox_linux_dma_gate_xa_reserve(&gate->array,
			DMA_XARRAY_TARGET, gfp);
	}
	WRITE_ONCE(gate->armed, false);
	if (irqs_disabled() != disabled || gate->hooks != 1 ||
	    (disabled && READ_ONCE(gate->callbacks)))
		gate->error = -EINVAL;
	if (disabled)
		local_irq_restore(flags);
	/* A failed mask-preservation assertion must not leave the bounded
	 * completion wait atomic. The gate entered with interrupts enabled.
	 */
	if (irqs_disabled())
		local_irq_enable();
	if (gate->queued) {
		if (!wait_for_completion_timeout(&gate->done, DMA_XARRAY_WAIT)) {
			migrate_enable();
			return -ETIMEDOUT;
		}
		irq_work_sync(&gate->irq);
	}
	migrate_enable();
	if (!result && (gate->error || !gate->queued || gate->callbacks != 1))
		result = gate->error ?: -EINVAL;
	if (!releasing)
		kobox_linux_dma_gate_xa_release(&gate->array, DMA_XARRAY_TARGET);
	if (!result && !xa_empty(&gate->array))
		result = -EINVAL;
out_array:
	xa_destroy(&gate->array);
	return result;
}

static int dma_xarray_preserve(void)
{
	struct dma_xarray_gate *gate = &xarray_gate;
	unsigned long flags;
	int result;

	memset(gate, 0, sizeof(*gate));
	xa_init_flags(&gate->array, XA_FLAGS_LOCK_IRQ);
	/* A negative control proves that the hook detects ordinary unmasked
	 * locking without queuing the IRQ which caused the production deadlock.
	 */
	gate->armed = true;
	xa_lock(&gate->array);
	kobox_linux_dma_gate_xarray_locked(&gate->array, DMA_XARRAY_TARGET,
					 false);
	xa_unlock(&gate->array);
	gate->armed = false;
	if (gate->error != -EACCES || gate->hooks != 1 || gate->queued ||
	    gate->callbacks || irqs_disabled())
		return -EINVAL;
	gate->error = 0;
	result = xa_err(xa_store_irq(&gate->array, DMA_XARRAY_TARGET,
				   xa_mk_value(2), GFP_KERNEL));
	if (result)
		goto out;
	/* Reservation cleanup must never erase a real entry installed by a
	 * concurrent successful mapper. IRQsave must also preserve nested masks.
	 */
	local_irq_save(flags);
	result = kobox_linux_dma_gate_xa_reserve(&gate->array,
		DMA_XARRAY_TARGET, GFP_ATOMIC);
	kobox_linux_dma_gate_xa_release(&gate->array, DMA_XARRAY_TARGET);
	if (!irqs_disabled() || xa_load(&gate->array, DMA_XARRAY_TARGET) !=
	    xa_mk_value(2))
		result = -EINVAL;
	local_irq_restore(flags);
	if (irqs_disabled())
		result = -EINVAL;
out:
	xa_destroy(&gate->array);
	return result;
}

#if defined(CONFIG_FAILSLAB) && defined(CONFIG_FAIL_PAGE_ALLOC) && \
	defined(CONFIG_FAULT_INJECTION_DEBUG_FS)
static int dma_xarray_persistent(void)
{
	struct {
		const char *name, *value;
		char saved[32];
		struct file *file;
		bool changed;
	} controls[] = {
		{.name = "failslab/cache-filter", .value = "N"},
		{.name = "failslab/task-filter", .value = "Y"},
		{.name = "failslab/interval", .value = "1"},
		{.name = "failslab/space", .value = "0"},
		{.name = "failslab/times", .value = "-1"},
		{.name = "failslab/probability", .value = "100"},
	};
	const gfp_t modes[] = {GFP_ATOMIC, GFP_ATOMIC, GFP_KERNEL};
	struct file_system_type *type;
	struct vfsmount *mount;
	struct xarray array;
	unsigned int i, mode;
	int result = 0;

	if (READ_ONCE(current->make_it_fail) || READ_ONCE(current->fail_nth))
		return -EBUSY;
	type = get_fs_type("debugfs");
	if (!type)
		return -ENOENT;
	mount = kern_mount(type);
	put_filesystem(type);
	if (IS_ERR(mount))
		return PTR_ERR(mount);
	/* A single fail_nth is recovered by XArray's fallback allocation.
	 * Persistent upstream failures must cover both attempts, but only this
	 * task is marked during the helper: IRQs and other tasks stay unaffected.
	 * Save controls before changing them and restore even partial setup.
	 */
	for (i = 0; i < ARRAY_SIZE(controls); i++) {
		loff_t position = 0;
		ssize_t bytes;

		controls[i].file = file_open_root_mnt(mount, controls[i].name,
						    O_RDWR, 0);
		if (IS_ERR(controls[i].file)) {
			result = PTR_ERR(controls[i].file);
			controls[i].file = NULL;
			goto restore;
		}
		bytes = vfs_read(controls[i].file,
			(char __user *)controls[i].saved,
			sizeof(controls[i].saved) - 1, &position);
		if (bytes <= 0) {
			result = bytes ?: -EINVAL;
			goto restore;
		}
		controls[i].saved[bytes] = '\0';
		position = 0;
		controls[i].changed = true;
		bytes = vfs_write(controls[i].file,
			(const char __user *)controls[i].value,
			strlen(controls[i].value), &position);
		if (bytes != strlen(controls[i].value)) {
			result = bytes < 0 ? bytes : -EINVAL;
			goto restore;
		}
	}
	for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
		unsigned long flags = 0;
		bool disabled = mode == 1, bad_mask;

		xa_init_flags(&array, XA_FLAGS_LOCK_IRQ);
		result = xa_err(xa_store_irq(&array, DMA_XARRAY_VICTIM,
					    xa_mk_value(1), GFP_KERNEL));
		if (result) {
			xa_destroy(&array);
			goto restore;
		}
		if (disabled)
			local_irq_save(flags);
		WRITE_ONCE(current->make_it_fail, 1);
		result = kobox_linux_dma_gate_xa_reserve(&array,
			DMA_XARRAY_TARGET, modes[mode] | __GFP_NOWARN);
		WRITE_ONCE(current->make_it_fail, 0);
		bad_mask = irqs_disabled() != disabled;
		if (disabled)
			local_irq_restore(flags);
		if (irqs_disabled())
			local_irq_enable();
		kobox_linux_dma_gate_xa_release(&array, DMA_XARRAY_TARGET);
		if (result != -ENOMEM || bad_mask ||
		    xa_load(&array, DMA_XARRAY_TARGET) ||
		    xa_load(&array, DMA_XARRAY_VICTIM) != xa_mk_value(1)) {
			pr_err("kobox-dma: xarray persistent mode=%u result=%d bad_mask=%u\n",
				mode, result, bad_mask);
			result = -EINVAL;
			xa_destroy(&array);
			goto restore;
		}
		result = kobox_linux_dma_gate_xa_reserve(&array,
			DMA_XARRAY_TARGET, modes[mode]);
		kobox_linux_dma_gate_xa_release(&array, DMA_XARRAY_TARGET);
		if (!result && (irqs_disabled() ||
		    xa_erase(&array, DMA_XARRAY_VICTIM) != xa_mk_value(1) ||
		    !xa_empty(&array)))
			result = -EINVAL;
		xa_destroy(&array);
		if (result)
			goto restore;
		pr_info("kobox-dma: xarray persistent mode=%u irq_disabled=%u result=-ENOMEM unwind/preserve/recovery PASS\n",
			mode, disabled);
	}
restore:
	WRITE_ONCE(current->make_it_fail, 0);
	for (i = ARRAY_SIZE(controls); i; i--) {
		loff_t position = 0;
		ssize_t bytes;

		if (controls[i - 1].changed) {
			bytes = vfs_write(controls[i - 1].file,
				(const char __user *)controls[i - 1].saved,
				strlen(controls[i - 1].saved), &position);
			if (bytes != strlen(controls[i - 1].saved) && !result)
				result = bytes < 0 ? bytes : -EINVAL;
		}
		if (controls[i - 1].file)
			__fput_sync(controls[i - 1].file);
	}
	kern_unmount(mount);
	return result;
}

static int dma_xarray_failures(void *argument)
{
	const struct {
		gfp_t gfp;
		bool disabled;
	} modes[] = {{GFP_ATOMIC, false}, {GFP_ATOMIC, true},
		     {GFP_KERNEL, false}};
	struct xarray array;
	unsigned int mode, nth, failed, injected;
	int result;

	for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
		failed = 0;
		injected = 0;
		pr_info("kobox-dma: xarray fail_nth start mode=%u irq_disabled=%u\n",
			mode, modes[mode].disabled);
		/* Walk every allocation point until nth lies beyond this operation.
		 * The upstream per-task counter never affects cleanup or recovery.
		 */
		for (nth = 1; nth <= 64; nth++) {
			unsigned int remaining;
			unsigned long flags = 0;
			bool bad_mask;

			xa_init_flags(&array, XA_FLAGS_LOCK_IRQ);
			if (modes[mode].disabled)
				local_irq_save(flags);
			WRITE_ONCE(current->fail_nth, nth);
			result = kobox_linux_dma_gate_xa_reserve(&array,
				DMA_XARRAY_TARGET, modes[mode].gfp | __GFP_NOWARN);
			remaining = READ_ONCE(current->fail_nth);
			WRITE_ONCE(current->fail_nth, 0);
			bad_mask = irqs_disabled() != modes[mode].disabled;
			if (modes[mode].disabled)
				local_irq_restore(flags);
			if (irqs_disabled())
				local_irq_enable();
			if (bad_mask || (result && result != -ENOMEM) ||
			    (result && remaining)) {
				pr_err("kobox-dma: xarray fail_nth mask/result mode=%u nth=%u result=%d remaining=%u bad_mask=%u\n",
					mode, nth, result, remaining, bad_mask);
				xa_destroy(&array);
				return -EINVAL;
			}
			if (result)
				failed++;
			if (!remaining)
				injected++;
			kobox_linux_dma_gate_xa_release(&array, DMA_XARRAY_TARGET);
			if (!xa_empty(&array)) {
				pr_err("kobox-dma: xarray fail_nth unwind mode=%u nth=%u result=%d remaining=%u head=%px entry=%px\n",
					mode, nth, result, remaining,
					array.xa_head, xa_load(&array, DMA_XARRAY_TARGET));
				xa_destroy(&array);
				return -EINVAL;
			}
			result = kobox_linux_dma_gate_xa_reserve(&array,
				DMA_XARRAY_TARGET, modes[mode].gfp);
			kobox_linux_dma_gate_xa_release(&array, DMA_XARRAY_TARGET);
			if (result || !xa_empty(&array) || irqs_disabled()) {
				pr_err("kobox-dma: xarray fail_nth recovery mode=%u nth=%u result=%d empty=%u masked=%u\n",
					mode, nth, result, xa_empty(&array), irqs_disabled());
				xa_destroy(&array);
				return -EINVAL;
			}
			xa_destroy(&array);
			if (remaining)
				break;
		}
		if (!injected || nth > 64) {
			pr_err("kobox-dma: xarray fail_nth exhausted mode=%u failed=%u nth=%u\n",
				mode, failed, nth);
			return -EINVAL;
		}
		pr_info("kobox-dma: xarray fail_nth mode=%u irq_disabled=%u injected=%u ENOMEM=%u swept=%u recovered=%u\n",
			mode, modes[mode].disabled, injected, failed, nth, nth);
	}
	return dma_xarray_persistent();
}
#endif

int kobox_linux_dma_xarray_verify(void)
{
	int result;

	if (irqs_disabled())
		return -EINVAL;
	result = dma_xarray_case(GFP_ATOMIC, false, false);
	if (!result)
		result = dma_xarray_case(GFP_ATOMIC, false, true);
	if (!result)
		result = dma_xarray_case(GFP_KERNEL, false, false);
	if (!result)
		result = dma_xarray_case(GFP_ATOMIC, true, false);
	if (!result)
		result = dma_xarray_case(GFP_ATOMIC, true, true);
	if (!result)
		result = dma_xarray_preserve();
#if defined(CONFIG_FAILSLAB) && defined(CONFIG_FAIL_PAGE_ALLOC) && \
	defined(CONFIG_FAULT_INJECTION_DEBUG_FS)
	if (!result)
		result = kobox_linux_with_allocation_failures(dma_xarray_failures, NULL);
	if (!result)
		pr_info("kobox-dma: xarray controlled ENOMEM unwind/recovery PASS\n");
#else
	pr_info("kobox-dma: xarray ENOMEM injection unavailable in this config\n");
#endif
	if (!result)
		pr_info("kobox-dma: xarray same-CPU hardIRQ defer/drain IRQrestore GFP_KERNEL/ATOMIC reserved-only release PASS\n");
	return result;
}

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

static int shared_queue_page(struct device *dev,
			     struct kobox_linux_dma_port *port,
			     const struct kobox_linux_dma_test *test)
{
	struct virtio_device vdev = {.dev.parent = dev};
	union virtio_map map = {.dma_dev = dev};
	struct page *page = alloc_page(GFP_KERNEL);
	dma_addr_t first = DMA_MAPPING_ERROR, peer = DMA_MAPPING_ERROR;
	dma_addr_t read_only = DMA_MAPPING_ERROR;
	u32 value = 0x97531246;
	int references, result = -EINVAL;

	if (!page)
		return -ENOMEM;
	INIT_LIST_HEAD(&vdev.vqs);
	if (kobox_linux_dma_bind_virtio(port, &vdev))
		goto free_page;
	references = page_count(page);
	first = vdev.map->map_page(map, page, 128, 64, DMA_BIDIRECTIONAL, 0);
	peer = vdev.map->map_page(map, page, 256, 128, DMA_BIDIRECTIONAL, 0);
	if (dma_mapping_error(dev, first) || dma_mapping_error(dev, peer) ||
	    peer != first + 128 || page_count(page) != references + 1 ||
	    kobox_linux_dma_detach(port) != -EBUSY)
		goto unmap;
	read_only = vdev.map->map_page(map, page, 128, 64, DMA_TO_DEVICE, 0);
	if (dma_mapping_error(dev, read_only) || read_only == first ||
	    transfer(test, read_only, &value, true) != -EACCES)
		goto unmap;
	vdev.map->unmap_page(map, read_only, 64, DMA_TO_DEVICE, 0);
	read_only = DMA_MAPPING_ERROR;
	vdev.map->unmap_page(map, first, 64, DMA_BIDIRECTIONAL, 0);
	first = DMA_MAPPING_ERROR;
	if (page_count(page) != references + 1 ||
	    transfer(test, peer, &value, true) ||
	    *(u32 *)(page_address(page) + 256) != value)
		goto unmap;
	vdev.map->unmap_page(map, peer, 128, DMA_BIDIRECTIONAL, 0);
	if (page_count(page) != references ||
	    transfer(test, peer, &value, false) != -EFAULT) {
		peer = DMA_MAPPING_ERROR;
		goto unmap;
	}
	peer = DMA_MAPPING_ERROR;
	/* Failed first publication must not install an entry for a later hit. */
	fail_map(test, 1);
	first = vdev.map->map_page(map, page, 128, 64, DMA_BIDIRECTIONAL, 0);
	fail_map(test, 0);
	if (!dma_mapping_error(dev, first) || page_count(page) != references)
		goto unmap;
	first = vdev.map->map_page(map, page, 128, 64, DMA_BIDIRECTIONAL, 0);
	if (!dma_mapping_error(dev, first) &&
	    !transfer(test, first, &value, true))
		result = 0;
unmap:
	if (!dma_mapping_error(dev, read_only))
		vdev.map->unmap_page(map, read_only, 64, DMA_TO_DEVICE, 0);
	if (!dma_mapping_error(dev, peer))
		vdev.map->unmap_page(map, peer, 128, DMA_BIDIRECTIONAL, 0);
	if (!dma_mapping_error(dev, first))
		vdev.map->unmap_page(map, first, 64, DMA_BIDIRECTIONAL, 0);
free_page:
	__free_page(page);
	return result;
}

static int bounced_queue_buffer(struct device *dev,
		struct kobox_linux_dma_port *port,
		const struct kobox_linux_dma_test *test)
{
	struct virtio_device vdev = {.dev.parent = dev};
	union virtio_map map = {.dma_dev = dev};
	struct page *page = alloc_page(GFP_KERNEL);
	dma_addr_t send = DMA_MAPPING_ERROR, receive = DMA_MAPPING_ERROR;
	dma_addr_t saturation[129];
	unsigned int allocated = 0;
	u32 value = 0, *cpu;
	int result = -EINVAL;

	if (!page)
		return -ENOMEM;
	cpu = page_address(page);
	memset(cpu, 0x3a, PAGE_SIZE);
	INIT_LIST_HEAD(&vdev.vqs);
	if (kobox_linux_dma_bind_virtio(port, &vdev))
		goto out;
	send = vdev.map->map_page(map, page, 128, 64, DMA_TO_DEVICE, 0);
	receive = vdev.map->map_page(map, page, 256, 128, DMA_FROM_DEVICE, 0);
	if (dma_mapping_error(dev, send) || dma_mapping_error(dev, receive) ||
	    !vdev.map->need_sync(map, send) ||
	    !vdev.map->need_sync(map, receive) ||
	    kobox_linux_dma_detach(port) != -EBUSY ||
	    transfer(test, send, &value, false) || value != 0x3a3a3a3a ||
	    transfer(test, send, &value, true) != -EACCES ||
	    transfer(test, receive, &value, false) != -EACCES)
		goto out;
	value = 0x12345678;
	if (transfer(test, receive + 4, &value, true) ||
	    cpu[65] != 0x3a3a3a3a)
		goto out;
	vdev.map->sync_single_for_cpu(map, receive + 4, sizeof(value),
				      DMA_FROM_DEVICE);
	if (cpu[65] != value || cpu[64] != 0x3a3a3a3a)
		goto out;
	cpu[32] = 0x87654321;
	vdev.map->sync_single_for_device(map, send, sizeof(value), DMA_TO_DEVICE);
	if (transfer(test, send, &value, false) || value != cpu[32])
		goto out;
	value = 0x76543210;
	if (transfer(test, receive, &value, true) || cpu[64] != 0x3a3a3a3a)
		goto out;
	vdev.map->unmap_page(map, receive, 128, DMA_FROM_DEVICE, 0);
	receive = DMA_MAPPING_ERROR;
	if (cpu[64] != value || cpu[65] != 0x12345678 ||
	    cpu[66] != 0x3a3a3a3a || page_count(page) != 1)
		goto out;
	vdev.map->unmap_page(map, send, 64, DMA_TO_DEVICE, 0);
	/* An idle private slot is cleared; the caller's RAM is untouched. */
	result = transfer(test, send, &value, false) || value ||
		cpu[32] != 0x87654321 ? -EINVAL : 0;
	send = DMA_MAPPING_ERROR;
	if (result)
		goto out;
	/* Fill the bounded pool, then exercise the ordinary IOMMU fallback. */
	while (allocated < ARRAY_SIZE(saturation)) {
		dma_addr_t address = vdev.map->map_page(map, page, 128, 64,
						     DMA_TO_DEVICE, 0);

		if (dma_mapping_error(dev, address)) {
			result = -ENOMEM;
			goto out;
		}
		saturation[allocated++] = address;
	}
	if (vdev.map->need_sync(map, saturation[128]) || page_count(page) != 2)
		result = -EINVAL;
out:
	while (allocated)
		vdev.map->unmap_page(map, saturation[--allocated], 64,
				     DMA_TO_DEVICE, 0);
	if (!result && (page_count(page) != 1 ||
	    transfer(test, saturation[128], &value, false) != -EFAULT))
		result = -EINVAL;
	if (!dma_mapping_error(dev, receive))
		vdev.map->unmap_page(map, receive, 128, DMA_FROM_DEVICE, 0);
	if (!dma_mapping_error(dev, send))
		vdev.map->unmap_page(map, send, 64, DMA_TO_DEVICE, 0);
	__free_page(page);
	return result;
}

static int bounced_queue_span(struct device *dev,
		struct kobox_linux_dma_port *port,
		const struct kobox_linux_dma_test *test)
{
	struct virtio_device vdev = {.dev.parent = dev};
	union virtio_map map = {.dma_dev = dev};
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, 2);
	const unsigned int offset = PAGE_SIZE - 32, size = 2 * PAGE_SIZE;
	const unsigned int probes[] = {0, 28, 32, PAGE_SIZE, size - 4};
	dma_addr_t addresses[33], address = DMA_MAPPING_ERROR;
	unsigned int allocated = 0, i;
	u32 value = 0, *cpu;
	int result = -EINVAL;

	if (!page)
		return -ENOMEM;
	cpu = page_address(page) + offset;
	memset(page_address(page), 0x6b, 4 * PAGE_SIZE);
	INIT_LIST_HEAD(&vdev.vqs);
	if (kobox_linux_dma_bind_virtio(port, &vdev))
		goto out;
	address = vdev.map->map_page(map, page, offset, size, DMA_TO_DEVICE, 0);
	if (dma_mapping_error(dev, address) ||
	    !vdev.map->need_sync(map, address) || page_count(page) != 1 ||
	    transfer(test, address, &value, true) != -EACCES ||
	    kobox_linux_dma_detach(port) != -EBUSY)
		goto out;
	for (i = 0; i < ARRAY_SIZE(probes); i++)
		if (transfer(test, address + probes[i], &value, false) ||
		    value != 0x6b6b6b6b)
			goto out;
	/* A range sync crossing the source page boundary must copy both pages. */
	cpu[7] = 0x12345678;
	cpu[8] = 0x87654321;
	vdev.map->sync_single_for_device(map, address + 28, 8, DMA_TO_DEVICE);
	if (transfer(test, address + 28, &value, false) || value != cpu[7] ||
	    transfer(test, address + 32, &value, false) || value != cpu[8])
		goto out;
	vdev.map->unmap_page(map, address, size, DMA_TO_DEVICE, 0);
	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		if (transfer(test, address + probes[i], &value, false) || value) {
			address = DMA_MAPPING_ERROR;
			goto out;
		}
	}
	address = DMA_MAPPING_ERROR;
	if (cpu[7] != 0x12345678 || cpu[8] != 0x87654321)
		goto out;
	/* Attributes and other directions retain the ordinary DMA contract. */
	address = vdev.map->map_page(map, page, offset, size, DMA_TO_DEVICE,
				   DMA_ATTR_SKIP_CPU_SYNC);
	if (dma_mapping_error(dev, address))
		goto out;
	i = vdev.map->need_sync(map, address);
	vdev.map->unmap_page(map, address, size, DMA_TO_DEVICE,
			     DMA_ATTR_SKIP_CPU_SYNC);
	address = DMA_MAPPING_ERROR;
	if (i || page_count(page) != 1)
		goto out;
	address = vdev.map->map_page(map, page, offset, size, DMA_FROM_DEVICE, 0);
	if (dma_mapping_error(dev, address))
		goto out;
	i = vdev.map->need_sync(map, address);
	vdev.map->unmap_page(map, address, size, DMA_FROM_DEVICE, 0);
	address = DMA_MAPPING_ERROR;
	if (i || page_count(page) != 1)
		goto out;
	/* Saturation falls back to a live-only mapping of the original span. */
	while (allocated < ARRAY_SIZE(addresses)) {
		address = vdev.map->map_page(map, page, offset, size,
					   DMA_TO_DEVICE, 0);
		if (dma_mapping_error(dev, address))
			goto out;
		addresses[allocated++] = address;
		address = DMA_MAPPING_ERROR;
	}
	if (!vdev.map->need_sync(map, addresses[31]) ||
	    vdev.map->need_sync(map, addresses[32]) || page_count(page) <= 1)
		goto out;
	result = 0;
out:
	while (allocated)
		vdev.map->unmap_page(map, addresses[--allocated], size,
				     DMA_TO_DEVICE, 0);
	if (!dma_mapping_error(dev, address))
		vdev.map->unmap_page(map, address, size, DMA_TO_DEVICE, 0);
	if (!result && (page_count(page) != 1 ||
	    transfer(test, addresses[32], &value, false) != -EFAULT))
		result = -EINVAL;
	__free_pages(page, 2);
	return result;
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

static int noncompound_rx_buffer(struct device *dev,
				 const struct kobox_linux_dma_test *test)
{
	const size_t length = 4 * PAGE_SIZE - 1;
	const size_t probe = 4 * PAGE_SIZE - 8;
	struct page *head = alloc_pages(GFP_KERNEL, 2);
	dma_addr_t address = DMA_MAPPING_ERROR;
	u32 value = 0x81726354;
	int result = -EINVAL;

	if (!head)
		return -ENOMEM;
	/* Drivers such as r8169 use order-2 pages without __GFP_COMP. Only
	 * the allocation head has a reference; the three tails must still map. */
	if (page_count(head) != 1 || page_count(head + 1) ||
	    page_count(head + 2) || page_count(head + 3))
		goto out;
	address = dma_map_page(dev, head, 0, length, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, address) || page_count(head) != 2)
		goto out;
	if (transfer(test, address + probe, &value, true))
		goto out;
	dma_sync_single_for_cpu(dev, address, length, DMA_FROM_DEVICE);
	if (*(u32 *)(page_address(head) + probe) != value)
		goto out;
	dma_unmap_page(dev, address, length, DMA_FROM_DEVICE);
	if (page_count(head) == 1 &&
	    transfer(test, address, &value, false) == -EFAULT)
		result = 0;
	address = DMA_MAPPING_ERROR;
out:
	if (!dma_mapping_error(dev, address))
		dma_unmap_page(dev, address, length, DMA_FROM_DEVICE);
	__free_pages(head, 2);
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
	result = kobox_linux_dma_xarray_verify();
	if (result)
		goto out;
	report->cases++;
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
	result = noncompound_rx_buffer(&device->dev, test);
	if (result)
		goto out;
	report->cases++;
	report->phase = 10;
	result = slab_buffers(&device->dev, test);
	if (result)
		goto out;
	result = shared_queue_page(&device->dev, port, test);
	if (result)
		goto out;
	result = bounced_queue_buffer(&device->dev, port, test);
	if (result)
		goto out;
	result = bounced_queue_span(&device->dev, port, test);
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
