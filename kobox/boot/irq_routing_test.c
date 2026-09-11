// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "boot_test.h"
#include "dma_fixture.h"
#include "irq_gate.h"
#include "pci_config_fixture.h"
#include "../task/posix_machine.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ROUTES 32
#define TABLE_OFFSET 0x2000
#define PBA_OFFSET 0x3000

/* Emulated interrupt-controller and PCI registers for the routing test.
 * The producer is a host device thread, never a Linux task or IRQ handler.
 */
struct route {
	struct kobox_linux_irq_route identity;
	unsigned int cpu;
	int live, masked, queued, servicing;
};

struct fixture {
	struct kobox_pci_config_fixture config;
	struct kobox_linux_pci_host config_ops;
	pthread_mutex_t lock;
	pthread_t producer;
	unsigned char *bar;
	struct route routes[ROUTES];
	uint64_t cookie;
	unsigned int pending[3], tables, emitted[3], allocated, released;
	unsigned int fail_after;
	unsigned int hold;
	uint64_t release_at;
	struct kobox_linux_irq_event captured[3], retired;
	unsigned int retired_cpu, retired_deliveries;
	struct kobox_dma_fixture *dma_fixture;
	const struct kobox_linux_dma_test *dma;
	uint64_t dma_iova;
	uint32_t dma_value;
	enum kobox_linux_irq_mode dma_mode;
	unsigned int dma_index, dma_pending, dma_completed;
	unsigned int paused;
	int stop;
};

static void lock(struct fixture *fixture)
{
	if (pthread_mutex_lock(&fixture->lock))
		abort();
}

static void unlock(struct fixture *fixture)
{
	if (pthread_mutex_unlock(&fixture->lock))
		abort();
}

static void notify(unsigned int cpus)
{
	unsigned int cpu;

	for (cpu = 0; cpu < 2; cpu++)
		if ((cpus & (1U << cpu)) &&
		    kobox_task_posix_operations.cpu_notify(cpu, KOBOX_LINUX_TASK_DEVICE_IRQ))
			abort();
}

static uint64_t clock_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value))
		abort();
	return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static int read_config(void *context, uint32_t offset, uint32_t width, uint32_t *value)
{
	struct fixture *fixture = context;
	int result;

	lock(fixture);
	result = fixture->config_ops.config_read(&fixture->config, offset, width, value);
	unlock(fixture);
	return result;
}

static int write_config(void *context, uint32_t offset, uint32_t width, uint32_t value)
{
	struct fixture *fixture = context;
	int result;

	lock(fixture);
	result = fixture->config_ops.config_write(&fixture->config, offset, width, value);
	unlock(fixture);
	return result;
}

static int map_memory(void *context, void *address, uint64_t physical,
		      size_t length, unsigned int protection, enum kobox_mmio_cache cache)
{
	struct fixture *fixture = context;

	return fixture->config_ops.memory_map(&fixture->config, address, physical,
					     length, protection, cache);
}

static int unmap_memory(void *context, void *address, size_t length)
{
	struct fixture *fixture = context;

	return fixture->config_ops.memory_unmap(&fixture->config, address, length);
}

static struct route *lookup(struct fixture *fixture, uint64_t hwirq, uint64_t cookie)
{
	struct route *route;

	if (hwirq >= ROUTES)
		return NULL;
	route = &fixture->routes[hwirq];
	return route->live && route->identity.cookie == cookie ? route : NULL;
}

