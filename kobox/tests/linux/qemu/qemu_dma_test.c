// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "qemu_pci.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RAM_SIZE (256UL << 20)
#define EDU_BAR 0x20000000ULL
#define EDU_BUFFER 0x40000ULL
#define REQUIRE(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "DMA hardware check failed at line %d: %s\n", __LINE__, #expression); \
		result = EPROTO; \
		goto out; \
	} \
} while (0)

static int notify(void *context, unsigned int cpu)
{
	atomic_uint *cpus = context;
	sigset_t signals;

	if (pthread_sigmask(SIG_BLOCK, NULL, &signals) ||
	    sigismember(&signals, SIGCHLD) != 1)
		return EINVAL;
	atomic_fetch_or_explicit(cpus, 1U << cpu, memory_order_release);
	return 0;
}

static int wait_irq(const struct kobox_linux_irq_host *irq, unsigned int cpu,
		    const struct kobox_linux_irq_route *route)
{
	const struct timespec interval = {.tv_nsec = 1000000};
	struct kobox_linux_irq_event event;
	unsigned int attempt;
	int result;

	for (attempt = 0; attempt < 1000; attempt++) {
		result = irq->next(irq->context, cpu, &event);
		if (!result)
			return event.hwirq == route->hwirq && event.cookie == route->cookie ? 0 : EPROTO;
		if (result != -EAGAIN)
			return result;
		nanosleep(&interval, NULL);
	}
	return ETIMEDOUT;
}

static int msi_transfer(struct kobox_qemu_pci *device)
{
	const struct kobox_linux_pci_host *pci = kobox_qemu_pci_host(device);
	const struct kobox_linux_irq_host *irq = kobox_qemu_pci_irq(device);
	struct kobox_linux_irq_route route = {0};
	struct kobox_linux_irq_event event;
	atomic_uint cpus = 0;
	uint32_t capability, identifier, flags = 0;
	unsigned int visits = 0;
	bool allocated = false;
	int result = 0;

	REQUIRE(!kobox_qemu_pci_irq_start(device, notify, &cpus));
	REQUIRE(!pci->config_read(pci->context, 0x34, 1, &capability));
	while (capability && visits++ < 48) {
		REQUIRE(!pci->config_read(pci->context, capability, 1, &identifier));
		if (identifier == 5)
			break;
		REQUIRE(!pci->config_read(pci->context, capability + 1, 1, &capability));
	}
	REQUIRE(capability && visits <= 48);
	REQUIRE(!irq->allocate(irq->context, KOBOX_IRQ_MSI, 0, 1, 0, &route));
	allocated = true;
	REQUIRE(!pci->config_read(pci->context, capability + 2, 2, &flags));
	REQUIRE(flags & 0x80);
	REQUIRE(!pci->config_write(pci->context, capability + 4, 4, route.address));
	REQUIRE(!pci->config_write(pci->context, capability + 8, 4, route.address >> 32));
	REQUIRE(!pci->config_write(pci->context, capability + 12, 2, route.data));
	REQUIRE(!pci->config_write(pci->context, capability + 2, 2, flags | 1));
	/* EDU sends an actual MSI transaction through its bus-master address
	 * space and VT-d. No test callback fabricates the arrival at the host.
	 */
	REQUIRE(!pci->memory_write(pci->context, EDU_BAR + 0x60, 4, 1));
	REQUIRE(irq->next(irq->context, 0, &event) == -EAGAIN);
	REQUIRE(!irq->affinity(irq->context, route.hwirq, route.cookie, 1));
	REQUIRE(!irq->mask(irq->context, route.hwirq, route.cookie, 0));
	REQUIRE(!wait_irq(irq, 1, &route));
	REQUIRE(irq->next(irq->context, 0, &event) == -EAGAIN);
	REQUIRE(!irq->ack(irq->context, route.hwirq, route.cookie));
	REQUIRE(!pci->memory_write(pci->context, EDU_BAR + 0x64, 4, 1));
out:
	if (allocated) {
		if (pci->config_write(pci->context, capability + 2, 2, flags))
			result = EPROTO;
		if (irq->mask(irq->context, route.hwirq, route.cookie, 1) ||
		    irq->ack(irq->context, route.hwirq, route.cookie) ||
		    irq->quiesce(irq->context, route.hwirq, route.cookie) ||
		    irq->release(irq->context, route.hwirq, route.cookie))
			result = EPROTO;
	}
	if (kobox_qemu_pci_irq_stop(device))
		result = EPROTO;
	if (!result && !(atomic_load_explicit(&cpus, memory_order_acquire) & 2))
		result = EPROTO;
	return result;
}

