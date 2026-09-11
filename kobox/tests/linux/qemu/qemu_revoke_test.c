// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "qemu_pci.h"
#include "qtest.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RAM_SIZE (256UL << 20)
#define EDU_BAR 0x20000000ULL
#define EDU_BUFFER 0x40000ULL

struct revoke_test {
	struct kobox_qemu_pci *device;
	atomic_bool notified, allow_notify, done;
	atomic_uint notifications;
	int result;
};

static atomic_bool fail_invalidation;
static struct revoke_test *current_test;

int __real_kobox_qtest_command(struct kobox_qtest *test, char *response, size_t capacity,
			       const char *format, ...);
int __wrap_kobox_qtest_command(struct kobox_qtest *test, char *response, size_t capacity,
			       const char *format, ...)
{
	char command[256];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(command, sizeof(command), format, arguments);
	va_end(arguments);
	if (length < 0 || (size_t)length >= sizeof(command))
		return EOVERFLOW;
	/* Only the failure case suppresses an actual invalidation command.
	 * No successful hardware result is synthesized by this test wrapper.
	 */
	if (!strncmp(command, "writeq 0xfed90028 ", 18) &&
	    atomic_exchange_explicit(&fail_invalidation, false, memory_order_acq_rel))
		return EIO;
	return __real_kobox_qtest_command(test, response, capacity, "%s", command);
}

static void require(bool condition, unsigned int line)
{
	if (!condition) {
		fprintf(stderr, "host revoke check failed at %u\n", line);
		if (current_test && current_test->device) {
			atomic_store_explicit(&current_test->allow_notify, true, memory_order_release);
			(void)kobox_qemu_pci_terminate(current_test->device);
		}
		_exit(1);
	}
}
#define REQUIRE(condition) require(!!(condition), __LINE__)

static void interval(void)
{
	struct timespec delay = {.tv_nsec = 1000000};

	while (nanosleep(&delay, &delay) && errno == EINTR)
		;
}

static int notify(void *context, unsigned int cpu)
{
	struct revoke_test *test = context;

	REQUIRE(cpu == 0);
	atomic_fetch_add_explicit(&test->notifications, 1, memory_order_relaxed);
	atomic_store_explicit(&test->notified, true, memory_order_release);
	while (!atomic_load_explicit(&test->allow_notify, memory_order_acquire))
		interval();
	return 0;
}

static void *revoke_worker(void *context)
{
	struct revoke_test *test = context;

	test->result = kobox_qemu_pci_revoke(test->device);
	atomic_store_explicit(&test->done, true, memory_order_release);
	return NULL;
}

static void msi_enable(const struct kobox_linux_pci_host *pci,
		       const struct kobox_linux_irq_route *route)
{
	uint32_t capability, identifier, flags;
	unsigned int visits;

	REQUIRE(!pci->config_read(pci->context, 0x34, 1, &capability));
	for (visits = 0; capability && visits < 48; visits++) {
		REQUIRE(!pci->config_read(pci->context, capability, 1, &identifier));
		if (identifier == 5)
			break;
		REQUIRE(!pci->config_read(pci->context, capability + 1, 1, &capability));
	}
	REQUIRE(capability && visits < 48);
	REQUIRE(!pci->config_read(pci->context, capability + 2, 2, &flags));
	REQUIRE(flags & 0x80);
	REQUIRE(!pci->config_write(pci->context, capability + 4, 4, route->address));
	REQUIRE(!pci->config_write(pci->context, capability + 8, 4, route->address >> 32));
	REQUIRE(!pci->config_write(pci->context, capability + 12, 2, route->data));
	REQUIRE(!pci->config_write(pci->context, capability + 2, 2, flags | 1));
}

static void submit(const struct kobox_linux_pci_host *pci, uint64_t iova, bool to_ram)
{
	REQUIRE(!pci->memory_write(pci->context, EDU_BAR + 0x80, 8, to_ram ? EDU_BUFFER : iova));
	REQUIRE(!pci->memory_write(pci->context, EDU_BAR + 0x88, 8, to_ram ? iova : EDU_BUFFER));
	REQUIRE(!pci->memory_write(pci->context, EDU_BAR + 0x90, 8, 8));
	REQUIRE(!pci->memory_write(pci->context, EDU_BAR + 0x98, 8, 1 | (to_ram << 1)));
}

