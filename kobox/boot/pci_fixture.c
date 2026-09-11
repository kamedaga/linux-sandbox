// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "pci_fixture.h"
#include "../provider/device_resource_interfaces.h"

#include <kobox2/pci_function_layout.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct pci_fixture {
	uint64_t generation, object_id;
	unsigned char *config, *bar;
	unsigned int protection;
	int descriptor;
};

static bool access_valid(uint64_t offset, uint32_t width)
{
	return (width == 1 || width == 2 || width == 4) &&
	       offset <= 4096 - width && !(offset % width);
}

static uint32_t load(const unsigned char *bytes, uint32_t width)
{
	uint32_t value = 0, index;

	for (index = 0; index < width; index++)
		value |= (uint32_t)bytes[index] << (8 * index);
	return value;
}

static void store(unsigned char *bytes, uint32_t width, uint32_t value)
{
	uint32_t index;

	for (index = 0; index < width; index++)
		bytes[index] = value >> (8 * index);
}

static int identity(void *object, struct kobox_pci_function_identity *out)
{
	struct pci_fixture *pci = object;

	if (!out)
		return -EINVAL;
	*out = (struct kobox_pci_function_identity) {
		.generation = pci->generation, .object_id = pci->object_id,
		.vendor_id = load(pci->config, 2), .device_id = load(pci->config + 2, 2),
	};
	return 0;
}

static int config_read(void *object, uint32_t offset, uint32_t width, uint32_t *out)
{
	struct pci_fixture *pci = object;

	if (!out || !access_valid(offset, width))
		return -EINVAL;
	*out = load(pci->config + offset, width);
	return 0;
}

static int config_write(void *object, uint32_t offset, uint32_t width, uint32_t value)
{
	struct pci_fixture *pci = object;

	if (!access_valid(offset, width))
		return -EINVAL;
	/* Fixture identity/header registers are read-only. */
	if (offset < 0x40)
		return -EACCES;
	store(pci->config + offset, width, value);
	return 0;
}

static int bar_info(void *object, uint32_t bar, uint64_t *cpu_address,
		    uint64_t *length, uint32_t *flags)
{
	struct pci_fixture *pci = object;
	if (!cpu_address || !length || !flags || bar >= KB2_PCI_FUNCTION_BAR_COUNT)
		return -EINVAL;
	if (bar)
		return -ENOENT;
	*cpu_address = load(pci->config + 0x10, 4) & ~0xfU;
	*length = 4096;
	*flags = KB2_PCI_FUNCTION_BAR_FLAG_MEMORY;
	return 0;
}

static int bar_map(void *object, uint32_t bar, uint64_t offset, size_t length,
		   uint32_t protection, uint32_t cache_type, void *requested_address,
		   void **address)
{
	struct pci_fixture *pci = object;
	int native = 0;

	if (!address || bar || offset || length != 4096 || !protection ||
	    protection & ~(KB2_PCI_FUNCTION_MAP_PROTECTION_READ | KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE))
		return -EINVAL;
	*address = NULL;
	if (requested_address || cache_type != KB2_PCI_FUNCTION_CACHE_UC_MINUS)
		return -EOPNOTSUPP;
	if (pci->protection)
		return -EBUSY;
	if (protection & KB2_PCI_FUNCTION_MAP_PROTECTION_READ)
		native |= PROT_READ;
	if (protection & KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE)
		native |= PROT_WRITE;
	if (mprotect(pci->bar, 4096, native))
		return -errno;
	pci->protection = protection;
	*address = pci->bar;
	return 0;
}

static int bar_unmap(void *object, void *address, size_t length)
{
	struct pci_fixture *pci = object;

	if (!pci->protection || address != pci->bar || length != 4096)
		return -EINVAL;
	if (mprotect(pci->bar, 4096, PROT_NONE))
		return -errno;
	pci->protection = 0;
	return 0;
}

static int bar_read(void *object, uint32_t bar, uint64_t offset, uint32_t width, uint32_t *out)
{
	struct pci_fixture *pci = object;

	if (!out || bar || !access_valid(offset, width))
		return -EINVAL;
	if (!(pci->protection & KB2_PCI_FUNCTION_MAP_PROTECTION_READ))
		return -EACCES;
	*out = load(pci->bar + offset, width);
	return 0;
}

