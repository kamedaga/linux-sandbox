// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "qemu_pci.h"
#include "qtest.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define QEMU_RAM_BASE UINT64_C(0x100000000)
#define QEMU_ECAM_BASE UINT64_C(0xe0000000)
#define QEMU_BAR_BASE UINT64_C(0x20000000)
#define QEMU_BAR_SIZE UINT64_C(0x1000000)
#define QEMU_GPU_DEVFN 8U
#define QEMU_BOARD_SIZE (64UL << 20)

/* q35 VT-d legacy, 39-bit second-level translation. These are hardware
 * register/entry encodings, not Linux structure layouts. Board-private
 * tables never overlap the shared Linux RAM. Linux allocates the IOVAs.
 */
#define VTD_REG UINT64_C(0xfed90000)
#define VTD_ROOT 0x200000U
#define VTD_CONTEXT 0x201000U
#define VTD_LEVEL3 0x202000U
#define VTD_LEVEL2 0x203000U
#define VTD_LEAVES 0x204000U
#define VTD_IOVA_BASE UINT64_C(0x40000000)
#define VTD_IOVA_SIZE UINT64_C(0x40000000)
#define VTD_DMA_START (VTD_IOVA_BASE + 0x10000)
#define MSI_SLOTS 16U
#define MSI_BOARD_BASE 0x500000U

struct qemu_route {
	struct kobox_linux_irq_route identity;
	unsigned int cpu;
	bool live, masked, pending, servicing;
	bool delayed;
	uint64_t deliver_at;
};

struct kobox_qemu_pci {
	pthread_mutex_t lock;
	struct kobox_qtest *test;
	struct kobox_linux_pci_host host;
	struct kobox_linux_dma_host dma;
	struct kobox_linux_irq_host irq;
	struct qemu_route routes[MSI_SLOTS];
	uint32_t cookie;
	uint64_t irq_delay;
	pthread_t irq_thread;
	bool irq_started;
	bool clock_held;
	atomic_bool irq_stop;
	int (*notify)(void *context, unsigned int cpu);
	void *notify_context;
	unsigned char *board;
	int board_descriptor;
	uint64_t iotlb_register;
	size_t dma_pages;
	bool dma_enabled;
	bool failed;
	bool revoked, drained;
	size_t ram_size;
	size_t mappings;
};

static void lock(struct kobox_qemu_pci *device)
{
	if (pthread_mutex_lock(&device->lock))
		abort();
}

static void unlock(struct kobox_qemu_pci *device)
{
	if (pthread_mutex_unlock(&device->lock))
		abort();
}

static char width_suffix(unsigned int width)
{
	switch (width) {
	case 1: return 'b';
	case 2: return 'w';
	case 4: return 'l';
	case 8: return 'q';
	default: return 0;
	}
}

static int physical_read(struct kobox_qemu_pci *device, uint64_t address,
			 unsigned int width, uint64_t *value)
{
	char response[128], trailing;
	int result;

	result = kobox_qtest_command(device->test, response, sizeof(response),
				    "read%c 0x%" PRIx64, width_suffix(width), address);
	if (!result && sscanf(response, "OK 0x%" SCNx64 "%c", value, &trailing) != 1)
		return EPROTO;
	return result;
}

static int physical_write(struct kobox_qemu_pci *device, uint64_t address,
			  unsigned int width, uint64_t value)
{
	char response[128];

	return kobox_qtest_command(device->test, response, sizeof(response),
				   "write%c 0x%" PRIx64 " 0x%" PRIx64,
				   width_suffix(width), address, value);
}

static bool config_valid(uint32_t offset, uint32_t width)
{
	return (width == 1 || width == 2 || width == 4) &&
	       offset <= 4096 - width && !(offset & (width - 1));
}

static int config_read(void *context, uint32_t offset, uint32_t width,
		       uint32_t *value)
{
	struct kobox_qemu_pci *device = context;
	uint64_t read;
	int result;

	if (!value || !config_valid(offset, width))
		return -EINVAL;
	lock(device);
	result = device->revoked ? ESTALE :
		physical_read(device, QEMU_ECAM_BASE + (QEMU_GPU_DEVFN << 12) + offset,
			       width, &read);
	unlock(device);
	if (!result)
		*value = read;
	return -result;
}

static int config_write(void *context, uint32_t offset, uint32_t width, uint32_t value)
{
	struct kobox_qemu_pci *device = context;
	int result;

	if (!config_valid(offset, width))
		return -EINVAL;
	lock(device);
	result = device->revoked ? ESTALE :
		physical_write(device, QEMU_ECAM_BASE + (QEMU_GPU_DEVFN << 12) + offset,
				width, value);
	unlock(device);
	return -result;
}

static bool bar_range(struct kobox_qemu_pci *device, uint64_t physical, size_t length)
{
	uint64_t base = device->host.windows[0].start;

	return length && length <= QEMU_BAR_SIZE && physical >= base &&
	       physical - base <= QEMU_BAR_SIZE - length;
}

