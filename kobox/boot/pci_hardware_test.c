// SPDX-License-Identifier: GPL-2.0-only

#include "boot_test.h"
#include "../tests/linux/qemu/qemu_pci.h"

#include <stdio.h>
#include <stdlib.h>

struct hardware {
	const char *executable;
	struct kobox_qemu_pci *device;
	struct kobox_linux_pci_host host;
};

static int prepare(void *context, int descriptor, size_t size)
{
	struct hardware *hardware = context;
	int result;

	result = kobox_qemu_pci_create(&hardware->device, hardware->executable, descriptor, size, false);
	if (!result)
		hardware->host = *kobox_qemu_pci_host(hardware->device);
	return result;
}

static void close_hardware(void *context)
{
	struct hardware *hardware = context;

	if (kobox_qemu_pci_close(hardware->device))
		abort();
	fputs("QEMU hardware backend closed\n", stderr);
}

int main(int argc, char **argv)
{
	struct hardware hardware = {0};
	struct kobox_boot_test_resources resources = {
		.pci = &hardware.host, .pci_verifier = "kobox_linux_pci_hardware_verify",
		.prepare_dma = prepare, .close = close_hardware, .context = &hardware,
	};

	if (argc != 3)
		return 1;
	hardware.executable = argv[2];
	return kobox_boot_test_run(2, argv, &resources);
}
