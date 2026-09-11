// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "device_proxy.h"
#include "device_channel.h"
#include "../../runtime/device_session.h"
#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>


_Static_assert(KOBOX_IRQ_INTX == KB2_DEVICE_PORT_IRQ_MODE_INTX &&
	KOBOX_IRQ_MSI == KB2_DEVICE_PORT_IRQ_MODE_MSI &&
	KOBOX_IRQ_MSIX == KB2_DEVICE_PORT_IRQ_MODE_MSIX, "IRQ mode encoding");
_Static_assert(KOBOX_MMIO_UC == KB2_DEVICE_PORT_CACHE_UC &&
	KOBOX_MMIO_UC_MINUS == KB2_DEVICE_PORT_CACHE_UC_MINUS, "MMIO cache encoding");
_Static_assert(KOBOX_LINUX_MEMORY_READ == KB2_DEVICE_PORT_MEMORY_READ &&
	KOBOX_LINUX_MEMORY_WRITE == KB2_DEVICE_PORT_MEMORY_WRITE &&
	KOBOX_DMA_DEVICE_READ == KB2_DEVICE_PORT_DEVICE_READ &&
	KOBOX_DMA_DEVICE_WRITE == KB2_DEVICE_PORT_DEVICE_WRITE, "access encoding");

struct lease {
	void *address;
	size_t length;
	uint64_t token;
	struct lease *next;
};

struct kobox_posix_proxy_device {
	struct kobox_posix_device_proxy *remote;
	uint64_t object;
	struct kobox_linux_pci_host pci;
	struct kobox_linux_dma_host dma;
	struct kobox_linux_irq_host irq;
	struct lease *leases;
	atomic_bool active;
	bool opened;
};

struct kobox_posix_device_proxy {
	pthread_mutex_t lock;
	int request, event;
	struct kobox_device_session session;
	struct kobox_posix_proxy_device *devices;
	size_t object_count;
	unsigned int logical_cpus;
	pthread_t notifier;
	int (*notify)(void *, unsigned int);
	void *notify_context;
	atomic_bool stopping;
	size_t retired, delivered;
	bool started, registered;
};

static void lock(struct kobox_posix_device_proxy *remote)
{
	if (pthread_mutex_lock(&remote->lock))
		abort();
}

static void unlock(struct kobox_posix_device_proxy *remote)
{
	if (pthread_mutex_unlock(&remote->lock))
		abort();
}

/* All Linux-facing callbacks are leaf calls with guest IRQs disabled. */
static int exchange_locked(struct kobox_posix_proxy_device *device,
			    uint32_t operation, const uint64_t *values, size_t count,
			    struct kobox_device_packet *reply, int descriptor)
{
	struct kobox_posix_device_proxy *remote = device->remote;
	struct kobox_device_packet request;
	enum kobox_device_session_result issued;
	int returned = -1, result;

	issued = kobox_device_session_issue(&remote->session, device->object,
					   operation, values, count, &request);
	if (issued != KOBOX_DEVICE_SESSION_OK)
		return issued == KOBOX_DEVICE_SESSION_OVERFLOW ? EOVERFLOW :
		       issued == KOBOX_DEVICE_SESSION_FAILED ? EIO : EINVAL;
	result = kobox_device_send(remote->request, &request, descriptor, 0);
	if (!result)
		result = kobox_device_receive(remote->request, reply, &returned);
	if (!result && (returned >= 0 ||
	    kobox_device_session_reply(&request, reply) != KOBOX_DEVICE_SESSION_OK))
		result = EPROTO;
	if (returned >= 0)
		close(returned);
	if (result) {
		remote->session.failed = true;
		return result;
	}
	return kobox_device_error(reply->status);
}

static int call(struct kobox_posix_proxy_device *device, uint32_t operation,
		 const uint64_t *values, size_t count, uint64_t *out, size_t output_count)
{
	struct kobox_device_packet reply;
	int result;

	lock(device->remote);
	result = exchange_locked(device, operation, values, count, &reply, -1);
	if (!result && reply.count != output_count)
		result = EPROTO;
	if (!result && output_count)
		memcpy(out, reply.values, output_count * sizeof(*out));
	unlock(device->remote);
	return -result;
}