static int memory_map(void *context, void *address, uint64_t physical,
		      size_t length, unsigned int protection, enum kobox_mmio_cache cache)
{
	struct kobox_qemu_pci *device = context;
	int result = 0;

	if (!address || ((uintptr_t)address | physical | length) % 4096 ||
	    !bar_range(device, physical, length) ||
	    !(protection & KOBOX_LINUX_MEMORY_READ) ||
	    protection & ~(KOBOX_LINUX_MEMORY_READ | KOBOX_LINUX_MEMORY_WRITE))
		return -EINVAL;
	if (cache != KOBOX_MMIO_UC && cache != KOBOX_MMIO_UC_MINUS)
		return -EOPNOTSUPP;
	lock(device);
	/* A lease for address identity only: native loads must fault rather than
	 * reading a stale shared copy of side-effectful device registers.
	 */
	if (device->revoked)
		result = -ESTALE;
	else if (mmap(address, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		 -1, 0) == MAP_FAILED)
		result = -errno;
	else
		device->mappings += length / 4096;
	unlock(device);
	return result;
}

static int memory_unmap(void *context, void *address, size_t length)
{
	struct kobox_qemu_pci *device = context;
	int result = 0;

	if (!address || !length || ((uintptr_t)address | length) % 4096)
		return -EINVAL;
	lock(device);
	if (length / 4096 > device->mappings)
		result = -EINVAL;
	else if (mmap(address, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		      -1, 0) == MAP_FAILED)
		result = -errno;
	else
		device->mappings -= length / 4096;
	unlock(device);
	return result;
}

static int memory_read(void *context, uint64_t physical, unsigned int width, uint64_t *value)
{
	struct kobox_qemu_pci *device = context;
	int result;

	if (!value || !width_suffix(width) || !bar_range(device, physical, width))
		return -EINVAL;
	lock(device);
	result = device->revoked ? ESTALE : physical_read(device, physical, width, value);
	unlock(device);
	return -result;
}

static int memory_write(void *context, uint64_t physical, unsigned int width, uint64_t value)
{
	struct kobox_qemu_pci *device = context;
	int result;

	if (!width_suffix(width) || !bar_range(device, physical, width))
		return -EINVAL;
	lock(device);
	result = device->revoked ? ESTALE : physical_write(device, physical, width, value);
	unlock(device);
	return -result;
}

static void table_write(struct kobox_qemu_pci *device, size_t offset, uint64_t value)
{
	/* The pinned machine is little-endian x86. Naturally aligned atomic
	 * publication prevents a hardware walk seeing a torn table entry.
	 */
	atomic_store_explicit((_Atomic uint64_t *)(device->board + offset), value,
			      memory_order_release);
}

static uint64_t table_read(struct kobox_qemu_pci *device, size_t offset)
{
	return atomic_load_explicit((_Atomic uint64_t *)(device->board + offset),
				   memory_order_acquire);
}

static int register_wait(struct kobox_qemu_pci *device, uint64_t address,
			 unsigned int width, uint64_t mask, uint64_t expected)
{
	unsigned int attempt;
	uint64_t value;
	int result;

	for (attempt = 0; attempt < 64; attempt++) {
		result = physical_read(device, address, width, &value);
		if (result)
			return result;
		if ((value & mask) == expected)
			return 0;
	}
	return ETIMEDOUT;
}

static int invalidate(struct kobox_qemu_pci *device, bool context)
{
	int result = 0;

	if (context) {
		result = physical_write(device, VTD_REG + 0x28, 8, UINT64_C(0xa000000000000000));
		if (!result)
			result = register_wait(device, VTD_REG + 0x28, 8,
				UINT64_C(0x9800000000000000), UINT64_C(0x0800000000000000));
	}
	/* Global invalidation with read/write drain. Check the completed actual
	 * granularity, not merely the disappearance of the request bit.
	 */
	if (!result)
		result = physical_write(device, device->iotlb_register, 8, UINT64_C(0x9003000000000000));
	if (!result)
		result = register_wait(device, device->iotlb_register, 8,
			UINT64_C(0x8600000000000000), UINT64_C(0x0200000000000000));
	return result;
}

