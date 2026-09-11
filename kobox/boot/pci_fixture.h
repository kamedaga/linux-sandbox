/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_PCI_FIXTURE_H
#define KOBOX_BOOT_PCI_FIXTURE_H

#include "../host/posix/resource.h"

#include <sys/types.h>

#define KOBOX_PCI_FIXTURE_SIZE 8192U
#define KOBOX_PCI_ENUM_FIXTURE_SIZE 24576U

/* Trusted launch-owner identity of one isolated conformance PCI function.
 * This is host-local state, never data accepted from an untrusted grant.
 * Physical VFIO reset domains are not represented by this fixture.
 */
struct kobox_pci_fixture_owner {
	uint64_t generation;
	uint64_t object_id;
	dev_t device;
	ino_t inode;
	unsigned int imported;
	unsigned int released;
};

int kobox_pci_fixture_descriptor(void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles, size_t handle_count,
	size_t expected_size, int *descriptor_out);

int kobox_pci_enum_fixture_import(void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles, size_t handle_count,
	void **object_out, const struct kobox_resource_interface_operations **operations_out);
void kobox_pci_enum_fixture_release(void *context, void *object);

int kobox_pci_fixture_import(void *context, const kb2_resource_grant_slot_t *slot,
			     const kb2_resource_grant_object_t *object,
			     const struct kobox_resource_native_handle *handles,
			     size_t handle_count, void **object_out,
			     const struct kobox_resource_interface_operations **operations_out);
void kobox_pci_fixture_release(void *context, void *object);

#endif