static int config_read(void *context, uint32_t offset, uint32_t width, uint32_t *value)
{
	uint64_t input[] = {offset, width}, output;
	int result = call(context, KB2_DEVICE_PORT_OP_CONFIG_READ, input, 2, &output, 1);

	if (!result && output > UINT32_MAX)
		return -EPROTO;
	if (!result)
		*value = output;
	return result;
}

static int config_write(void *context, uint32_t offset, uint32_t width, uint32_t value)
{
	uint64_t input[] = {offset, width, value};

	return call(context, KB2_DEVICE_PORT_OP_CONFIG_WRITE, input, 3, NULL, 0);
}

static int memory_read(void *context, uint64_t physical, unsigned int width, uint64_t *value)
{
	uint64_t input[] = {physical, width};

	return call(context, KB2_DEVICE_PORT_OP_MMIO_READ, input, 2, value, 1);
}

static int memory_write(void *context, uint64_t physical, unsigned int width, uint64_t value)
{
	uint64_t input[] = {physical, width, value};

	return call(context, KB2_DEVICE_PORT_OP_MMIO_WRITE, input, 3, NULL, 0);
}

static int memory_map(void *context, void *address, uint64_t physical, size_t length,
		      unsigned int protection, enum kobox_mmio_cache cache)
{
	struct kobox_posix_proxy_device *device = context;
	struct kobox_device_packet reply;
	uint64_t input[] = {physical, length, protection, cache};
	struct lease *lease, *allocated = NULL;
	int result = 0;

	if (!address || !length || ((uintptr_t)address | length | physical) % 4096 ||
	    (uintptr_t)address > UINTPTR_MAX - length)
		return -EINVAL;
	if (cache != KOBOX_MMIO_UC && cache != KOBOX_MMIO_UC_MINUS)
		return -EOPNOTSUPP;
	lock(device->remote);
	for (lease = device->leases; lease; lease = lease->next) {
		if ((uintptr_t)address < (uintptr_t)lease->address + lease->length &&
		    (uintptr_t)lease->address < (uintptr_t)address + length) {
			result = EBUSY;
			goto out;
		}
	}
	allocated = calloc(1, sizeof(*allocated));
	if (!allocated) {
		result = ENOMEM;
		goto out;
	}
	result = exchange_locked(device, KB2_DEVICE_PORT_OP_MMIO_MAP, input, 4, &reply, -1);
	if (result)
		goto out;
	if (reply.count != 1 || !reply.values[0]) {
		result = EPROTO;
		goto out;
	}
	*allocated = (struct lease) {address, length, reply.values[0], device->leases};
	device->leases = allocated;
	allocated = NULL;
	if (mmap(address, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		 -1, 0) == MAP_FAILED) {
		uint64_t undo[] = {reply.values[0], length};

		result = errno;
		if (exchange_locked(device, KB2_DEVICE_PORT_OP_MMIO_UNMAP, undo, 2, &reply, -1))
			device->remote->session.failed = true;
		else {
			allocated = device->leases;
			device->leases = allocated->next;
		}
	}
out:
	free(allocated);
	unlock(device->remote);
	return -result;
}

static int memory_unmap(void *context, void *address, size_t length)
{
	struct kobox_posix_proxy_device *device = context;
	struct kobox_device_packet reply;
	struct lease **position;
	int result = EINVAL;

	lock(device->remote);
	for (position = &device->leases; *position; position = &(*position)->next) {
		struct lease *lease = *position;
		uint64_t input[] = {lease->token, length};

		if (!lease->token || lease->address != address || lease->length != length)
			continue;
		if (mmap(address, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
			 -1, 0) == MAP_FAILED) {
			result = errno;
			break;
		}
		result = exchange_locked(device, KB2_DEVICE_PORT_OP_MMIO_UNMAP, input, 2, &reply, -1);
		if (!result && reply.count)
			result = EPROTO;
		if (!result) {
			*position = lease->next;
			free(lease);
		}
		break;
	}
	unlock(device->remote);
	return -result;
}

