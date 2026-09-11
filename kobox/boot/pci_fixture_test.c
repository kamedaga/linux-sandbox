// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "pci_fixture.h"
#include "pci_config_fixture.h"
#include "../provider/device_resource_interfaces.h"

#include <kobox2/pci_function_layout.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "PCI fixture check failed at %u\n", __LINE__); \
		return 1; \
	} \
} while (0)

static int denied(void *pointer, int write)
{
	pid_t child = fork();
	int status;

	if (!child) {
		volatile unsigned int *address = pointer;
		struct sigaction action = {.sa_handler = SIG_DFL};
		unsigned int value;

		sigemptyset(&action.sa_mask);
		if (prctl(PR_SET_DUMPABLE, 0) || sigaction(SIGSEGV, &action, NULL))
			_exit(125);
		value = *address;
		if (write)
			*address = value;
		_exit(0);
	}
	if (child < 0 || waitpid(child, &status, 0) != child)
		return 0;
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

static int legacy_fixture(void)
{
	struct kobox_pci_fixture_owner owner = {.generation = 17, .object_id = 101};
	kb2_resource_grant_slot_t slot = {
		.slot_id = 1, .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
		.interface_schema_digest = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES,
	};
	kb2_resource_grant_object_t object = {
		.slot_id = 1, .object_id = 101, .granted_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
	};
	struct kobox_resource_native_handle handle = {.role = KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE};
	const struct kobox_resource_interface_operations *base;
	const struct kobox_pci_function_resource_operations *pci;
	struct stat info;
	void *native = NULL, *bar = NULL;
	uint32_t value;

	handle.handle = memfd_create("pci-fixture-unit", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	CHECK(handle.handle >= 0 && !ftruncate(handle.handle, KOBOX_PCI_FIXTURE_SIZE));
	CHECK(!fstat(handle.handle, &info));
	owner.device = info.st_dev;
	owner.inode = info.st_ino;
	CHECK(kobox_pci_fixture_import(&owner, &slot, &object, &handle, 1, &native, &base) == -EPERM);
	CHECK(!native && !base);
	CHECK(!fcntl(handle.handle, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_GROW | F_SEAL_SHRINK));
	owner.inode++;
	CHECK(kobox_pci_fixture_import(&owner, &slot, &object, &handle, 1, &native, &base) == -EPERM);
	owner.inode--;
	CHECK(!kobox_pci_fixture_import(&owner, &slot, &object, &handle, 1, &native, &base));
	CHECK(owner.imported == 1 && !owner.released);
	pci = (const void *)base;
	CHECK(pci->config_read(native, 4094, 4, &value) == -EINVAL);
	CHECK(pci->config_write(native, 0, 4, 0) == -EACCES);
	CHECK(!pci->config_write(native, 0x40, 4, 0x12345678));
	CHECK(!pci->config_read(native, 0x40, 4, &value) && value == 0x12345678);
	CHECK(pci->bar_read(native, 0, 0, 4, &value) == -EACCES);
	CHECK(!pci->bar_map(native, 0, 0, 4096, KB2_PCI_FUNCTION_MAP_PROTECTION_READ,
		KB2_PCI_FUNCTION_CACHE_UC_MINUS, NULL, &bar));
	CHECK(denied(bar, 1));
	CHECK(pci->bar_write(native, 0, 0, 4, 7) == -EACCES);
	CHECK(!pci->bar_unmap(native, bar, 4096));
	CHECK(denied(bar, 0));
	CHECK(!pci->bar_map(native, 0, 0, 4096,
		KB2_PCI_FUNCTION_MAP_PROTECTION_READ | KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE,
		KB2_PCI_FUNCTION_CACHE_UC_MINUS, NULL, &bar));
	CHECK(!pci->bar_write(native, 0, 0, 4, 0x42));
	CHECK(*(volatile unsigned int *)bar == 0x42);
	CHECK(pci->bar_write(native, 0, UINT64_MAX, 4, 7) == -EINVAL);
	CHECK(!pci->bar_unmap(native, bar, 4096));
	kobox_pci_fixture_release(&owner, native);
	CHECK(owner.imported == owner.released);
	CHECK(denied(bar, 0));
	CHECK(!close(handle.handle));
	return 0;
}

static int enumeration_fixture(void)
{
	struct kobox_pci_fixture_owner owner = {.generation = 17, .object_id = 101};
	kb2_resource_grant_slot_t slot = {
		.slot_id = 1, .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
		.interface_schema_digest = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES,
	};
	kb2_resource_grant_object_t object = {
		.slot_id = 1, .object_id = 101,
		.granted_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
	};
	struct kobox_resource_native_handle handle = {
		.role = KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE,
	};
	const struct kobox_resource_interface_operations *base;
	const struct kobox_pci_function_resource_operations *pci;
	struct stat info;
	void *native, *mapped;
	unsigned char *window;
	uint32_t value;
	const uint32_t read = KB2_PCI_FUNCTION_MAP_PROTECTION_READ;
	const uint32_t write = KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE;
	const uint32_t cache = KB2_PCI_FUNCTION_CACHE_UC_MINUS;

	handle.handle = memfd_create("pci-enum-unit", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	CHECK(handle.handle >= 0 && !ftruncate(handle.handle, KOBOX_PCI_ENUM_FIXTURE_SIZE));
	CHECK(!fstat(handle.handle, &info));
	owner.device = info.st_dev;
	owner.inode = info.st_ino;
	CHECK(!fcntl(handle.handle, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_GROW | F_SEAL_SHRINK));
	CHECK(!kobox_pci_enum_fixture_import(&owner, &slot, &object, &handle, 1,
					    &native, &base));
	pci = (const void *)base;
	window = mmap(NULL, 3 * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(window != MAP_FAILED);
	CHECK(!pci->bar_map(native, 0, 0, 4096, read | write, cache, window, &mapped));
	CHECK(mapped == window);
	CHECK(!pci->bar_map(native, 0, 0, 4096, read, cache, window + 4096, &mapped));
	CHECK(mapped == window + 4096);
	*(volatile uint32_t *)window = 0x12345678;
	CHECK(*(volatile uint32_t *)(window + 4096) == 0x12345678);
	CHECK(denied(window + 4096, 1));
	CHECK(pci->bar_map(native, 0, 0, 4096, read, cache, window, &mapped) == -EBUSY);
	CHECK(!mapped && *(volatile uint32_t *)window == 0x12345678);
	CHECK(pci->bar_map(native, 0, 0x4000, 4096, read, cache,
			  window + 8192, &mapped) == -EINVAL);
	CHECK(pci->bar_map(native, 0, 0, 4096, read, KB2_PCI_FUNCTION_CACHE_WB,
			  window + 8192, &mapped) == -EOPNOTSUPP);
	CHECK(denied(window + 8192, 0));
	CHECK(!pci->config_write(native, 0x148, 4, 1));
	CHECK(pci->bar_map(native, 0, 4096, 4096, read, cache,
			  window + 8192, &mapped) == -ENOMEM);
	CHECK(!mapped && denied(window + 8192, 0));
	CHECK(!pci->bar_unmap(native, window, 4096));
	CHECK(denied(window, 0));
	CHECK(pci->bar_unmap(native, window, 4096) == -EINVAL);
	CHECK(pci->bar_write(native, 0, 0, 4, 9) == -EACCES);
	CHECK(!pci->bar_read(native, 0, 0, 4, &value) && value == 0x12345678);
	CHECK(!pci->bar_unmap(native, window + 4096, 4096));
	CHECK(denied(window + 4096, 0));
	CHECK(!pci->bar_map(native, 2, 0, 4096, read | write, cache, window, &mapped));
	CHECK(mapped == window);
	*(volatile uint32_t *)(window + 0x100) = 0xfeed;
	CHECK(!pci->bar_read(native, 2, 0, 4, &value) && value == 0xfeed);
	CHECK(pci->bar_read(native, 2, 0x100, 4, &value) == -EACCES);
	CHECK(!pci->bar_write(native, 3, 0, 4, 0xbeef));
	CHECK(*(volatile uint32_t *)(window + 0x300) == 0xbeef);
	CHECK(!pci->bar_map(native, 3, 0, 4096, read, cache, window + 4096, &mapped));
	CHECK(*(volatile uint32_t *)(window + 4096 + 0x300) == 0xbeef);
	CHECK(pci->bar_map(native, 2, 4096, 4096, read, cache,
			  window + 8192, &mapped) == -EINVAL);
	CHECK(!pci->bar_unmap(native, window, 4096) && denied(window, 0));
	CHECK(*(volatile uint32_t *)(window + 4096 + 0x100) == 0xfeed);
	CHECK(!pci->bar_unmap(native, window + 4096, 4096));
	kobox_pci_enum_fixture_release(&owner, native);
	CHECK(owner.imported == 1 && owner.released == 1);
	CHECK(!munmap(window, 3 * 4096) && !close(handle.handle));
	return 0;
}

static int config_order(void)
{
	struct kobox_pci_config_fixture fixture;
	struct kobox_linux_pci_host host;

	kobox_pci_config_fixture_init(&fixture, &host);
	CHECK(!host.config_write(&fixture, PCI_BASE_ADDRESS_0, 4, UINT32_MAX));
	CHECK(fixture.bad_sizing == 1);
	kobox_pci_config_fixture_init(&fixture, &host);
	CHECK(!host.config_write(&fixture, PCI_COMMAND, 2, 0));
	CHECK(!host.config_write(&fixture, PCI_BASE_ADDRESS_0, 4, UINT32_MAX));
	CHECK(!host.config_write(&fixture, PCI_BASE_ADDRESS_1, 4, UINT32_MAX));
	CHECK(!fixture.bad_sizing && fixture.sizing_pending == 3);
	CHECK(!host.config_write(&fixture, PCI_BASE_ADDRESS_0, 4, 0x20000000));
	CHECK(fixture.sizing_pending == 2);
	CHECK(!host.config_write(&fixture, PCI_COMMAND, 2, PCI_COMMAND_MEMORY));
	CHECK(fixture.bad_sizing == 1);
	CHECK(!host.config_write(&fixture, PCI_COMMAND, 2, 0));
	CHECK(!host.config_write(&fixture, PCI_BASE_ADDRESS_1, 4, 1));
	CHECK(!host.config_write(&fixture, PCI_COMMAND, 2, PCI_COMMAND_MEMORY));
	CHECK(!fixture.sizing_pending && fixture.bad_sizing == 1);
	return 0;
}

int main(void)
{
	return legacy_fixture() || enumeration_fixture() || config_order();
}