static void revoked_operations(struct kobox_qemu_pci *device,
			       const struct kobox_linux_irq_route *route)
{
	const struct kobox_linux_pci_host *pci = kobox_qemu_pci_host(device);
	const struct kobox_linux_dma_host *dma = kobox_qemu_pci_dma(device);
	const struct kobox_linux_irq_host *irq = kobox_qemu_pci_irq(device);
	struct kobox_linux_irq_route rejected;
	struct kobox_linux_irq_event event;
	uint32_t config;
	uint64_t value;
	unsigned int active;

	REQUIRE(pci->config_read(pci->context, 0, 4, &config) == -ESTALE);
	REQUIRE(pci->config_write(pci->context, 4, 2, 6) == -ESTALE);
	REQUIRE(pci->memory_read(pci->context, EDU_BAR, 4, &value) == -ESTALE);
	REQUIRE(pci->memory_write(pci->context, EDU_BAR + 0x98, 8, 3) == -ESTALE);
	REQUIRE(dma->enable(dma->context, 1) == -ESTALE);
	REQUIRE(dma->map(dma->context, dma->aperture_start, 0, 4096, 3) == -ESTALE);
	REQUIRE(dma->unmap(dma->context, dma->aperture_start, 4096) == -ESTALE);
	REQUIRE(irq->allocate(irq->context, KOBOX_IRQ_MSI, 0, 1, 0, &rejected) == -ESTALE);
	REQUIRE(irq->next(irq->context, 0, &event) == -ESTALE);
	REQUIRE(irq->mask(irq->context, route->hwirq, route->cookie, 0) == -ESTALE);
	REQUIRE(irq->ack(irq->context, route->hwirq, route->cookie) == -ESTALE);
	REQUIRE(irq->active(irq->context, route->hwirq, route->cookie, &active) == -ESTALE);
	REQUIRE(irq->retrigger(irq->context, route->hwirq, route->cookie) == -ESTALE);
	REQUIRE(irq->quiesce(irq->context, route->hwirq, route->cookie) == -ESTALE);
	REQUIRE(irq->release(irq->context, route->hwirq, route->cookie) == -ESTALE);
}