static int dma_enable(void *context, unsigned int enabled)
{
	uint64_t input[] = {enabled};

	return call(context, KB2_DEVICE_PORT_OP_DMA_ENABLE, input, 1, NULL, 0);
}

static int dma_map(void *context, uint64_t iova, uint64_t offset, size_t length,
		   unsigned int protection)
{
	uint64_t input[] = {iova, offset, length, protection};

	return call(context, KB2_DEVICE_PORT_OP_DMA_MAP, input, 4, NULL, 0);
}

static int dma_unmap(void *context, uint64_t iova, size_t length)
{
	uint64_t input[] = {iova, length};

	return call(context, KB2_DEVICE_PORT_OP_DMA_UNMAP, input, 2, NULL, 0);
}

static int irq_allocate(void *context, enum kobox_linux_irq_mode mode,
			uint32_t index, uint32_t count, uint32_t cpu,
			struct kobox_linux_irq_route *routes)
{
	uint64_t input[] = {mode, index, count, cpu};
	uint64_t output[KB2_DEVICE_PORT_MAX_VALUES];
	unsigned int i;
	int result;

	if (!routes || !count || count > KB2_DEVICE_PORT_MAX_ROUTES)
		return -EINVAL;
	result = call(context, KB2_DEVICE_PORT_OP_IRQ_ALLOCATE, input, 4, output, count * 4);
	if (result)
		return result;
	for (i = 0; i < count; i++) {
		if (!output[i * 4 + 1] || output[i * 4 + 3] > UINT32_MAX)
			return -EPROTO;
		routes[i] = (struct kobox_linux_irq_route) {
			.hwirq = output[i * 4], .cookie = output[i * 4 + 1],
			.address = output[i * 4 + 2], .data = output[i * 4 + 3],
		};
	}
	return 0;
}

#define IRQ_PAIR(name, operation) \
static int name(void *context, uint64_t hwirq, uint64_t cookie) \
{ \
	uint64_t input[] = {hwirq, cookie}; \
	return call(context, operation, input, 2, NULL, 0); \
}
IRQ_PAIR(irq_release, KB2_DEVICE_PORT_OP_IRQ_RELEASE)
IRQ_PAIR(irq_ack, KB2_DEVICE_PORT_OP_IRQ_ACK)
IRQ_PAIR(irq_quiesce, KB2_DEVICE_PORT_OP_IRQ_QUIESCE)
IRQ_PAIR(irq_retrigger, KB2_DEVICE_PORT_OP_IRQ_RETRIGGER)

static int irq_mask(void *context, uint64_t hwirq, uint64_t cookie, unsigned int masked)
{
	uint64_t input[] = {hwirq, cookie, masked};

	return call(context, KB2_DEVICE_PORT_OP_IRQ_MASK, input, 3, NULL, 0);
}

static int irq_affinity(void *context, uint64_t hwirq, uint64_t cookie, uint32_t cpu)
{
	uint64_t input[] = {hwirq, cookie, cpu};

	return call(context, KB2_DEVICE_PORT_OP_IRQ_AFFINITY, input, 3, NULL, 0);
}

static int irq_active(void *context, uint64_t hwirq, uint64_t cookie, unsigned int *active)
{
	uint64_t input[] = {hwirq, cookie}, output;
	int result = call(context, KB2_DEVICE_PORT_OP_IRQ_ACTIVE, input, 2, &output, 1);

	if (!result && output > 1)
		return -EPROTO;
	if (!result)
		*active = output;
	return result;
}

static int irq_next(void *context, uint32_t cpu, struct kobox_linux_irq_event *event)
{
	uint64_t input[] = {cpu}, output[2];
	int result = call(context, KB2_DEVICE_PORT_OP_IRQ_NEXT, input, 1, output, 2);

	if (!result)
		*event = (struct kobox_linux_irq_event) {output[0], output[1]};
	return result;
}