static int vtd_initialize(struct kobox_qemu_pci *device)
{
	uint64_t cap, ecap;
	unsigned int index;
	int result;

	result = physical_read(device, VTD_REG + 8, 8, &cap);
	if (!result)
		result = physical_read(device, VTD_REG + 16, 8, &ecap);
	if (result)
		return result;
	/* Require 39-bit walks and both drain capabilities. */
	if (!(cap & (1ULL << 9)) || (cap & (3ULL << 54)) != (3ULL << 54))
		return EOPNOTSUPP;
	device->iotlb_register = VTD_REG + ((ecap >> 8) & 0x3ff) * 16 + 8;
	table_write(device, VTD_ROOT, VTD_CONTEXT | 1);
	/* One PCI requester, domain 1, adjusted guest address width 39 bits.
	 * Keep its context non-present until Linux attaches the DMA domain.
	 */
	table_write(device, VTD_CONTEXT + QEMU_GPU_DEVFN * 16 + 8, 0x101);
	table_write(device, VTD_LEVEL3 + 8, VTD_LEVEL2 | 3);
	for (index = 0; index < 512; index++)
		table_write(device, VTD_LEVEL2 + index * 8, (VTD_LEAVES + index * 4096) | 3);
	result = physical_write(device, VTD_REG + 0x20, 8, VTD_ROOT);
	if (!result)
		result = physical_write(device, VTD_REG + 0x18, 4, 1U << 30);
	if (!result)
		result = register_wait(device, VTD_REG + 0x1c, 4, 1U << 30, 1U << 30);
	if (!result)
		result = invalidate(device, true);
	if (!result)
		result = physical_write(device, VTD_REG + 0x18, 4, 1U << 31);
	if (!result)
		result = register_wait(device, VTD_REG + 0x1c, 4, 1U << 31, 1U << 31);
	return result;
}

static int dma_enable(void *context, unsigned int enabled)
{
	struct kobox_qemu_pci *device = context;
	int result;

	if (enabled > 1)
		return -EINVAL;
	lock(device);
	if (device->revoked) {
		result = ESTALE;
		goto out;
	}
	if (device->failed) {
		result = EIO;
		goto out;
	}
	/* Never turn off translation: that would enable identity DMA. */
	table_write(device, VTD_CONTEXT + QEMU_GPU_DEVFN * 16,
		    enabled ? VTD_LEVEL3 | 1 : 0);
	result = invalidate(device, true);
	if (result)
		device->failed = true;
	else
		device->dma_enabled = enabled;
out:
	unlock(device);
	return -result;
}

static bool dma_range(uint64_t iova, size_t length)
{
	return length && !(iova % 4096) && !(length % 4096) &&
	       length <= VTD_IOVA_SIZE && iova >= VTD_DMA_START &&
	       iova - VTD_IOVA_BASE <= VTD_IOVA_SIZE - length;
}

static int dma_map(void *context, uint64_t iova, uint64_t offset,
		   size_t length, unsigned int protection)
{
	struct kobox_qemu_pci *device = context;
	size_t first, index, count;
	int result = 0;

	if (!dma_range(iova, length) || offset % 4096 || length > device->ram_size ||
	    offset > device->ram_size - length || !protection || (protection & ~3U))
		return -EINVAL;
	first = VTD_LEAVES + ((iova - VTD_IOVA_BASE) / 4096) * 8;
	count = length / 4096;
	lock(device);
	if (device->revoked) {
		result = ESTALE;
		goto out;
	}
	if (device->failed || !device->dma_enabled) {
		result = EIO;
		goto out;
	}
	for (index = 0; index < count; index++) {
		if (table_read(device, first + index * 8)) {
			result = EEXIST;
			goto out;
		}
	}
	for (index = 0; index < count; index++)
		table_write(device, first + index * 8,
			    (QEMU_RAM_BASE + offset + index * 4096) | protection);
	result = invalidate(device, false);
	if (result) {
		for (index = 0; index < count; index++)
			table_write(device, first + index * 8, 0);
		/* The backend is unusable if invalidation cannot be acknowledged.
		 * Stop its owned process before the caller may recycle any RAM.
		 */
		if (kobox_qtest_close(device->test))
			abort();
		device->test = NULL;
		device->failed = true;
	} else {
		device->dma_pages += count;
	}
out:
	unlock(device);
	return -result;
}

static int dma_unmap(void *context, uint64_t iova, size_t length)
{
	struct kobox_qemu_pci *device = context;
	size_t first, index, count;
	int result = 0;

	if (!dma_range(iova, length))
		return -EINVAL;
	first = VTD_LEAVES + ((iova - VTD_IOVA_BASE) / 4096) * 8;
	count = length / 4096;
	lock(device);
	if (device->revoked) {
		result = ESTALE;
		goto out;
	}
	if (device->failed) {
		result = EIO;
		goto out;
	}
	for (index = 0; index < count; index++) {
		if (!table_read(device, first + index * 8)) {
			result = ENOENT;
			goto out;
		}
	}
	for (index = 0; index < count; index++)
		table_write(device, first + index * 8, 0);
	result = invalidate(device, false);
	if (result)
		device->failed = true;
	else
		device->dma_pages -= count;
out:
	unlock(device);
	return -result;
}

static uint32_t doorbell_take(struct kobox_qemu_pci *device, unsigned int slot)
{
	return atomic_exchange_explicit((_Atomic uint32_t *)(device->board +
		MSI_BOARD_BASE + slot * 4096), 0, memory_order_acq_rel);
}