static void run(const char *qemu, int backing, volatile uint64_t *ram, bool delayed,
		bool invalidation_failure)
{
	struct revoke_test test = {0};
	const struct kobox_linux_pci_host *pci;
	const struct kobox_linux_dma_host *dma;
	const struct kobox_linux_irq_host *irq;
	struct kobox_linux_irq_route route;
	pthread_t thread;
	uint64_t command, iova;
	uint32_t config;
	unsigned int attempt, active = 0, count;
	void *lease;

	current_test = &test;
	REQUIRE(!kobox_qemu_pci_create_dma_test(&test.device, qemu, backing, RAM_SIZE));
	pci = kobox_qemu_pci_host(test.device);
	dma = kobox_qemu_pci_dma(test.device);
	irq = kobox_qemu_pci_irq(test.device);
	iova = dma->aperture_start;
	REQUIRE(!pci->config_write(pci->context, 0x10, 4, EDU_BAR));
	REQUIRE(!pci->config_write(pci->context, 4, 2, 6));
	REQUIRE(!dma->enable(dma->context, 1));
	REQUIRE(!dma->map(dma->context, iova, 0, 4096, 3));
	lease = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	REQUIRE(lease != MAP_FAILED);
	REQUIRE(!pci->memory_map(pci->context, lease, EDU_BAR, 4096,
				KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC_MINUS));
	ram[0] = 0x123456789abcdef0ULL;
	submit(pci, iova, false);
	REQUIRE(!kobox_qemu_pci_clock_step(test.device, 100000000));
	REQUIRE(!pci->memory_read(pci->context, EDU_BAR + 0x98, 8, &command) && !(command & 1));
	REQUIRE(!kobox_qemu_pci_irq_delay(test.device, delayed ? 1000000000 : 0));
	REQUIRE(!kobox_qemu_pci_irq_start(test.device, notify, &test));
	REQUIRE(!irq->allocate(irq->context, KOBOX_IRQ_MSI, 0, 1, 0, &route));
	msi_enable(pci, &route);
	REQUIRE(!irq->mask(irq->context, route.hwirq, route.cookie, 0));
	REQUIRE(!pci->memory_write(pci->context, EDU_BAR + 0x60, 4, 1));
	for (attempt = 0; attempt < 1000; attempt++) {
		REQUIRE(!irq->active(irq->context, route.hwirq, route.cookie, &active));
		if (delayed ? active : atomic_load_explicit(&test.notified, memory_order_acquire))
			break;
		interval();
	}
	REQUIRE(attempt < 1000);
	if (delayed)
		REQUIRE(!atomic_load_explicit(&test.notified, memory_order_acquire));
	/* Queue an actual device timer while the MSI is delayed or notify is
	 * in flight. Revoke must stop this DMA without any Linux unmap call.
	 */
	ram[0] = 0xababababababababULL;
	submit(pci, iova, true);
	REQUIRE(!pci->memory_read(pci->context, EDU_BAR + 0x98, 8, &command) && (command & 1));
	atomic_store_explicit(&fail_invalidation, invalidation_failure, memory_order_release);
	REQUIRE(!pthread_create(&thread, NULL, revoke_worker, &test));
	for (attempt = 0; attempt < 1000; attempt++) {
		if (pci->config_read(pci->context, 0, 4, &config) == -ESTALE)
			break;
		interval();
	}
	REQUIRE(attempt < 1000);
	if (!delayed)
		REQUIRE(!atomic_load_explicit(&test.done, memory_order_acquire));
	atomic_store_explicit(&test.allow_notify, true, memory_order_release);
	REQUIRE(!pthread_join(thread, NULL));
	if (invalidation_failure) {
		REQUIRE(test.result == EIO);
		REQUIRE(!atomic_load_explicit(&fail_invalidation, memory_order_acquire));
		revoked_operations(test.device, &route);
		REQUIRE(kobox_qemu_pci_revoke(test.device) == EIO);
		REQUIRE(kobox_qemu_pci_close(test.device) == EBUSY);
		/* A failed invalidation leaves ownership quarantined. Only actual
		 * hardware process reaping supplies the missing DMA-stop proof.
		 */
		REQUIRE(!kobox_qemu_pci_terminate(test.device));
		REQUIRE(kobox_qemu_pci_close(test.device) == EBUSY);
		REQUIRE(!pci->memory_unmap(pci->context, lease, 4096));
		REQUIRE(!munmap(lease, 4096));
		REQUIRE(!kobox_qemu_pci_close(test.device));
		current_test = NULL;
		return;
	}
	REQUIRE(!test.result);
	REQUIRE(!kobox_qemu_pci_revoke(test.device));
	count = atomic_load_explicit(&test.notifications, memory_order_relaxed);
	REQUIRE(count == (delayed ? 0U : 1U));
	revoked_operations(test.device, &route);
	REQUIRE(kobox_qemu_pci_irq_start(test.device, notify, &test) == ESTALE);
	REQUIRE(!kobox_qemu_pci_clock_step(test.device, 2000000000));
	REQUIRE(ram[0] == 0xababababababababULL);
	REQUIRE(atomic_load_explicit(&test.notifications, memory_order_relaxed) == count);
	/* A successful DMA revoke does not erase CPU/MMIO lease accounting. */
	REQUIRE(kobox_qemu_pci_close(test.device) == EBUSY);
	REQUIRE(pci->memory_map(pci->context, lease, EDU_BAR, 4096,
			       KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC_MINUS) == -ESTALE);
	REQUIRE(!pci->memory_unmap(pci->context, lease, 4096));
	REQUIRE(!munmap(lease, 4096));
	REQUIRE(!kobox_qemu_pci_close(test.device));
	current_test = NULL;
}

int main(int argc, char **argv)
{
	volatile uint64_t *ram;
	int backing;

	if (argc != 2)
		return 1;
	backing = memfd_create("revoke hardware backing", MFD_CLOEXEC);
	REQUIRE(backing >= 0 && !ftruncate(backing, RAM_SIZE));
	ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, backing, 0);
	REQUIRE(ram != MAP_FAILED);
	run(argv[1], backing, ram, false, false);
	run(argv[1], backing, ram, true, false);
	run(argv[1], backing, ram, true, true);
	REQUIRE(!munmap((void *)ram, RAM_SIZE) && !close(backing));
	puts("Host VT-d revoke: queued DMA blocked, delayed/in-flight IRQ drained, old operations rejected, failed invalidation quarantined; GPU generation integration not certified");
	return 0;
}
