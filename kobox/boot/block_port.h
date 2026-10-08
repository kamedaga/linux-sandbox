/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_BLOCK_PORT_H
#define KOBOX_BOOT_BLOCK_PORT_H

#include "../task/host.h"

#define KOBOX_BLOCK_NAME_BYTES 32U
#define KOBOX_BLOCK_IO_MAX (128U * 1024U)

/* These are process-local decoded values. The host transport must serialize
 * them explicitly; neither Linux pointers nor dev_t cross the boundary. */
struct kobox_linux_block_info {
	uint64_t diskseq;
	uint64_t bytes;
	uint32_t major;
	uint32_t minor;
	uint32_t logical_block_size;
	uint32_t physical_block_size;
	uint32_t removable;
	uint32_t read_only;
	char name[KOBOX_BLOCK_NAME_BYTES];
};

struct kobox_linux_block_key {
	uint64_t diskseq;
	uint32_t major;
	uint32_t minor;
};

struct kobox_linux_block_port;
#ifdef __KERNEL__
struct pci_dev;

/* The PCI ancestor limits discovery to this sandbox's one granted function.
 * Snapshot and I/O revalidate diskseq so a removed device cannot turn an old
 * selection into access to a newly reused major/minor. */
int kobox_linux_block_port_open(struct kobox_linux_block_port **out,
				struct pci_dev *pci,
				const struct kobox_linux_block_key *write_key);
void kobox_linux_block_port_close(struct kobox_linux_block_port *port);
int kobox_linux_block_port_snapshot(struct kobox_linux_block_port *port,
		size_t start, struct kobox_linux_block_info *devices,
		size_t capacity, size_t *count, size_t *next);
int kobox_linux_block_port_read(struct kobox_linux_block_port *port,
		const struct kobox_linux_block_key *key, u64 offset,
		void *buffer, size_t length);
int kobox_linux_block_port_write(struct kobox_linux_block_port *port,
		const struct kobox_linux_block_key *key, u64 offset,
		const void *buffer, size_t length);
int kobox_linux_block_port_flush(struct kobox_linux_block_port *port,
		const struct kobox_linux_block_key *key);
#endif

#endif