static int allocate(void *context, enum kobox_linux_irq_mode mode, uint32_t index,
		    uint32_t count, uint32_t cpu, struct kobox_linux_irq_route *output)
{
	struct fixture *fixture = context;
	unsigned int first = mode * 8 + index, i;

	if (mode > KOBOX_IRQ_MSIX)
		return -EOPNOTSUPP;
	if (!count || count > 8 || index >= 8 || count > 8 - index ||
	    first + count > ROUTES || cpu >= 2)
		return -EINVAL;
	lock(fixture);
	if (fixture->fail_after && !--fixture->fail_after) {
		unlock(fixture);
		return -ENOSPC;
	}
	for (i = 0; i < count; i++) {
		if (fixture->routes[first + i].live) {
			unlock(fixture);
			return -EBUSY;
		}
	}
	if (fixture->cookie == UINT32_MAX) {
		unlock(fixture);
		return -ENOSPC;
	}
	fixture->cookie++;
	for (i = 0; i < count; i++) {
		struct route *route = &fixture->routes[first + i];

		*route = (struct route) {
			.identity = {
				.hwirq = first + i, .cookie = fixture->cookie,
				.address = (fixture->cookie << 32) | 0xfee00000,
				.data = first + i,
			},
			.live = 1, .masked = 1, .cpu = cpu,
		};
		output[i] = route->identity;
		fixture->allocated++;
	}
	unlock(fixture);
	return 0;
}

static int release(void *context, uint64_t hwirq, uint64_t cookie)
{
	struct fixture *fixture = context;
	struct route *route;
	int result = -ESTALE;

	lock(fixture);
	route = lookup(fixture, hwirq, cookie);
	if (route && !route->servicing) {
		memset(route, 0, sizeof(*route));
		fixture->released++;
		result = 0;
	}
	unlock(fixture);
	return result;
}

static int mask(void *context, uint64_t hwirq, uint64_t cookie, unsigned int masked)
{
	struct fixture *fixture = context;
	struct route *route;
	unsigned int cpus = 0;

	lock(fixture);
	route = lookup(fixture, hwirq, cookie);
	if (route) {
		route->masked = masked;
		if (!masked && route->queued)
			cpus = 1U << route->cpu;
	}
	unlock(fixture);
	notify(cpus);
	return route ? 0 : -ESTALE;
}

static int ack(void *context, uint64_t hwirq, uint64_t cookie)
{
	struct fixture *fixture = context;
	struct route *route;

	lock(fixture);
	route = lookup(fixture, hwirq, cookie);
	if (route)
		route->servicing = 0;
	unlock(fixture);
	return route ? 0 : -ESTALE;
}

static int quiesce(void *context, uint64_t hwirq, uint64_t cookie)
{
	struct fixture *fixture = context;
	struct route *route;
	int result;

	lock(fixture);
	route = lookup(fixture, hwirq, cookie);
	result = !route ? -ESTALE : !route->masked || route->servicing ? -EBUSY : 0;
	if (!result)
		route->queued = 0;
	unlock(fixture);
	return result;
}

static int affinity(void *context, uint64_t hwirq, uint64_t cookie, uint32_t cpu)
{
	struct fixture *fixture = context;
	struct route *route;
	unsigned int cpus = 0;

	if (cpu >= 2)
		return -EINVAL;
	lock(fixture);
	route = lookup(fixture, hwirq, cookie);
	if (route) {
		route->cpu = cpu;
		if (route->queued)
			cpus = 1U << cpu;
	}
	unlock(fixture);
	notify(cpus);
	return route ? 0 : -ESTALE;
}

static int active(void *context, uint64_t hwirq, uint64_t cookie, unsigned int *state)
{
	struct fixture *fixture = context;
	struct route *route;

	lock(fixture);
	route = lookup(fixture, hwirq, cookie);
	if (route)
		*state = route->servicing || (route->queued && !route->masked);
	unlock(fixture);
	return route ? 0 : -ESTALE;
}

static int retrigger(void *context, uint64_t hwirq, uint64_t cookie)
{
	struct fixture *fixture = context;
	struct route *route;
	unsigned int cpus = 0;

	lock(fixture);
	route = lookup(fixture, hwirq, cookie);
	if (route) {
		route->queued = 1;
		cpus = 1U << route->cpu;
	}
	unlock(fixture);
	notify(cpus);
	return route ? 0 : -ESTALE;
}