static int transfer(struct kobox_qemu_pci *device, uint64_t address, unsigned int to_ram)
{
	const struct kobox_linux_pci_host *pci = kobox_qemu_pci_host(device);
	uint64_t command;
	int result;

	result = pci->memory_write(pci->context, EDU_BAR + 0x80, 8,
				   to_ram ? EDU_BUFFER : address);
	if (!result)
		result = pci->memory_write(pci->context, EDU_BAR + 0x88, 8,
					   to_ram ? address : EDU_BUFFER);
	if (!result)
		result = pci->memory_write(pci->context, EDU_BAR + 0x90, 8, 8);
	if (!result)
		result = pci->memory_write(pci->context, EDU_BAR + 0x98, 8, 1 | (to_ram << 1));
	if (!result)
		result = kobox_qemu_pci_clock_step(device, 100000000);
	if (!result)
		result = pci->memory_read(pci->context, EDU_BAR + 0x98, 8, &command);
	if (!result && (command & 1))
		result = ETIMEDOUT;
	if (result)
		fprintf(stderr, "hardware DMA transfer: %d\n", result);
	return result;
}

int main(int argc, char **argv)
{
	struct kobox_qemu_pci *device = NULL;
	const struct kobox_linux_pci_host *pci;
	const struct kobox_linux_dma_host *dma = NULL;
	volatile uint64_t *ram;
	uint64_t first = 0, second = 0;
	int backing, result;

	if (argc != 2)
		return 1;
	backing = memfd_create("QEMU DMA backing", MFD_CLOEXEC);
	if (backing < 0)
		return 1;
	if (ftruncate(backing, RAM_SIZE)) {
		close(backing);
		return 1;
	}
	ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, backing, 0);
	if (ram == MAP_FAILED) {
		close(backing);
		return 1;
	}
	result = kobox_qemu_pci_create_dma_test(&device, argv[1], backing, RAM_SIZE);
	if (result)
		goto out;
	pci = kobox_qemu_pci_host(device);
	dma = kobox_qemu_pci_dma(device);
	first = dma->aperture_start;
	second = first + 4096;
	REQUIRE(!pci->config_write(pci->context, 0x10, 4, EDU_BAR));
	REQUIRE(!pci->config_write(pci->context, 4, 2, 6));
	REQUIRE(dma->map(dma->context, first, 0, 4096, 3) == -EIO);
	REQUIRE(!dma->enable(dma->context, 1));
	REQUIRE(dma->map(dma->context, first - 4096, 0, 4096, 3) == -EINVAL);
	REQUIRE(dma->map(dma->context, first, RAM_SIZE, 4096, 3) == -EINVAL);
	REQUIRE(!dma->map(dma->context, first, 0, 4096, KOBOX_DMA_DEVICE_READ));
	REQUIRE(!dma->map(dma->context, second, 4096, 4096, KOBOX_DMA_DEVICE_WRITE));
	REQUIRE(dma->map(dma->context, first, 8192, 4096, 3) == -EEXIST);
	ram[0] = 0x123456789abcdef0ULL;
	ram[512] = 0;
	REQUIRE(!transfer(device, first, 0));
	REQUIRE(!transfer(device, second, 1));
	REQUIRE(ram[512] == ram[0]);
	/* Device writes to a read-only IOVA must not reach its backing page. */
	ram[0] = 0xfedcba9876543210ULL;
	REQUIRE(!transfer(device, first, 1));
	REQUIRE(ram[0] == 0xfedcba9876543210ULL);
	REQUIRE(!dma->unmap(dma->context, second, 4096));
	ram[512] = 0xccccccccccccccccULL;
	REQUIRE(!transfer(device, second, 1));
	REQUIRE(ram[512] == 0xccccccccccccccccULL);
	/* Reusing the same IOVA must not hit the old cached translation. */
	REQUIRE(!dma->map(dma->context, second, 8192, 4096, KOBOX_DMA_DEVICE_WRITE));
	ram[1024] = 0;
	REQUIRE(!transfer(device, second, 1));
	REQUIRE(ram[1024] == 0x123456789abcdef0ULL);
	REQUIRE(ram[512] == 0xccccccccccccccccULL);
	REQUIRE(!dma->enable(dma->context, 0));
	ram[1024] = 0xddddddddddddddddULL;
	REQUIRE(!transfer(device, second, 1));
	REQUIRE(ram[1024] == 0xddddddddddddddddULL);
	REQUIRE(!dma->enable(dma->context, 1));
	REQUIRE(!transfer(device, second, 1));
	REQUIRE(ram[1024] == 0x123456789abcdef0ULL);
	REQUIRE(!dma->unmap(dma->context, first, 4096));
	REQUIRE(!dma->unmap(dma->context, second, 4096));
	REQUIRE(!msi_transfer(device));
out:
	if (dma) {
		int cleanup;

		cleanup = dma->unmap(dma->context, first, 4096);
		if (cleanup && cleanup != -ENOENT)
			result = EPROTO;
		cleanup = dma->unmap(dma->context, second, 4096);
		if (cleanup && cleanup != -ENOENT)
			result = EPROTO;
		if (dma->enable(dma->context, 0))
			result = EPROTO;
	}
	if (device && kobox_qemu_pci_close(device))
		result = EPROTO;
	if (munmap((void *)ram, RAM_SIZE) || close(backing))
		result = EPROTO;
	if (result)
		fprintf(stderr, "QEMU VT-d DMA: %s (%d)\n", strerror(result), result);
	else
		puts("QEMU VT-d DMA/permissions/revoke/remap and actual MSI routing passed; not the GPU Gate");
	return !!result;
}
