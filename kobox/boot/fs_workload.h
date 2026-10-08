/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_WORKLOAD_H
#define KOBOX_BOOT_FS_WORKLOAD_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <uapi/linux/openat2.h>
#include <uapi/linux/memfd.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/openat2.h>
#include <linux/fs.h>
#include <linux/memfd.h>
#endif

enum fs_test_role { FS_ROOT, FS_USER, FS_GROUP, FS_OTHER, FS_ROLES };
enum fs_test_change { FS_TEST_MKDIR, FS_TEST_UNLINK, FS_TEST_RENAME,
		      FS_TEST_SYMLINK, FS_TEST_LINK };
enum fs_test_meta { FS_TEST_CHMOD, FS_TEST_CHOWN, FS_TEST_ACCESS,
		    FS_TEST_MKNOD, FS_TEST_SYNCFS };

/* Test-only adapter: compile one workload against the port and native Linux
 * syscalls, so an expected result cannot diverge between the two drivers.
 */
struct fs_test_stat {
	uint64_t size;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint32_t nlink;
	int64_t atime, mtime;
	uint32_t atime_nsec, mtime_nsec;
};

struct fs_test_ops {
	int (*openat)(void *ctx, int role, uint64_t directory, const char *name,
		      uint64_t flags, uint64_t mode, uint64_t resolve,
		      unsigned int mask, uint64_t *handle);
	int (*open)(void *ctx, int role, const char *name, uint64_t flags,
		    uint64_t mode, uint64_t resolve, uint64_t *handle);
	int (*close)(void *ctx, int role, uint64_t handle);
	int (*dup)(void *ctx, int role, uint64_t handle, uint64_t *duplicate);
	int64_t (*io)(void *ctx, int role, uint64_t handle, void *buffer,
		      size_t count, int64_t offset, int write, int positioned);
	int64_t (*seek)(void *ctx, uint64_t handle, int64_t offset, int whence);
	int64_t (*getdents)(void *ctx, uint64_t handle, void *buffer,
			   unsigned int count);
	int (*stat)(void *ctx, const char *name, uint64_t handle, int flags,
		    struct fs_test_stat *stat);
	int (*change)(void *ctx, int role, int operation, const char *from,
		      const char *to, unsigned int flags);
	int64_t (*readlink)(void *ctx, const char *name, void *buf, size_t count);
	int (*truncate)(void *ctx, uint64_t handle, int64_t length);
	int (*sync)(void *ctx, uint64_t handle, int data_only);
	int (*metadata)(void *ctx, int role, int operation, uint64_t handle,
			const char *path, uint64_t first, uint64_t second,
			unsigned int flags);
	int64_t (*fcntl)(void *ctx, uint64_t handle, unsigned int command,
			 uint64_t argument);
	int (*memfd)(void *ctx, const char *name, unsigned int flags, uint64_t *handle);
	int (*utimes)(void *ctx, const char *path, const int64_t *times, unsigned int flags);
};

struct fs_test_report {
	uint32_t checks;
	uint32_t line;
	int64_t actual;
	int64_t expected;
	uint64_t digest;
};

int kobox_fs_workload(const struct fs_test_ops *ops, void *ctx,
		      struct fs_test_report *report);

#endif /* KOBOX_BOOT_FS_WORKLOAD_H */