static int next(void *context, uint32_t cpu, struct kobox_linux_irq_event *event)
{
	struct fixture *fixture = context;
	unsigned int index;
	int result = -EAGAIN;

	lock(fixture);
	if (fixture->paused) {
		unlock(fixture);
		return result;
	}
	if (fixture->retired.cookie && fixture->retired_cpu == cpu) {
		*event = fixture->retired;
		fixture->retired.cookie = 0;
		fixture->retired_deliveries++;
		unlock(fixture);
		return 0;
	}
	for (index = 0; index < ROUTES; index++) {
		struct route *route = &fixture->routes[index];

		if (!route->live || route->masked || !route->queued || route->cpu != cpu)
			continue;
		route->queued = 0;
		route->servicing = 1;
		*event = (struct kobox_linux_irq_event) {
			.hwirq = route->identity.hwirq, .cookie = route->identity.cookie,
		};
		result = 0;
		break;
	}
	unlock(fixture);
	return result;
}

static int fire(void *context, enum kobox_linux_irq_mode mode, unsigned int index)
{
	struct fixture *fixture = context;

	if (mode > KOBOX_IRQ_MSIX || index >= (mode == KOBOX_IRQ_INTX ? 1 : 4))
		return -EINVAL;
	lock(fixture);
	fixture->pending[mode] |= 1U << index;
	unlock(fixture);
	return 0;
}

static int clear(void *context, enum kobox_linux_irq_mode mode, unsigned int index)
{
	struct fixture *fixture = context;

	if (mode > KOBOX_IRQ_MSIX || index >= 4)
		return -EINVAL;
	lock(fixture);
	fixture->pending[mode] &= ~(1U << index);
	if (fixture->dma_pending && fixture->dma_mode == mode && fixture->dma_index == index)
		fixture->dma_pending = 0;
	if (mode == KOBOX_IRQ_INTX)
		fixture->routes[0].queued = 0;
	unlock(fixture);
	return 0;
}

static uint32_t config_value(struct fixture *fixture, unsigned int offset, unsigned int width)
{
	uint32_t value = 0, i;

	for (i = 0; i < width; i++)
		value |= (uint32_t)fixture->config.bytes[offset + i] << (8 * i);
	return value;
}

static int snapshot(void *context, struct kobox_linux_irq_test_state *state)
{
	struct fixture *fixture = context;
	unsigned int index;

	lock(fixture);
	memset(state, 0, sizeof(*state));
	memcpy(state->emitted, fixture->emitted, sizeof(state->emitted));
	memcpy(state->pending, fixture->pending, sizeof(state->pending));
	state->masked[KOBOX_IRQ_INTX] = fixture->routes[0].masked;
	state->masked[KOBOX_IRQ_MSI] = config_value(fixture, 0x90, 4);
	state->pending[KOBOX_IRQ_MSI] = config_value(fixture, 0x94, 4);
	state->live = fixture->allocated - fixture->released;
	state->hold = fixture->hold;
	state->retired_deliveries = fixture->retired_deliveries;
	for (index = 0; index < ROUTES; index++)
		state->queued += fixture->routes[index].live && fixture->routes[index].queued;
	for (index = 0; index < 3; index++) {
		uint32_t *entry = (uint32_t *)(fixture->bar + TABLE_OFFSET + index * 16);

		if (__atomic_load_n(entry + 3, __ATOMIC_ACQUIRE) & PCI_MSIX_ENTRY_CTRL_MASKBIT)
			state->masked[KOBOX_IRQ_MSIX] |= 1U << index;
	}
	state->msix_pba = __atomic_load_n((uint64_t *)(fixture->bar + PBA_OFFSET), __ATOMIC_ACQUIRE);
	state->function_mask = !!(config_value(fixture, 0xa2, 2) & PCI_MSIX_FLAGS_MASKALL);
	unlock(fixture);
	return 0;
}

static void fail_allocate(void *context, unsigned int after)
{
	struct fixture *fixture = context;

	lock(fixture);
	fixture->fail_after = after;
	unlock(fixture);
}