static struct qemu_route *route_lookup(struct kobox_qemu_pci *device,
				       uint64_t hwirq, uint64_t cookie)
{
	struct qemu_route *route;

	if (device->revoked || hwirq >= MSI_SLOTS)
		return NULL;
	route = &device->routes[hwirq];
	return route->live && route->identity.cookie == cookie ? route : NULL;
}

static void *irq_poll(void *argument)
{
	struct kobox_qemu_pci *device = argument;
	const struct timespec interval = {.tv_nsec = 100000};
	struct timespec now;
	uint64_t previous;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		abort();
	previous = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;

	while (!atomic_load_explicit(&device->irq_stop, memory_order_acquire)) {
		unsigned int slot, cpus = 0;
		uint64_t current;
		char response[128];

		lock(device);
		if (clock_gettime(CLOCK_MONOTONIC, &now))
			abort();
		current = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
		/* qtest never executes guest CPUs. Advance device timers from the
		 * host clock while Linux's own tick remains on its existing port.
		 */
		if (!device->failed && !device->revoked && !device->clock_held &&
		    kobox_qtest_command(device->test, response, sizeof(response),
							 "clock_step %" PRIu64, current - previous))
			abort();
		previous = current;
		for (slot = 0; slot < MSI_SLOTS; slot++) {
			struct qemu_route *route = &device->routes[slot];
			uint32_t value = doorbell_take(device, slot);

			if (!route->live)
				continue;
			/* A retired device message cannot enter a recycled route. */
			if (value && value == route->identity.data && !route->delayed) {
				route->delayed = true;
				route->deliver_at = current + device->irq_delay;
			}
			if (route->delayed && current >= route->deliver_at) {
				route->delayed = false;
				route->pending = true;
			}
			if (route->pending && !route->masked && !route->servicing)
				cpus |= 1U << route->cpu;
		}
		unlock(device);
		for (slot = 0; slot < 2; slot++)
			if ((cpus & (1U << slot)) && device->notify(device->notify_context, slot))
				abort();
		while (nanosleep(&interval, NULL) && errno == EINTR)
			;
	}
	return NULL;
}

static int irq_allocate(void *context, enum kobox_linux_irq_mode mode,
			uint32_t index, uint32_t count, uint32_t cpu,
			struct kobox_linux_irq_route *output)
{
	struct kobox_qemu_pci *device = context;
	unsigned int i;
	int result = 0;

	if (mode != KOBOX_IRQ_MSI && mode != KOBOX_IRQ_MSIX)
		return -EOPNOTSUPP;
	/* One MSI doorbell, or independent MSI-X addresses. Multiple MSI vectors
	 * sharing a single word could overwrite one another, so reject that form.
	 */
	if (mode == KOBOX_IRQ_MSI && (count != 1 || index))
		return -EOPNOTSUPP;
	if (!output || !count || index >= MSI_SLOTS || count > MSI_SLOTS - index || cpu >= 2)
		return -EINVAL;
	lock(device);
	if (device->revoked) {
		result = ESTALE;
		goto out;
	}
	if (device->failed || !device->dma_enabled || !device->irq_started) {
		result = EIO;
		goto out;
	}
	if (count > UINT32_MAX - device->cookie) {
		result = ENOSPC;
		goto out;
	}
	if (mode == KOBOX_IRQ_MSI && device->cookie >= UINT16_MAX) {
		result = ENOSPC;
		goto out;
	}
	for (i = 0; i < count; i++) {
		if (device->routes[index + i].live) {
			result = EBUSY;
			goto out;
		}
	}
	for (i = 0; i < count; i++) {
		unsigned int slot = index + i;

		(void)doorbell_take(device, slot);
		table_write(device, VTD_LEAVES + slot * 8, (MSI_BOARD_BASE + slot * 4096) | 2);
	}
	result = invalidate(device, false);
	if (result) {
		for (i = 0; i < count; i++)
			table_write(device, VTD_LEAVES + (index + i) * 8, 0);
		if (kobox_qtest_close(device->test))
			abort();
		device->test = NULL;
		device->failed = true;
		goto out;
	}
	for (i = 0; i < count; i++) {
		unsigned int slot = index + i;
		struct qemu_route *route = &device->routes[slot];

		device->cookie++;
		*route = (struct qemu_route) {
			.identity = {.hwirq = slot, .cookie = device->cookie,
				.address = VTD_IOVA_BASE + slot * 4096, .data = device->cookie},
			.cpu = cpu, .live = true, .masked = true,
		};
		output[i] = route->identity;
	}
out:
	unlock(device);
	return -result;
}

