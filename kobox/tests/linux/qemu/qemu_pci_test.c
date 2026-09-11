// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "qemu_pci.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define RAM_SIZE (256UL << 20)

int main(int argc, char **argv)
{
	struct kobox_qemu_pci *device = NULL;
	const struct kobox_linux_pci_host *host;
	uint64_t *ram, value;
	uint32_t config;
	void *lease;
	int backing, result;

	if (argc != 2)
		return 1;
	backing = memfd_create("QEMU shared Linux RAM conformance", MFD_CLOEXEC);
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
	result = kobox_qemu_pci_create(&device, argv[1], backing, RAM_SIZE, false);
	if (result)
		goto out;
	host = kobox_qemu_pci_host(device);
	result = -host->config_read(host->context, 0, 4, &config);
	if (!result && config != 0x10501af4)
		result = EPROTO;
	if (!result && host->config_read(host->context, 4095, 4, &config) != -EINVAL)
		result = EPROTO;
	/* Low offsets and the last RAM page share the exact same backing. */
	ram[0] = 0x123456789abcdef0ULL;
	ram[RAM_SIZE / 8 - 1] = 0xabcdef0123456789ULL;
	if (!result)
		result = kobox_qemu_pci_ram_read(device, 0, &value);
	if (!result && value != ram[0])
		result = EPROTO;
	if (!result)
		result = kobox_qemu_pci_ram_read(device, RAM_SIZE - 8, &value);
	if (!result && value != ram[RAM_SIZE / 8 - 1])
		result = EPROTO;
	if (!result)
		result = kobox_qemu_pci_ram_write(device, 4096, 0x56789abcdef01234ULL);
	if (!result && *(volatile uint64_t *)(ram + 512) != 0x56789abcdef01234ULL)
		result = EPROTO;
	if (!result && kobox_qemu_pci_ram_read(device, RAM_SIZE, &value) != EINVAL)
		result = EPROTO;
	lease = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (lease == MAP_FAILED) {
		result = errno;
		goto out;
	}
	if (!result)
		result = -host->memory_map(host->context, lease, host->windows[0].start,
					   4096, KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC_MINUS);
	if (!result) {
		if (kobox_qemu_pci_close(device) != EBUSY)
			result = EPROTO;
		if (host->memory_unmap(host->context, lease, 4096))
			result = EPROTO;
	}
	if (munmap(lease, 4096))
		result = errno;
out:
	if (device && kobox_qemu_pci_close(device))
		result = EPROTO;
	if (munmap(ram, RAM_SIZE) || close(backing))
		result = EPROTO;
	if (result)
		fprintf(stderr, "QEMU PCI/RAM backing: %s (%d)\n", strerror(result), result);
	else
		puts("QEMU PCI/shared RAM backing passed (DMA and driver Gate pending)");
	return !!result;
}
