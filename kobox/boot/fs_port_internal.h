/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_PORT_INTERNAL_H
#define KOBOX_BOOT_FS_PORT_INTERNAL_H

#include "fs_port.h"

struct kobox_linux_fs_table;
struct fs_struct;
struct kobox_linux_fs_shared;
/* A request view borrows the client's table and owns only these file refs.
 * The request's retained client keeps the table/root alive. Installs and CLOSE
 * affect that same table; lookups of admitted operands use the pinned OFDs.
 */
struct kobox_linux_fs_port {
	struct kobox_linux_fs_table *table;
	struct kobox_linux_fs_shared *shared; /* Service-owned, retained by client. */
	struct file *pinned[2];
	u64 handles[2];
	/* Borrowed only during execution from this worker's exclusive context. */
	struct fs_struct *scope;
	/* Inline attempt on the ring owner: an operation that could sleep for
	 * I/O, an uncached lookup or another task must return -EAGAIN before any
	 * visible effect, so a worker can then execute the request unchanged. */
	bool nowait;
};

int kobox_linux_fs_port_snapshot(struct kobox_linux_fs_port *port,
	u64 first, u64 second, struct kobox_linux_fs_port *view);
void kobox_linux_fs_port_release(struct kobox_linux_fs_port *view);
bool kobox_linux_fs_port_release_nonfinal(struct kobox_linux_fs_port *view);
int kobox_linux_fs_port_shared_export(struct kobox_linux_fs_port *,
	const struct cred *, u64 handle, u64 client, u64 sequence, u64 offset,
	u64 length, u64 flags, u32 prot, u64 *id, u64 *file_size);

#endif /* KOBOX_BOOT_FS_PORT_INTERNAL_H */