static int bar_write(void *object, uint32_t bar, uint64_t offset, uint32_t width, uint32_t value)
{
	struct pci_fixture *pci = object;

	if (bar || !access_valid(offset, width))
		return -EINVAL;
	if (!(pci->protection & KB2_PCI_FUNCTION_MAP_PROTECTION_WRITE))
		return -EACCES;
	store(pci->bar + offset, width, value);
	return 0;
}

static const struct kobox_pci_function_resource_operations operations = {
	.base = {.size = sizeof(operations), .identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER},
	.identity = identity, .config_read = config_read, .config_write = config_write,
	.bar_info = bar_info, .bar_map = bar_map, .bar_unmap = bar_unmap,
	.bar_read = bar_read, .bar_write = bar_write,
};

int kobox_pci_fixture_descriptor(void *context, const kb2_resource_grant_slot_t *slot,
			     const kb2_resource_grant_object_t *object,
			     const struct kobox_resource_native_handle *handles,
			     size_t handle_count, size_t expected_size, int *descriptor_out)
{
	static const uint8_t digest[32] = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES;
	struct kobox_pci_fixture_owner *owner = context;
	const int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW;
	struct stat info;
	int descriptor, present;

	if (!descriptor_out)
		return -EINVAL;
	*descriptor_out = -1;
	if (!owner || !owner->generation || !slot || !object || !handles ||
	    slot->resource_type != KB2_CLOSURE_RESOURCE_DEVICE ||
	    memcmp(slot->interface_schema_digest, digest, 32) ||
	    object->object_id != owner->object_id || object->granted_rights != KB2_PCI_FUNCTION_REQUIRED_RIGHTS ||
	    handle_count != 1 || handles[0].role != KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE)
		return -EINVAL;
	descriptor = fcntl(handles[0].handle, F_DUPFD_CLOEXEC, 0);
	if (descriptor < 0)
		return -errno;
	present = fcntl(descriptor, F_GET_SEALS);
	if (fstat(descriptor, &info) || !S_ISREG(info.st_mode) ||
	    info.st_size < 0 || (uint64_t)info.st_size != expected_size ||
	    info.st_dev != owner->device || info.st_ino != owner->inode ||
	    present < 0 || (present & seals) != seals) {
		close(descriptor);
		return -EPERM;
	}
	*descriptor_out = descriptor;
	return 0;
}

int kobox_pci_fixture_import(void *context, const kb2_resource_grant_slot_t *slot,
			     const kb2_resource_grant_object_t *object,
			     const struct kobox_resource_native_handle *handles,
			     size_t handle_count, void **object_out,
			     const struct kobox_resource_interface_operations **operations_out)
{
	struct kobox_pci_fixture_owner *owner = context;
	struct pci_fixture *pci;
	int descriptor, error;

	if (!object_out || !operations_out)
		return -EINVAL;
	*object_out = NULL;
	*operations_out = NULL;
	error = kobox_pci_fixture_descriptor(context, slot, object, handles, handle_count,
					     KOBOX_PCI_FIXTURE_SIZE, &descriptor);
	if (error)
		return error;
	pci = calloc(1, sizeof(*pci));
	if (!pci) {
		close(descriptor);
		return -ENOMEM;
	}
	pci->descriptor = descriptor;
	pci->generation = owner->generation;
	pci->object_id = owner->object_id;
	pci->config = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	if (pci->config == MAP_FAILED) {
		error = -errno;
		goto fail;
	}
	pci->bar = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, descriptor, 4096);
	if (pci->bar == MAP_FAILED) {
		error = -errno;
		munmap(pci->config, 4096);
		goto fail;
	}
	owner->imported++;
	*object_out = pci;
	*operations_out = &operations.base;
	return 0;
fail:
	close(descriptor);
	free(pci);
	return error;
}

void kobox_pci_fixture_release(void *context, void *object)
{
	struct kobox_pci_fixture_owner *owner = context;
	struct pci_fixture *pci = object;

	if (pci->protection)
		abort();
	munmap(pci->bar, 4096);
	munmap(pci->config, 4096);
	close(pci->descriptor);
	free(pci);
	owner->released++;
}