int kobox_posix_device_proxy_open(struct kobox_posix_device_proxy *remote, uint64_t object,
			   int ram_descriptor, size_t ram_size, uint64_t delay,
			   struct kobox_posix_proxy_device **out)
{
	struct kobox_posix_proxy_device *device;
	struct kobox_device_packet reply;
	uint64_t input[] = {ram_size, delay};
	int result;

	if (!remote || !out || !object || object > remote->object_count)
		return EINVAL;
	*out = NULL;
	device = &remote->devices[object - 1];
	lock(remote);
	if (device->opened) {
		unlock(remote);
		return EBUSY;
	}
	device->remote = remote;
	device->object = object;
	result = exchange_locked(device, KB2_DEVICE_PORT_OP_OPEN, input, 2, &reply,
		remote->registered ? -1 : ram_descriptor);
	if (!result && (reply.count != 9 || reply.values[0] > UINT16_MAX ||
	    reply.values[1] > UINT8_MAX || reply.values[2] > UINT8_MAX ||
	    reply.values[3] != 1 || !reply.values[5] || reply.values[8] != 1))
		result = EPROTO;
	if (!result) {
		device->pci = (struct kobox_linux_pci_host) {
			.size = sizeof(device->pci), .context = device,
			.segment = reply.values[0], .bus = reply.values[1], .devfn = reply.values[2],
			.window_count = 1, .windows = {{reply.values[4], reply.values[5]}},
			.config_read = config_read, .config_write = config_write,
			.memory_map = memory_map, .memory_unmap = memory_unmap,
			.memory_read = memory_read, .memory_write = memory_write,
		};
		device->dma = (struct kobox_linux_dma_host) {
			.size = sizeof(device->dma), .context = device,
			.aperture_start = reply.values[6], .aperture_end = reply.values[7], .coherent = 1,
			.enable = dma_enable, .map = dma_map, .unmap = dma_unmap,
		};
		device->irq = (struct kobox_linux_irq_host) {
			.size = sizeof(device->irq), .context = device,
			.allocate = irq_allocate, .release = irq_release, .mask = irq_mask,
			.ack = irq_ack, .quiesce = irq_quiesce, .active = irq_active,
			.affinity = irq_affinity, .retrigger = irq_retrigger, .next = irq_next,
		};
		remote->registered = true;
		device->opened = true;
		atomic_store_explicit(&device->active, true, memory_order_release);
		*out = device;
	}
	unlock(remote);
	return result;
}

static void *notifications(void *context)
{
	struct kobox_posix_device_proxy *remote = context;

	while (!atomic_load_explicit(&remote->stopping, memory_order_acquire)) {
		struct kobox_device_packet event;
		int descriptor = -1;
		int result = kobox_device_receive(remote->event, &event, &descriptor);

		if (descriptor >= 0)
			close(descriptor);
		if (result || descriptor >= 0)
			break;
		result = kobox_device_session_event(&remote->session, &event,
						    remote->object_count, remote->logical_cpus);
		if (result == KOBOX_DEVICE_SESSION_STALE) {
			remote->retired++;
			continue;
		}
		if (result != KOBOX_DEVICE_SESSION_OK)
			continue;
		if (atomic_load_explicit(&remote->devices[event.object - 1].active, memory_order_acquire)) {
			if (remote->notify(remote->notify_context, event.values[0]))
				abort();
			remote->delivered++;
		}
	}
	return NULL;
}

