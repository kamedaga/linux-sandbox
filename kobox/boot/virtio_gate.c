// SPDX-License-Identifier: GPL-2.0-only

#include "virtio_gate.h"
#include "exception.h"
#include "../arch/x86_64/host_call.h"

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/fcntl.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/string.h>
#include <linux/sync_file.h>
#include <linux/swap.h>
#include <linux/task_work.h>
#include <linux/vmalloc.h>
#include <asm/ptrace.h>
#include <asm/unistd.h>
#include <drm/drm.h>
#pragma GCC diagnostic push
/* Upstream virtio_config.h uses a valid partial aggregate initializer. */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "../../drivers/gpu/drm/virtio/virtgpu_drv.h"
#pragma GCC diagnostic pop

long __x64_sys_init_module(const struct pt_regs *regs);
long __x64_sys_delete_module(const struct pt_regs *regs);
void flush_module_init_free_work(void);

#define REQUIRE(expression) do { \
	if (!(expression)) { result = -EINVAL; report->line = __LINE__; goto cleanup; } \
} while (0)

static int gpu_child(struct device *device, const void *argument)
{
	return device->bus && !strcmp(device->bus->name, "virtio") &&
	       device->driver && !strcmp(device->driver->name, "virtio_gpu");
}

static int drm_node(struct device *device, void *argument)
{
	struct kobox_exec_node *nodes = argument;
	unsigned int index;

	if (!device->class || strcmp(device->class->name, "drm") || !device->devt)
		return 0;
	if (!strncmp(dev_name(device), "renderD", 7))
		index = 1;
	else if (!strncmp(dev_name(device), "card", 4))
		index = 0;
	else
		return 0;
	nodes[index].major = MAJOR(device->devt);
	nodes[index].minor = MINOR(device->devt);
	return 0;
}

static int consumer_node(struct device *device, void *argument)
{
	struct kobox_exec_node *node = argument;

	if (device->class && !strcmp(device->class->name, "misc") &&
	    !strcmp(dev_name(device), "dma-consumer") && device->devt) {
		node->major = MAJOR(device->devt);
		node->minor = MINOR(device->devt);
	}
	return 0;
}

static void drain_consumer_lifetime(void)
{
	task_work_run();
	flush_delayed_fput();
	lru_add_drain_all();
	rcu_barrier();
}

static bool client_release_idle(struct virtio_gpu_device *vgdev,
				unsigned int control, unsigned int cursor)
{
	bool idle;

	/* Take one snapshot: a running callback can otherwise enqueue a new
	 * unref between separately checking free descriptors and work state.
	 */
	spin_lock(&vgdev->ctrlq.qlock);
	spin_lock(&vgdev->cursorq.qlock);
	spin_lock(&vgdev->obj_free_lock);
	idle = vgdev->ctrlq.vq->num_free == control &&
		vgdev->cursorq.vq->num_free == cursor &&
		list_empty(&vgdev->obj_free_list) &&
		!work_busy(&vgdev->ctrlq.dequeue_work) &&
		!work_busy(&vgdev->cursorq.dequeue_work) &&
		!work_busy(&vgdev->obj_free_work);
	spin_unlock(&vgdev->obj_free_lock);
	spin_unlock(&vgdev->cursorq.qlock);
	spin_unlock(&vgdev->ctrlq.qlock);
	return idle;
}

/* A normal client close can enqueue asynchronous RESOURCE_UNREF. Observe
 * its real response before this fixture unloads the driver. flush_work alone
 * cannot wait for hardware which has not delivered its interrupt yet.
 * This supervises the test; it does not replace driver remove or IRQ work.
 */