static int checkpoint(void *context, enum kobox_linux_irq_test_checkpoint checkpoint)
{
	struct fixture *fixture = context;
	int result = 0;

	lock(fixture);
	switch (checkpoint) {
	case KOBOX_IRQ_HOLD_ENTER:
		if (fixture->hold)
			result = -EBUSY;
		else
			fixture->hold = 1;
		break;
	case KOBOX_IRQ_HOLD_WAIT:
		result = fixture->hold == 3;
		break;
	case KOBOX_IRQ_HOLD_RELEASE_AFTER_DELAY:
		if (fixture->hold != 1)
			result = -EINVAL;
		else {
			fixture->hold = 2;
			fixture->release_at = clock_ns() + 20000000;
		}
		break;
	case KOBOX_IRQ_HOLD_DONE:
		result = fixture->hold == 3 ? 1 : -EBUSY;
		fixture->hold = 0;
		break;
	case KOBOX_IRQ_HOLD_ABORT:
		fixture->hold = 3;
		break;
	default:
		result = -EINVAL;
	}
	unlock(fixture);
	return result;
}

static int capture(void *context, enum kobox_linux_irq_mode mode)
{
	struct fixture *fixture = context;
	struct route *route;
	int result = -ENOENT;

	if (mode > KOBOX_IRQ_MSIX)
		return -EINVAL;
	lock(fixture);
	route = &fixture->routes[mode * 8];
	if (route->live) {
		fixture->captured[mode] = (struct kobox_linux_irq_event) {
			.hwirq = route->identity.hwirq, .cookie = route->identity.cookie,
		};
		result = 0;
	}
	unlock(fixture);
	return result;
}

static int replay_retired(void *context, enum kobox_linux_irq_mode mode)
{
	struct fixture *fixture = context;
	struct route *route;
	unsigned int cpus = 0;
	int result = 0;

	if (mode > KOBOX_IRQ_MSIX)
		return -EINVAL;
	lock(fixture);
	if (fixture->captured[mode].cookie) {
		route = &fixture->routes[mode * 8];
		if (!route->live || fixture->retired.cookie ||
		    route->identity.cookie == fixture->captured[mode].cookie) {
			result = -EINVAL;
		} else {
			/* Adversarial delayed transport input, after the remapper's
			 * normal release drained its queue. Keep the OLD lease tag.
			 */
			fixture->retired = fixture->captured[mode];
			fixture->retired_cpu = route->cpu;
			cpus = 1U << route->cpu;
			result = 1;
		}
	}
	unlock(fixture);
	notify(cpus);
	return result;
}

static unsigned int emit(struct fixture *fixture, uint64_t address, uint32_t data)
{
	struct route *route = data < ROUTES ? &fixture->routes[data] : NULL;

	/* Decode the message Linux wrote, never a test IRQ number. */
	if (!route || !route->live || route->identity.address != address)
		abort();
	route->queued = 1;
	return 1U << route->cpu;
}

static int dma_fire(void *context, enum kobox_linux_irq_mode mode,
		    unsigned int index, uint64_t iova, uint32_t value)
{
	struct fixture *fixture = context;
	int result = 0;

	if (mode > KOBOX_IRQ_MSIX || index >= 3)
		return -EINVAL;
	lock(fixture);
	if (fixture->dma_pending) {
		result = -EBUSY;
	} else {
		fixture->dma_mode = mode;
		fixture->dma_index = index;
		fixture->dma_iova = iova;
		fixture->dma_value = value;
		fixture->dma_pending = 1;
	}
	unlock(fixture);
	return result;
}

static void pause_delivery(void *context, unsigned int paused)
{
	struct fixture *fixture = context;
	unsigned int index, cpus = 0;

	lock(fixture);
	fixture->paused = paused;
	if (!paused)
		for (index = 0; index < ROUTES; index++) {
			struct route *route = &fixture->routes[index];

			if (route->live && route->queued && !route->masked)
				cpus |= 1U << route->cpu;
		}
	unlock(fixture);
	notify(cpus);
}