int kobox_posix_device_proxy_start(struct kobox_posix_device_proxy *remote,
			    int (*notify)(void *, unsigned int), void *context)
{
	sigset_t children, previous;
	int result;

	if (!remote || !notify || remote->started)
		return EINVAL;
	remote->notify = notify;
	remote->notify_context = context;
	/* Preserve the VM service's signalfd ownership even when this producer
	 * is started first. Otherwise it can consume a client's SIGCHLD and
	 * strand that client's real ptrace stop indefinitely.
	 */
	sigemptyset(&children);
	sigaddset(&children, SIGCHLD);
	result = pthread_sigmask(SIG_BLOCK, &children, &previous);
	if (!result) {
		result = pthread_create(&remote->notifier, NULL, notifications, remote);
		if (pthread_sigmask(SIG_SETMASK, &previous, NULL))
			abort();
	}
	if (!result)
		remote->started = true;
	return result;
}

int kobox_posix_device_proxy_close(struct kobox_posix_proxy_device *device)
{
	int result;

	if (!device || !atomic_load_explicit(&device->active, memory_order_acquire))
		return EINVAL;
	result = -call(device, KB2_DEVICE_PORT_OP_CLOSE, NULL, 0, NULL, 0);
	if (!result)
		atomic_store_explicit(&device->active, false, memory_order_release);
	return result;
}

int kobox_posix_device_proxy_stop(struct kobox_posix_device_proxy *remote)
{
	size_t i;

	if (!remote)
		return EINVAL;
	for (i = 0; i < remote->object_count; i++)
		if (atomic_load_explicit(&remote->devices[i].active, memory_order_acquire))
			return EBUSY;
	if (remote->started) {
		atomic_store_explicit(&remote->stopping, true, memory_order_release);
		if (shutdown(remote->event, SHUT_RD))
			return errno;
		if (pthread_join(remote->notifier, NULL))
			abort();
		remote->started = false;
	}
	return 0;
}

const struct kobox_linux_pci_host *kobox_posix_device_proxy_pci(struct kobox_posix_proxy_device *device)
{
	return device ? &device->pci : NULL;
}

const struct kobox_linux_dma_host *kobox_posix_device_proxy_dma(struct kobox_posix_proxy_device *device)
{
	return device ? &device->dma : NULL;
}

const struct kobox_linux_irq_host *kobox_posix_device_proxy_irq(struct kobox_posix_proxy_device *device)
{
	return device ? &device->irq : NULL;
}

int kobox_posix_device_proxy_create(int request, int event, uint64_t generation,
	size_t objects, unsigned int cpus, struct kobox_posix_device_proxy **out)
{
	struct kobox_posix_device_proxy *remote;
	int result;

	if (!out)
		return EINVAL;
	*out = NULL;
	if (request < 0 || event < 0 || !generation || !objects || !cpus ||
	    objects > SIZE_MAX / sizeof(*remote->devices))
		return EINVAL;
	remote = calloc(1, sizeof(*remote));
	if (!remote)
		return ENOMEM;
	remote->devices = calloc(objects, sizeof(*remote->devices));
	if (!remote->devices) {
		free(remote);
		return ENOMEM;
	}
	result = pthread_mutex_init(&remote->lock, NULL);
	if (result) {
		free(remote->devices);
		free(remote);
		return result;
	}
	remote->request = request;
	remote->event = event;
	remote->session.generation = generation;
	remote->object_count = objects;
	remote->logical_cpus = cpus;
	*out = remote;
	return 0;
}

int kobox_posix_device_proxy_statistics(struct kobox_posix_device_proxy *remote,
	size_t *retired, size_t *delivered)
{
	if (!remote || remote->started || !retired || !delivered)
		return EINVAL;
	*retired = remote->retired;
	*delivered = remote->delivered;
	return 0;
}

int kobox_posix_device_proxy_destroy(struct kobox_posix_device_proxy **pointer)
{
	struct kobox_posix_device_proxy *remote;
	size_t i;
	int result;

	if (!pointer || !*pointer)
		return EINVAL;
	remote = *pointer;
	result = kobox_posix_device_proxy_stop(remote);
	if (result)
		return result;
	for (i = 0; i < remote->object_count; i++)
		if (remote->devices[i].leases)
			return EBUSY;
	if (pthread_mutex_destroy(&remote->lock))
		abort();
	free(remote->devices);
	free(remote);
	*pointer = NULL;
	return 0;
}
