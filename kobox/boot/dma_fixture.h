/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DMA_FIXTURE_H
#define KOBOX_BOOT_DMA_FIXTURE_H

#include "dma_gate.h"
#include "resource_port.h"
#include "../host/posix/resource.h"
#include <sys/types.h>

struct kobox_dma_fixture;

/* Standalone conformance engine: start before Linux mappings or host threads. */
int kobox_dma_fixture_start(const char *core_path, struct kobox_dma_fixture **out);
int kobox_dma_fixture_prepare_host(void *context, int descriptor, size_t length);
void kobox_dma_fixture_stop(void *context);

int kobox_dma_fixture_test_main(int argc, char **argv);
int kobox_dma_fixture_engine(int socket, int fail_unmap);
/* fstat and close the trusted launch anchor before resource import. */
int kobox_dma_fixture_create(uint64_t generation, uint64_t object_id,
			     uint64_t device_id, dev_t device, ino_t inode,
			     struct kobox_dma_fixture **out);
int kobox_dma_fixture_import(void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles, size_t count, void **out,
	const struct kobox_resource_interface_operations **operations);
void kobox_dma_fixture_release(void *context, void *object);
int kobox_dma_fixture_prepare(struct kobox_dma_fixture *fixture,
			      const struct kobox_linux_resource_port *port,
			      int ram_descriptor, size_t ram_length);
const struct kobox_linux_dma_test *kobox_dma_fixture_test(struct kobox_dma_fixture *fixture);
/* Finish with the imported object alive, close the registry, then destroy.
 * The controller owns and reaps the separate device process.
 */
int kobox_dma_fixture_finish(struct kobox_dma_fixture *fixture);
void kobox_dma_fixture_destroy(struct kobox_dma_fixture *fixture);

#endif