static void *produce(void *argument)
{
	struct fixture *fixture = argument;
	const struct timespec delay = {.tv_nsec = 1000000};

	for (;;) {
		unsigned int index, flags, cpus = 0;

		lock(fixture);
		if (fixture->stop) {
			unlock(fixture);
			return NULL;
		}
		if (fixture->hold == 2 && clock_ns() >= fixture->release_at)
			fixture->hold = 3;
		if (fixture->dma_pending) {
			/* The existing isolated DMA engine writes registered Linux RAM.
			 * Only its completed transfer permits a device interrupt message.
			 */
			if (fixture->dma->transfer(fixture->dma->context, fixture->dma_iova,
					   &fixture->dma_value, sizeof(fixture->dma_value), 1))
				abort();
			fixture->pending[fixture->dma_mode] |= 1U << fixture->dma_index;
			fixture->dma_pending = 0;
			fixture->dma_completed++;
		}
		flags = config_value(fixture, 0xa2, 2);
		for (index = 0; index < 3; index++) {
			uint32_t *entry = (uint32_t *)(fixture->bar + TABLE_OFFSET + index * 16);
			uint64_t *pba = (uint64_t *)(fixture->bar + PBA_OFFSET);
			uint64_t address;
			uint32_t data;

			if (!(fixture->pending[KOBOX_IRQ_MSIX] & (1U << index)) ||
			    !(flags & PCI_MSIX_FLAGS_ENABLE))
				continue;
			if ((flags & PCI_MSIX_FLAGS_MASKALL) ||
			    (__atomic_load_n(entry + 3, __ATOMIC_ACQUIRE) & PCI_MSIX_ENTRY_CTRL_MASKBIT)) {
				__atomic_fetch_or(pba, 1ULL << index, __ATOMIC_RELEASE);
				continue;
			}
			address = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
			address |= (uint64_t)__atomic_load_n(entry + 1, __ATOMIC_ACQUIRE) << 32;
			data = __atomic_load_n(entry + 2, __ATOMIC_ACQUIRE);
			fixture->tables |= 1U << index;
			fixture->emitted[KOBOX_IRQ_MSIX]++;
			fixture->pending[KOBOX_IRQ_MSIX] &= ~(1U << index);
			__atomic_fetch_and(pba, ~(1ULL << index), __ATOMIC_RELEASE);
			cpus |= emit(fixture, address, data);
		}
		flags = config_value(fixture, 0x82, 2);
		if (flags & PCI_MSI_FLAGS_ENABLE) {
			unsigned int count = 1U << ((flags & PCI_MSI_FLAGS_QSIZE) >> 4);
			uint64_t address = config_value(fixture, 0x84, 4);
			uint32_t data = config_value(fixture, 0x8c, 2);
			uint32_t masked = config_value(fixture, 0x90, 4);

			address |= (uint64_t)config_value(fixture, 0x88, 4) << 32;
			for (index = 0; index < count; index++) {
				if (!(fixture->pending[KOBOX_IRQ_MSI] & (1U << index)) ||
				    (masked & (1U << index)))
					continue;
				cpus |= emit(fixture, address, data + index);
				fixture->pending[KOBOX_IRQ_MSI] &= ~(1U << index);
				fixture->emitted[KOBOX_IRQ_MSI]++;
			}
			for (index = 0; index < 4; index++)
				fixture->config.bytes[0x94 + index] =
					fixture->pending[KOBOX_IRQ_MSI] >> (8 * index);
		}
		if (fixture->pending[KOBOX_IRQ_INTX] &&
		    !(config_value(fixture, PCI_COMMAND, 2) & PCI_COMMAND_INTX_DISABLE)) {
			struct route *route = &fixture->routes[0];

			if (route->live && !route->masked && !route->queued && !route->servicing) {
				route->queued = 1;
				cpus |= 1U << route->cpu;
				fixture->emitted[KOBOX_IRQ_INTX]++;
			}
		}
		unlock(fixture);
		notify(cpus);
		nanosleep(&delay, NULL);
	}
}