static int irq_release(void *context, uint64_t hwirq, uint64_t cookie)
{
	struct kobox_qemu_pci *device = context;
	struct qemu_route *route;
	int result;

	lock(device);
	route = route_lookup(device, hwirq, cookie);
	result = !route ? ESTALE : !route->masked || route->servicing ? EBUSY : 0;
	if (!result) {
		table_write(device, VTD_LEAVES + hwirq * 8, 0);
		result = invalidate(device, false);
		if (result) {
			device->failed = true;
		} else {
			(void)doorbell_take(device, hwirq);
			*route = (struct qemu_route) {0};
		}
	}
	unlock(device);
	return -result;
}

static int irq_mask(void *context, uint64_t hwirq, uint64_t cookie, unsigned int masked)
{
	struct kobox_qemu_pci *device = context;
	struct qemu_route *route;

	if (masked > 1)
		return -EINVAL;
	lock(device);
	route = route_lookup(device, hwirq, cookie);
	if (route)
		route->masked = masked;
	unlock(device);
	return route ? 0 : -ESTALE;
}

static int irq_ack(void *context, uint64_t hwirq, uint64_t cookie)
{
	struct kobox_qemu_pci *device = context;
	struct qemu_route *route;

	lock(device);
	route = route_lookup(device, hwirq, cookie);
	if (route)
		route->servicing = false;
	unlock(device);
	return route ? 0 : -ESTALE;
}

static int irq_quiesce(void *context, uint64_t hwirq, uint64_t cookie)
{
	struct kobox_qemu_pci *device = context;
	struct qemu_route *route;
	int result;

	lock(device);
	route = route_lookup(device, hwirq, cookie);
	result = !route ? ESTALE : !route->masked || route->servicing ? EBUSY : 0;
	if (!result) {
		(void)doorbell_take(device, hwirq);
		route->pending = false;
		route->delayed = false;
	}
	unlock(device);
	return -result;
}

static int irq_affinity(void *context, uint64_t hwirq, uint64_t cookie, uint32_t cpu)
{
	struct kobox_qemu_pci *device = context;
	struct qemu_route *route;

	if (cpu >= 2)
		return -EINVAL;
	lock(device);
	route = route_lookup(device, hwirq, cookie);
	if (route)
		route->cpu = cpu;
	unlock(device);
	return route ? 0 : -ESTALE;
}

static int irq_active(void *context, uint64_t hwirq, uint64_t cookie, unsigned int *active)
{
	struct kobox_qemu_pci *device = context;
	struct qemu_route *route;

	if (!active)
		return -EINVAL;
	lock(device);
	route = route_lookup(device, hwirq, cookie);
	if (route)
		*active = route->servicing ||
			 ((route->pending || route->delayed) && !route->masked);
	unlock(device);
	return route ? 0 : -ESTALE;
}

static int irq_retrigger(void *context, uint64_t hwirq, uint64_t cookie)
{
	struct kobox_qemu_pci *device = context;
	struct qemu_route *route;

	lock(device);
	route = route_lookup(device, hwirq, cookie);
	if (route)
		route->pending = true;
	unlock(device);
	return route ? 0 : -ESTALE;
}

static int irq_next(void *context, uint32_t cpu, struct kobox_linux_irq_event *event)
{
	struct kobox_qemu_pci *device = context;
	unsigned int i;
	int result = -EAGAIN;

	if (!event || cpu >= 2)
		return -EINVAL;
	lock(device);
	if (device->revoked) {
		unlock(device);
		return -ESTALE;
	}
	for (i = 0; i < MSI_SLOTS; i++) {
		struct qemu_route *route = &device->routes[i];

		if (!route->live || route->masked || !route->pending || route->servicing || route->cpu != cpu)
			continue;
		route->pending = false;
		route->servicing = true;
		*event = (struct kobox_linux_irq_event) {
			.hwirq = route->identity.hwirq, .cookie = route->identity.cookie,
		};
		result = 0;
		break;
	}
	unlock(device);
	return result;
}

