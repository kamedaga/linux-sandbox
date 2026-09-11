/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_TEST_H
#define KOBOX_BOOT_TEST_H

#include <stddef.h>

struct kobox_linux_resource_port;
struct kobox_linux_lifecycle;
struct kobox_linux_module_launch;
struct kobox_linux_pci_host;
struct kobox_linux_dma_test;
struct kobox_linux_irq_test;
struct kobox_linux_virtio_test;
struct kobox_posix_vm_service;
struct kobox_exec_file;

struct kobox_boot_test_resources {
	const struct kobox_linux_resource_port *port;
	const struct kobox_linux_lifecycle *lifecycle;
	const struct kobox_linux_module_launch *modules;
	const struct kobox_linux_pci_host *pci;
	/* Optional PCI test entry exported by the core; NULL selects conformance. */
	const char *pci_verifier;
	const struct kobox_linux_dma_test *dma;
	const struct kobox_linux_irq_test *irq;
	const struct kobox_linux_virtio_test *virtio;
	const struct kobox_exec_file *client_files;
	size_t client_file_count;
	int (*prepare_dma)(void *context, int ram_descriptor, size_t ram_size);
	void (*vm_ready)(void *context, struct kobox_posix_vm_service *service);
	void (*close)(void *context);
	void *context;
};

/* Test entry in a fresh process, before any hosted Linux execution. */
int kobox_boot_test_run(int argc, char **argv,
			const struct kobox_boot_test_resources *resources);

#endif