static void close_fixture(void *context)
{
	struct fixture *fixture = context;

	lock(fixture);
	fixture->stop = 1;
	unlock(fixture);
	if (pthread_join(fixture->producer, NULL) || fixture->allocated != fixture->released ||
	    fixture->pending[0] || fixture->pending[1] || fixture->pending[2] ||
	    fixture->hold || fixture->retired.cookie || fixture->retired_deliveries != 3 ||
	    fixture->paused || fixture->dma_pending || fixture->dma_completed != 6 ||
	    fixture->tables != 7 || fixture->emitted[0] != 22 ||
	    fixture->emitted[1] != 34 || fixture->emitted[2] != 36 ||
	    fixture->config.map_calls != fixture->config.unmap_calls)
		abort();
	fprintf(stderr, "IRQ device: tables=%x emitted=%u/%u/%u allocated=%u released=%u\n",
		fixture->tables, fixture->emitted[0], fixture->emitted[1], fixture->emitted[2],
		fixture->allocated, fixture->released);
	munmap(fixture->bar, 0x5000);
	close(fixture->config.backing);
	pthread_mutex_destroy(&fixture->lock);
	kobox_dma_fixture_stop(fixture->dma_fixture);
}

static int prepare_dma(void *context, int descriptor, size_t size)
{
	struct fixture *fixture = context;

	return kobox_dma_fixture_prepare_host(fixture->dma_fixture, descriptor, size);
}

int main(int argc, char **argv)
{
	struct fixture fixture = {.lock = PTHREAD_MUTEX_INITIALIZER};
	struct kobox_linux_pci_host pci;
	struct kobox_linux_irq_test irq = {
		.host = {
			.size = sizeof(irq.host), .context = &fixture,
			.allocate = allocate, .release = release, .mask = mask,
			.ack = ack, .affinity = affinity, .retrigger = retrigger, .next = next,
			.active = active,
			.quiesce = quiesce,
		},
		.fire = fire, .clear = clear, .snapshot = snapshot, .fail_allocate = fail_allocate,
		.checkpoint = checkpoint,
		.capture = capture, .replay_retired = replay_retired,
		.dma_fire = dma_fire,
		.pause_delivery = pause_delivery,
	};
	struct kobox_boot_test_resources resources = {
		.pci = &pci, .irq = &irq, .context = &fixture, .close = close_fixture,
		.prepare_dma = prepare_dma,
	};

	if (argc != 2 || kobox_dma_fixture_start(argv[1], &fixture.dma_fixture))
		return 1;
	fixture.dma = kobox_dma_fixture_test(fixture.dma_fixture);
	irq.dma = fixture.dma;
	resources.dma = fixture.dma;
	kobox_pci_config_fixture_init(&fixture.config, &fixture.config_ops);
	/* Four-message, 64-bit MSI with per-vector masking, including writable
	 * message registers. Other PCI tests retain their original capability.
	 */
	fixture.config.bytes[0x82] = PCI_MSI_FLAGS_64BIT | (2U << 1);
	fixture.config.bytes[0x83] = PCI_MSI_FLAGS_MASKBIT >> 8;
	memset(fixture.config.writable + 0x84, 0xff, 10);
	memset(fixture.config.writable + 0x90, 0xff, 4);
	pci = fixture.config_ops;
	pci.context = &fixture;
	pci.config_read = read_config;
	pci.config_write = write_config;
	pci.memory_map = map_memory;
	pci.memory_unmap = unmap_memory;
	fixture.config.backing = memfd_create("MSI-X registers", MFD_CLOEXEC);
	if (fixture.config.backing < 0 || ftruncate(fixture.config.backing, 0x5000))
		return 1;
	fixture.bar = mmap(NULL, 0x5000, PROT_READ | PROT_WRITE, MAP_SHARED, fixture.config.backing, 0);
	if (fixture.bar == MAP_FAILED || pthread_create(&fixture.producer, NULL, produce, &fixture))
		return 1;
	return kobox_boot_test_run(argc, argv, &resources);
}