static int create(struct kobox_qemu_pci **out, const char *executable,
		  int ram_descriptor, size_t ram_size, bool dma_test, bool virgl,
		  uint32_t segment, uint64_t bar_base)
{
	struct kobox_qemu_pci *device;
	char backend[192], board_backend[192], response[128];
	const char *arguments[] = {
		"-machine", "q35,memory-backend=boardram",
		"-m", "64M,slots=1,maxmem=1088M", "-object", board_backend,
		"-object", backend,
		"-device", "pc-dimm,memdev=linuxram,addr=0x100000000",
		"-device", dma_test ? "edu,addr=1.0,dma_mask=0xffffffffffffffff" :
			virgl ? "virtio-gpu-gl-pci,addr=1.0,disable-legacy=on,iommu_platform=on" :
			"virtio-gpu-pci,addr=1.0,disable-legacy=on,iommu_platform=on",
		"-device", "intel-iommu,caching-mode=on,intremap=off,dma-drain=on",
		"-display", virgl ? "sdl,gl=on" : "none",
		"-trace", "enable=virtio_gpu_*",
	};
	struct stat status;
	uint32_t identifier;
	int result;

	if (!out || !executable || ram_descriptor < 0 || !ram_size ||
	    ram_size > (1UL << 30) || ram_size % (2UL << 20))
		return EINVAL;
	*out = NULL;
	if (fstat(ram_descriptor, &status))
		return errno;
	if (!S_ISREG(status.st_mode) || status.st_size < 0 || (uint64_t)status.st_size < ram_size)
		return EINVAL;
	device = calloc(1, sizeof(*device));
	if (!device)
		return ENOMEM;
	result = pthread_mutex_init(&device->lock, NULL);
	if (result) {
		free(device);
		return result;
	}
	device->ram_size = ram_size;
	device->board_descriptor = memfd_create("QEMU board-private tables", MFD_CLOEXEC);
	if (device->board_descriptor < 0) {
		result = errno;
		goto release;
	}
	if (ftruncate(device->board_descriptor, QEMU_BOARD_SIZE)) {
		result = errno;
		goto release;
	}
	device->board = mmap(NULL, QEMU_BOARD_SIZE, PROT_READ | PROT_WRITE,
			     MAP_SHARED, device->board_descriptor, 0);
	if (device->board == MAP_FAILED) {
		device->board = NULL;
		result = errno;
		goto release;
	}
	snprintf(board_backend, sizeof(board_backend),
		 "memory-backend-file,id=boardram,mem-path=/proc/%ld/fd/%d,size=%lu,share=on",
		 (long)getpid(), device->board_descriptor, QEMU_BOARD_SIZE);
	snprintf(backend, sizeof(backend),
		 "memory-backend-file,id=linuxram,mem-path=/proc/%ld/fd/%d,size=%zu,share=on",
		 (long)getpid(), ram_descriptor, ram_size);
	result = kobox_qtest_start(&device->test, executable, arguments,
				  sizeof(arguments) / sizeof(arguments[0]) - (virgl ? 0 : 2));
	/* Board bootstrap enables q35's ECAM. Linux config accesses thereafter
	 * remain actual device transactions, including the extended 4K space.
	 */
	if (!result)
		result = kobox_qtest_command(device->test, response, sizeof(response),
					    "outl 0xcf8 0x80000060");
	if (!result)
		result = kobox_qtest_command(device->test, response, sizeof(response),
					    "outl 0xcfc 0xe0000001");
	if (!result)
		result = -config_read(device, 0, 4, &identifier);
	if (!result && identifier != (dma_test ? 0x11e81234U : 0x10501af4U))
		result = ENODEV;
	if (!result)
		result = vtd_initialize(device);
release:
	if (result) {
		int cleanup = kobox_qemu_pci_close(device);

		return cleanup ? cleanup : result;
	}
	device->host = (struct kobox_linux_pci_host) {
		.size = sizeof(device->host), .context = device,
		.segment = segment, .bus = 0, .devfn = QEMU_GPU_DEVFN,
		.window_count = 1,
		.windows = {{.start = bar_base, .length = QEMU_BAR_SIZE}},
		.config_read = config_read, .config_write = config_write,
		.memory_map = memory_map, .memory_unmap = memory_unmap,
		.memory_read = memory_read, .memory_write = memory_write,
	};
	device->dma = (struct kobox_linux_dma_host) {
		.size = sizeof(device->dma), .context = device, .coherent = 1,
		.aperture_start = VTD_DMA_START,
		.aperture_end = VTD_IOVA_BASE + VTD_IOVA_SIZE - 1,
		.enable = dma_enable, .map = dma_map, .unmap = dma_unmap,
	};
	device->irq = (struct kobox_linux_irq_host) {
		.size = sizeof(device->irq), .context = device,
		.allocate = irq_allocate, .release = irq_release,
		.mask = irq_mask, .ack = irq_ack, .quiesce = irq_quiesce,
		.affinity = irq_affinity, .active = irq_active,
		.retrigger = irq_retrigger, .next = irq_next,
	};
	*out = device;
	return 0;
}

int kobox_qemu_pci_create(struct kobox_qemu_pci **out, const char *executable,
			  int ram_descriptor, size_t ram_size, bool virgl)
{
	return create(out, executable, ram_descriptor, ram_size, false, virgl, 1, QEMU_BAR_BASE);
}

int kobox_qemu_pci_create_dma_test(struct kobox_qemu_pci **out, const char *executable,
				 int ram_descriptor, size_t ram_size)
{
	return create(out, executable, ram_descriptor, ram_size, true, false, 1, QEMU_BAR_BASE);
}

int kobox_qemu_pci_create_dma_consumer(struct kobox_qemu_pci **out, const char *executable,
				      int ram_descriptor, size_t ram_size)
{
	return create(out, executable, ram_descriptor, ram_size, true, false, 2, 0x30000000);
}

