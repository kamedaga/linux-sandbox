/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_SHARED_H
#define KOBOX_BOOT_FS_SHARED_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Process-local callbacks, not a filesystem wire ABI. Linux owns the file,
 * page-cache folio references and serialization. The native host owns each
 * opaque export and revokes its aliases before reporting retirement success.
 * UINT64_MAX denotes unavailable backing. No Linux pointer crosses a queue.
 * All callbacks execute on an actual Linux task, with the shared registry
 * write lock held; they must not reenter VFS or take Linux locks.
 */
struct kobox_linux_fs_shared_ops {
	void *context;
	uint64_t budget_bytes;
	int (*create)(void *, uint64_t client, uint64_t sequence,
		const uint64_t *pages, size_t count, uint64_t flags, uint64_t *id);
	int (*refresh)(void *, uint64_t id, const uint64_t *pages, size_t count);
	/* Exact logical-page bitmap. Failure preserves native dirty inventory;
	 * Linux must retain pins and retry rather than acknowledge writeback. */
	int (*collect_dirty)(void *, uint64_t id, uint64_t *bitmap, size_t words);
	/* force=0: return 1 only after exclusive ownership AND alias retirement;
	 * 0 keeps the export alive. force=1 may revoke live mappings for shutdown.
	 * Errors always preserve Linux pins and the native owner's inventory.
	 */
	int (*retire)(void *, uint64_t id, int force);
};
#ifdef __KERNEL__
struct kobox_linux_fs_shared;
struct file;
struct address_space;
struct super_block;
struct path;
/* Stack-owned acquisition mode. An ordinary data guard can coexist with
 * exports; mutations and direct I/O take the write guard for matching exports. */
enum kobox_linux_fs_shared_mode {
	KOBOX_FS_SHARED_NONE, KOBOX_FS_SHARED_READ, KOBOX_FS_SHARED_WRITE,
};
struct kobox_linux_fs_shared_guard {
	enum kobox_linux_fs_shared_mode mode;
	/* Borrowed from the admitted file until leave; NULL covers all exports. */
	struct address_space *mapping;
	struct super_block *superblock;
};
struct kobox_linux_fs_shared *kobox_linux_fs_shared_create(void);
int kobox_linux_fs_shared_bind(struct kobox_linux_fs_shared *,
	const struct kobox_linux_fs_shared_ops *);
int kobox_linux_fs_shared_close(struct kobox_linux_fs_shared *);
void kobox_linux_fs_shared_destroy(struct kobox_linux_fs_shared *);
int kobox_linux_fs_shared_enter(struct kobox_linux_fs_shared *, bool nowait,
	bool publishing, struct kobox_linux_fs_shared_guard *);
int kobox_linux_fs_shared_enter_file(struct kobox_linux_fs_shared *, bool nowait,
	bool publishing, struct file *, struct kobox_linux_fs_shared_guard *);
int kobox_linux_fs_shared_enter_path(struct kobox_linux_fs_shared *, bool nowait,
	const struct path *, struct kobox_linux_fs_shared_guard *);
int kobox_linux_fs_shared_enter_sb(struct kobox_linux_fs_shared *, bool nowait,
	struct file *, struct kobox_linux_fs_shared_guard *);
int kobox_linux_fs_shared_enter_read(struct kobox_linux_fs_shared *, bool nowait,
	struct file *, struct kobox_linux_fs_shared_guard *);
int kobox_linux_fs_shared_leave(struct kobox_linux_fs_shared *, bool refresh,
	struct kobox_linux_fs_shared_guard *);
/* Try the shmem seal bridge only when owned native RO pins need accounting.
 * The caller holds the registry lock and the authenticated credential scope.
 * false leaves the ordinary Linux fcntl syscall responsible for the request.
 */
bool kobox_linux_fs_shared_add_seals(struct kobox_linux_fs_shared *,
	struct file *, unsigned long argument, long *result);
int kobox_linux_fs_shared_export(struct kobox_linux_fs_shared *, struct file *,
	u64 client, u64 sequence, u64 offset, u64 length, u64 flags, u32 prot,
	u64 *id, u64 *file_size);
#endif
#endif