static int wait_client_release(struct device *gpu)
{
	struct virtio_device *vdev = dev_to_virtio(gpu);
	struct drm_device *drm = vdev->priv;
	struct virtio_gpu_device *vgdev = drm->dev_private;
	unsigned int (*ring_size)(const struct virtqueue *vq);
	unsigned long deadline = jiffies + 5 * HZ;
	unsigned int control, cursor;
	int result = -ETIMEDOUT;

	ring_size = __symbol_get("virtqueue_get_vring_size");
	if (!ring_size)
		return -ENOENT;
	control = ring_size(vgdev->ctrlq.vq);
	cursor = ring_size(vgdev->cursorq.vq);
	flush_delayed_fput();
	do {
		flush_work(&vgdev->config_changed_work);
		flush_work(&vgdev->ctrlq.dequeue_work);
		flush_work(&vgdev->cursorq.dequeue_work);
		flush_work(&vgdev->obj_free_work);
		if (client_release_idle(vgdev, control, cursor)) {
			result = 0;
			break;
		}
		msleep(1);
	} while (time_before(jiffies, deadline));
	__symbol_put("virtqueue_get_vring_size");
	return result;
}

static int wait_client_phase(struct file *file, uint32_t expected)
{
	unsigned long deadline = jiffies + 10 * HZ;
	uint32_t phase;

	do {
		loff_t offset = KOBOX_EXEC_DEVICE_OFFSET;
		ssize_t size = kernel_read(file, &phase, sizeof(phase), &offset);

		if (size < 0)
			return size;
		if (size == sizeof(phase) && phase == expected)
			return 0;
		msleep(1);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int client_phase(struct file *file, uint32_t phase)
{
	loff_t offset = KOBOX_EXEC_DEVICE_OFFSET;

	return kernel_write(file, &phase, sizeof(phase), &offset) == sizeof(phase) ?
		0 : -EIO;
}

struct removal_observer {
	struct pci_dev *pci;
	const struct kobox_linux_virtio_test *test;
};

static int shared_waiter(struct file *result_file, unsigned int role,
			 int client_pid)
{
	struct kobox_exec_fence_wait record;
	loff_t offset = KOBOX_EXEC_FENCE_WAIT_OFFSET + role * sizeof(record);
	struct task_struct *task;
	struct dma_fence *fence;
	struct file *file;
	int fd, result = -EBUSY;

	if (kernel_read(result_file, &record, sizeof(record), &offset) != sizeof(record) ||
	    !record.pid || record.fd > INT_MAX)
		return -EAGAIN;
	task = find_get_task_by_vpid(record.pid);
	if (!task)
		return -ESRCH;
	rcu_read_lock();
	if ((!role && task_pid_vnr(task) != client_pid) ||
	    (role && task_pid_vnr(rcu_dereference(task->real_parent)) != client_pid))
		result = -EINVAL;
	rcu_read_unlock();
	if (result == -EINVAL || !wait_task_inactive(task, TASK_INTERRUPTIBLE) ||
	    task_pt_regs(task)->orig_ax != __NR_poll)
		goto out;
	file = fget_task(task, record.fd);
	if (!file)
		goto out;
	/* Borrow the real file into the supervisor's table so the upstream
	 * sync_file API checks its type and takes a fence reference for us.
	 */
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(file);
		result = fd;
		goto out;
	}
	fd_install(fd, file);
	fence = sync_file_get_fence(fd);
	close_fd(fd);
	if (fence) {
		if (!dma_fence_is_signaled(fence))
			result = 0;
		dma_fence_put(fence);
	}
out:
	put_task_struct(task);
	return result;
}

static int shared_during_client(void *context, struct file *file,
				unsigned int cpu, int child_pid)
{
	struct removal_observer *observer = context;
	const struct kobox_linux_virtio_test *test = observer->test;
	unsigned long deadline;
	int result, index;

	result = wait_client_phase(file, 5);
	if (result)
		return result;
	for (index = 0; index < 3; index++)
		disable_irq(pci_irq_vector(observer->pci, index));
	result = client_phase(file, 6);
	if (!result)
		result = wait_client_phase(file, 1);
	deadline = jiffies + 3 * HZ;
	while (!result) {
		int parent = shared_waiter(file, 0, child_pid);
		int child = shared_waiter(file, 1, child_pid);

		if (!parent && !child)
			break;
		if (time_after_eq(jiffies, deadline)) {
			result = -ETIMEDOUT;
			break;
		}
		msleep(1);
	}
	if (!result) {
		pr_info("Mesa shared checkpoint: cpu=%u actual-poll=2 pending-driver-fences=2\n", cpu);
		if (test->death_checkpoint && cpu == test->death_cpu) {
			kobox_host_call(test->death_checkpoint(test->death_context));
			result = -EIO;
		}
	}
	for (index = 0; index < 3; index++)
		enable_irq(pci_irq_vector(observer->pci, index));
	return result;
}

static int actual_syncobj_waiters(int client_pid)
{
	struct task_struct *client, *task, *waiters[3] = {NULL, NULL, NULL};
	unsigned long deadline = jiffies + HZ;
	unsigned int count = 0, mask = 0, i;
	int result = -EBUSY;

	client = find_get_task_by_vpid(client_pid);
	if (!client)
		return -ESRCH;
	rcu_read_lock();
	for_each_process(task) {
		if (rcu_dereference(task->real_parent) != client)
			continue;
		if (count == ARRAY_SIZE(waiters)) {
			count++;
			break;
		}
		get_task_struct(task);
		waiters[count++] = task;
	}
	rcu_read_unlock();
	put_task_struct(client);
	if (count != 2 && count != 3)
		goto out;
	for (i = 0; i < count; i++) {
		struct pt_regs *regs;

		while (!wait_task_inactive(waiters[i], TASK_INTERRUPTIBLE)) {
			if (time_after_eq(jiffies, deadline))
				goto out;
			msleep(1);
		}
		regs = task_pt_regs(waiters[i]);
		if (count == 3 && regs->orig_ax == __NR_nanosleep)
			continue;
		if (regs->orig_ax != __NR_ioctl)
			goto out;
		if (regs->si == DRM_IOCTL_SYNCOBJ_WAIT)
			mask |= 1;
		else if (regs->si == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT)
			mask |= 2;
		else
			goto out;
	}
	if (mask == 3)
		result = 0;
out:
	for (i = 0; i < ARRAY_SIZE(waiters); i++)
		if (waiters[i])
			put_task_struct(waiters[i]);
	return result;
}

static int remove_during_client(void *context, struct file *file, unsigned int cpu, int child_pid)
{
	struct removal_observer *observer = context;
	struct pci_dev *pci = observer->pci;
	const struct kobox_linux_virtio_test *test = observer->test;
	struct kobox_exec_node nodes[] = {
		{.name = "card0", .mode = 0666},
		{.name = "renderD128", .mode = 0666},
	};
	int result, index;

	result = wait_client_phase(file, 5);
	if (result)
		return result;
	/* Hold actual Linux IRQ lines before the fresh fenced submission.
	 * This removes any dependence on the timing of earlier coalesced MSIs.
	 * Driver-core unbind below destroys these disabled IRQ descriptors.
	 */
	for (index = 0; index < 3; index++)
		disable_irq(pci_irq_vector(pci, index));
	result = client_phase(file, 6);
	if (!result)
		result = wait_client_phase(file, 1);
	if (result)
		return result;
	/* A userspace readiness flag is not proof of ioctl entry. Require
	 * both upstream waits to be inactive before either remove or death.
	 */
	result = actual_syncobj_waiters(child_pid);
	if (result)
		return result;
	if (test->death_checkpoint && cpu == test->death_cpu) {
		if (test->arm_fault) {
			result = kobox_host_call(test->arm_fault(test->death_context,
				KOBOX_EXEC_REVOKE_ADDRESS));
			if (result)
				return result;
			result = client_phase(file, 7);
			if (result)
				return result;
			/* The real client will fault; its Linux fault path must reach
			 * the machine map checkpoint before the owner can kill us.
			 */
			msleep(5000);
			return -ETIMEDOUT;
		}
		/* Both real ioctls are sleeping; masked device IRQs keep their
		 * virtio-gpu fences pending. No remove/cleanup has started.
		 * The host checkpoint must not return: the owner sends SIGKILL.
		 */
		kobox_host_call(test->death_checkpoint(test->death_context));
		return -EIO;
	}
	/* Only the real driver owns fence completion. The fixture invokes the
	 * same driver-core unbind used by sysfs, with the client's FDs live.
	 */
	device_release_driver(&pci->dev);
	if (pci->driver || pci->msix_enabled)
		return -EBUSY;
	/* Rebind with the retired DRM file, sync_file and GEM VMA still
	 * alive. Their subsequent operations must retain the old device's
	 * terminal state, never resolve to this new native driver instance.
	 */
	result = device_attach(&pci->dev);
	if (result != 1)
		return result < 0 ? result : -ENODEV;
	if (!pci->msix_enabled)
		return -EINVAL;
	/* Upstream keeps the old minor allocated while old files exist.
	 * Refresh only the fixture's stable paths, using the new real devt.
	 * Existing open files keep their retired inode/device identity.
	 */
	device_for_each_child(&pci->dev, nodes, drm_node);
	for (index = 0; index < ARRAY_SIZE(nodes); index++) {
		if (!nodes[index].major)
			return -ENODEV;
		result = kobox_linux_exec_replace_node(file, &nodes[index]);
		if (result)
			return result;
	}
	result = client_phase(file, 2);
	if (!result)
		result = wait_client_phase(file, 3);
	if (result)
		return result;
	return client_phase(file, 4);
}

/* This test loads native modules through upstream Linux's syscall entry,
 * registers a real PCI device, and observes binding. No driver entry point,
 * virtqueue response, or IRQ handler is replaced by the test.
 */
int kobox_linux_virtio_verify(const struct kobox_linux_virtio_test *test,
			      struct kobox_linux_virtio_report *report)
{
	struct pci_host_bridge *bridge = NULL;
	struct pci_host_bridge *consumer_bridge = NULL;
	struct pci_dev *pci = NULL;
	struct pci_dev *consumer = NULL;
	struct device *gpu = NULL;
	struct kobox_linux_dma_port *dma = NULL;
	struct kobox_linux_dma_port *consumer_dma = NULL;
	struct kobox_linux_irq_port *irq = NULL;
	struct pt_regs regs = {0};
	struct kobox_exec_node nodes[] = {
		{.name = "card0", .mode = 0666},
		{.name = "renderD128", .mode = 0666},
		{.name = "dma-consumer", .mode = 0666},
	};
	size_t index;
	int result = -EINVAL;

	if (!test || test->size != sizeof(*test) || !test->pci || !test->dma || !test->irq ||
	    !test->modules || !test->count || test->count > 64 ||
	    !report || report->size != sizeof(*report))
		return -EINVAL;
	report->phase = 1;
	result = kobox_linux_pci_scan(test->pci, &bridge);
	if (result)
		goto cleanup;
	pci_assign_unassigned_bus_resources(bridge->bus);
	pci = pci_get_slot(bridge->bus, test->pci->devfn);
	REQUIRE(pci && !pci->driver);
	result = kobox_linux_dma_attach(&pci->dev, test->dma, &dma);
	if (result)
		goto cleanup;
	report->phase = 2;
	result = kobox_linux_irq_attach(pci, test->irq, &irq);
	if (result)
		goto cleanup;
	pci_bus_add_devices(bridge->bus);
	if (test->consumer_pci) {
		REQUIRE(test->consumer_dma);
		result = kobox_linux_pci_scan(test->consumer_pci, &consumer_bridge);
		if (result)
			goto cleanup;
		pci_assign_unassigned_bus_resources(consumer_bridge->bus);
		consumer = pci_get_slot(consumer_bridge->bus, test->consumer_pci->devfn);
		REQUIRE(consumer && !consumer->driver);
		result = kobox_linux_dma_attach(&consumer->dev, test->consumer_dma, &consumer_dma);
		if (result)
			goto cleanup;
		pci_bus_add_devices(consumer_bridge->bus);
	}
	for (index = 0; index < test->count; index++) {
		const struct kobox_linux_native_module *module = &test->modules[index];

		report->phase = 10 + index;
		REQUIRE(module->image && module->length && module->name);
		regs.di = (unsigned long)module->image;
		regs.si = module->length;
		regs.dx = (unsigned long)(module->parameters ?: "");
		result = __x64_sys_init_module(&regs);
		if (result)
			goto cleanup;
		report->loaded++;
	}
	report->phase = 30;
	REQUIRE(pci->driver && !strcmp(pci->driver->name, "virtio-pci"));
	REQUIRE(pci->msix_enabled && !pci->msi_enabled);
	for (index = 0; index < 3; index++) {
		REQUIRE(pci_irq_vector(pci, index) > 0);
		report->vectors++;
	}
	gpu = device_find_child(&pci->dev, NULL, gpu_child);
	REQUIRE(gpu);
	report->bound = 1;
	device_for_each_child(&pci->dev, nodes, drm_node);
	REQUIRE(nodes[0].major && nodes[1].major);
	report->nodes = 2;
	if (consumer) {
		int (*setup)(void (*drain)(void));

		REQUIRE(consumer->driver && !strcmp(consumer->driver->name, "dma-consumer-test"));
		device_for_each_child(&consumer->dev, &nodes[2], consumer_node);
		REQUIRE(nodes[2].major);
		report->nodes++;
		setup = __symbol_get("kobox_dma_consumer_test_setup");
		REQUIRE(setup);
		result = setup(drain_consumer_lifetime);
		__symbol_put("kobox_dma_consumer_test_setup");
		if (result)
			goto cleanup;
	}
	if (test->client) {
		struct kobox_exec_test client = *test->client;

		client.nodes = nodes;
		client.node_count = report->nodes;
		if (test->observation == KOBOX_VIRTIO_OBSERVE_REMOVE) {
			/* Kept alive across the synchronous client verification below. */
			client.observe = remove_during_client;
		}
		if (test->observation == KOBOX_VIRTIO_OBSERVE_SHARED)
			client.observe = shared_during_client;
		report->phase = 31;
		report->client.size = sizeof(report->client);
		{
			struct removal_observer observer = {pci, test};

			client.observe_context = &observer;
			result = kobox_linux_exec_verify(&client, &report->client);
		}
		if (result)
			goto cleanup;
	}
	if (test->client && test->client->file_count &&
	    test->observation != KOBOX_VIRTIO_OBSERVE_REMOVE) {
		result = wait_client_release(gpu);
		if (result)
			goto cleanup;
	}
	for (index = 0; index < 3; index++) {
		unsigned int cpu;

		for_each_online_cpu(cpu)
			report->interrupts += kstat_irqs_cpu(pci_irq_vector(pci, index), cpu);
	}
	REQUIRE(report->interrupts);
	result = 0;
cleanup:
	put_device(gpu);
	/* Native driver removal drains its queues/work before the machine ports
	 * are detached. Never recycle an aperture with a live driver reference.
	 */
	for (index = report->loaded; index; index--) {
		regs.di = (unsigned long)test->modules[index - 1].name;
		regs.si = O_NONBLOCK;
		report->cleanup = __x64_sys_delete_module(&regs);
		if (report->cleanup)
			goto out;
		report->unloaded++;
	}
	flush_module_init_free_work();
	rcu_barrier();
	if (consumer_dma) {
		report->cleanup = kobox_linux_dma_detach(consumer_dma);
		if (report->cleanup)
			goto out;
	}
	pci_dev_put(consumer);
	if (consumer_bridge) {
		report->cleanup = kobox_linux_pci_remove(consumer_bridge);
		if (report->cleanup)
			goto out;
	}
	if (irq) {
		report->cleanup = kobox_linux_irq_detach(irq);
		if (report->cleanup)
			goto out;
	}
	if (dma) {
		report->cleanup = kobox_linux_dma_detach(dma);
		if (report->cleanup)
			goto out;
	}
	pci_dev_put(pci);
	if (bridge) {
		report->cleanup = kobox_linux_pci_remove(bridge);
		if (report->cleanup)
			goto out;
	}
	report->drained = 1;
out:
	report->warnings = kobox_linux_exception_warnings();
	if (report->warnings) {
		size_t used = 0;

		for (index = 0; index < report->warnings && index < 16; index++) {
			struct kobox_linux_warning warning;

			if (!kobox_linux_exception_warning(index, &warning))
				used += scnprintf(report->diagnostics + used, sizeof(report->diagnostics) - used,
						  "WARN cpu=%u %s:%u\n", warning.cpu, warning.file, warning.line);
		}
	}
	if (!result && report->warnings)
		result = -EINVAL;
	report->result = result ?: report->cleanup;
	return report->result;
}