const struct kobox_linux_dma_host *kobox_qemu_pci_dma(struct kobox_qemu_pci *device)
{
	return device ? &device->dma : NULL;
}

const struct kobox_linux_irq_host *kobox_qemu_pci_irq(struct kobox_qemu_pci *device)
{
	return device ? &device->irq : NULL;
}

int kobox_qemu_pci_irq_start(struct kobox_qemu_pci *device,
			     int (*notify)(void *context, unsigned int cpu), void *context)
{
	sigset_t children, previous;
	int result;

	if (!device || !notify)
		return EINVAL;
	lock(device);
	if (device->revoked) {
		unlock(device);
		return ESTALE;
	}
	if (device->irq_started) {
		unlock(device);
		return EBUSY;
	}
	device->notify = notify;
	device->notify_context = context;
	atomic_store_explicit(&device->irq_stop, false, memory_order_release);
	/* The VM service consumes SIGCHLD through signalfd. A background
	 * hardware producer must not steal that process-directed notification,
	 * including when it starts before the VM service is constructed.
	 */
	sigemptyset(&children);
	sigaddset(&children, SIGCHLD);
	result = pthread_sigmask(SIG_BLOCK, &children, &previous);
	if (!result) {
		result = pthread_create(&device->irq_thread, NULL, irq_poll, device);
		if (pthread_sigmask(SIG_SETMASK, &previous, NULL))
			abort();
	}
	if (!result)
		device->irq_started = true;
	unlock(device);
	return result;
}

int kobox_qemu_pci_irq_delay(struct kobox_qemu_pci *device, uint64_t nanoseconds)
{
	if (!device || nanoseconds > UINT64_C(1000000000))
		return EINVAL;
	lock(device);
	if (device->revoked) {
		unlock(device);
		return ESTALE;
	}
	device->irq_delay = nanoseconds;
	unlock(device);
	return 0;
}

int kobox_qemu_pci_irq_stop(struct kobox_qemu_pci *device)
{
	unsigned int i;

	if (!device)
		return EINVAL;
	lock(device);
	for (i = 0; i < MSI_SLOTS; i++) {
		if (device->routes[i].live) {
			unlock(device);
			return EBUSY;
		}
	}
	if (!device->irq_started) {
		unlock(device);
		return 0;
	}
	atomic_store_explicit(&device->irq_stop, true, memory_order_release);
	unlock(device);
	if (pthread_join(device->irq_thread, NULL))
		abort();
	lock(device);
	device->irq_started = false;
	device->notify = NULL;
	device->notify_context = NULL;
	unlock(device);
	return 0;
}

int kobox_qemu_pci_clock_step(struct kobox_qemu_pci *device, uint64_t nanoseconds)
{
	char response[128];
	int result;

	if (!device || nanoseconds > INT64_MAX)
		return EINVAL;
	lock(device);
	result = kobox_qtest_command(device->test, response, sizeof(response),
				    "clock_step %" PRIu64, nanoseconds);
	unlock(device);
	return result;
}

int kobox_qemu_pci_clock_hold(struct kobox_qemu_pci *device)
{
	int result;

	if (!device)
		return EINVAL;
	lock(device);
	result = device->revoked ? ESTALE : 0;
	if (!result)
		device->clock_held = true;
	unlock(device);
	return result;
}

int kobox_qemu_pci_dma_checkpoint(struct kobox_qemu_pci *device,
				 uint64_t *ram_offset, uint64_t *before)
{
	uint64_t base, command, source, target, count, leaf, physical, payload;
	int result = EINVAL;

	if (!device || !ram_offset || !before)
		return EINVAL;
	lock(device);
	if (device->revoked || !device->clock_held || !device->dma_enabled)
		goto out;
	base = device->host.windows[0].start;
	result = physical_read(device, base + 0x98, 8, &command);
	if (!result)
		result = physical_read(device, base + 0x80, 8, &source);
	if (!result)
		result = physical_read(device, base + 0x88, 8, &target);
	if (!result)
		result = physical_read(device, base + 0x90, 8, &count);
	if (result)
		goto out;
	result = EPROTO;
	if (command != 3 || source != 0x40000 || count != 64 || target % 4096 != 128 ||
	    !dma_range(target - 128, 4096))
		goto out;
	leaf = table_read(device, VTD_LEAVES + ((target - VTD_IOVA_BASE) / 4096) * 8);
	physical = (leaf & ~UINT64_C(4095)) + 128;
	if ((leaf & 3) != 3 || physical < QEMU_RAM_BASE ||
	    physical - QEMU_RAM_BASE > device->ram_size - 64)
		goto out;
	result = physical_read(device, physical, 8, before);
	if (!result)
		result = physical_read(device, physical - 128, 8, &payload);
	/* Successful earlier RAM-to-EDU DMA loaded this distinct source.
	 * An unchanged target after drain cannot be a same-content false pass.
	 */
	if (!result && *before == payload)
		result = EALREADY;
	if (!result)
		*ram_offset = physical - QEMU_RAM_BASE;
out:
	unlock(device);
	return result;
}

int kobox_qemu_pci_revoke(struct kobox_qemu_pci *device)
{
	uint64_t command;
	size_t index;
	bool join;
	int result;

	if (!device)
		return EINVAL;
	lock(device);
	if (device->drained) {
		unlock(device);
		return 0;
	}
	device->revoked = true;
	join = device->irq_started;
	atomic_store_explicit(&device->irq_stop, true, memory_order_release);
	for (index = 0; index < MSI_SLOTS; index++) {
		device->routes[index].masked = true;
		device->routes[index].pending = false;
		device->routes[index].delayed = false;
	}
	result = device->failed || !device->test ? EIO :
		physical_read(device, QEMU_ECAM_BASE + (QEMU_GPU_DEVFN << 12) + 4, 2, &command);
	if (!result)
		result = physical_write(device, QEMU_ECAM_BASE + (QEMU_GPU_DEVFN << 12) + 4,
					2, (command & ~4U) | 0x400U);
	/* Translation stays enabled. Remove the requester context and wait
	 * for context/IOTLB invalidation with read/write drain before releasing
	 * any DMA accounting or backing ownership.
	 */
	if (!result) {
		table_write(device, VTD_CONTEXT + QEMU_GPU_DEVFN * 16, 0);
		result = invalidate(device, true);
	}
	if (!result) {
		for (index = 0; index < VTD_IOVA_SIZE / 4096; index++)
			table_write(device, VTD_LEAVES + index * 8, 0);
		result = invalidate(device, false);
	}
	if (!result) {
		device->dma_enabled = false;
		device->dma_pages = 0;
		for (index = 0; index < MSI_SLOTS; index++) {
			(void)doorbell_take(device, index);
			device->routes[index] = (struct qemu_route) {0};
		}
	} else {
		device->failed = true;
	}
	unlock(device);
	/* A producer may have left the lock with notify in flight. Success
	 * cannot be published until that final host callback has returned.
	 */
	if (join && pthread_join(device->irq_thread, NULL))
		abort();
	lock(device);
	device->irq_started = false;
	device->notify = NULL;
	device->notify_context = NULL;
	device->drained = !result;
	unlock(device);
	return result;
}

int kobox_qemu_pci_terminate(struct kobox_qemu_pci *device)
{
	unsigned int index;
	int result = 0;

	if (!device)
		return EINVAL;
	/* Revoke rejects new calls and joins notification delivery even when
	 * hardware invalidation fails. Never recycle accounting on that error
	 * alone: the independent process-exit proof below is required.
	 */
	(void)kobox_qemu_pci_revoke(device);
	lock(device);
	if (device->test)
		result = kobox_qtest_close(device->test);
	if (!result) {
		device->test = NULL;
		device->dma_enabled = false;
		device->dma_pages = 0;
		for (index = 0; index < MSI_SLOTS; index++)
			device->routes[index] = (struct qemu_route) {0};
		device->drained = true;
	}
	unlock(device);
	return result;
}

const struct kobox_linux_pci_host *kobox_qemu_pci_host(struct kobox_qemu_pci *device)
{
	return device ? &device->host : NULL;
}

int kobox_qemu_pci_ram_read(struct kobox_qemu_pci *device, uint64_t offset, uint64_t *value)
{
	int result;

	if (!device || !value || offset > device->ram_size - 8 || offset % 8)
		return EINVAL;
	lock(device);
	result = physical_read(device, QEMU_RAM_BASE + offset, 8, value);
	unlock(device);
	return result;
}

int kobox_qemu_pci_ram_write(struct kobox_qemu_pci *device, uint64_t offset, uint64_t value)
{
	int result;

	if (!device || offset > device->ram_size - 8 || offset % 8)
		return EINVAL;
	lock(device);
	result = physical_write(device, QEMU_RAM_BASE + offset, 8, value);
	unlock(device);
	return result;
}

int kobox_qemu_pci_close(struct kobox_qemu_pci *device)
{
	unsigned int i;
	int result;

	if (!device)
		return EINVAL;
	if (device->mappings || device->dma_pages || device->dma_enabled)
		return EBUSY;
	for (i = 0; i < MSI_SLOTS; i++)
		if (device->routes[i].live)
			return EBUSY;
	if (device->irq_started) {
		if (kobox_qemu_pci_irq_stop(device))
			abort();
	}
	if (device->test) {
		result = kobox_qtest_close(device->test);
		if (result)
			return result;
	}
	if (pthread_mutex_destroy(&device->lock))
		abort();
	if (device->board && munmap(device->board, QEMU_BOARD_SIZE))
		abort();
	if (device->board_descriptor >= 0 && close(device->board_descriptor))
		abort();
	free(device);
	return 0;
}
