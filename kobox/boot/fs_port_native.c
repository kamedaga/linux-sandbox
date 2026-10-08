// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "fs_workload.h"

#include <errno.h>
#include <grp.h>
#include <linux/memfd.h>
#include <asm/unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/fsuid.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Run as root inside a disposable Linux guest, chrooted into the test FS.
 * fsuid/fsgid and supplementary groups mirror the port's override_creds.
 */
static void native_role(int role)
{
	gid_t group = 2001;

	/* faccessat2 uses real/effective IDs, not just fsuid. Keep saved root
	 * authority solely to switch roles inside this disposable fixture.
	 */
	if (setresuid(0, 0, 0) || setresgid(0, 0, 0))
		abort();
	if (setgroups(role == FS_GROUP ? 1 : 0, &group))
		abort();
	if (setresgid(role ? 2000 + role : 0, role ? 2000 + role : 0, 0) ||
	    setresuid(role ? 1000 + role : 0, role ? 1000 + role : 0, 0))
		abort();
}

static int native_open(void *ctx, int role, const char *name, uint64_t flags,
		       uint64_t mode, uint64_t resolve, uint64_t *handle)
{
	struct open_how how = {.flags = flags, .mode = mode, .resolve = resolve};
	int fd, error;

	(void)ctx;
	native_role(role);
	fd = syscall(SYS_openat2, AT_FDCWD, name, &how, sizeof(how));
	error = fd < 0 ? -errno : 0;
	if (!error)
		*handle = fd;
	native_role(FS_ROOT);
	return error;
}

static int native_openat(void *ctx, int role, uint64_t directory, const char *name,
			 uint64_t flags, uint64_t mode, uint64_t resolve,
			 unsigned int mask, uint64_t *handle)
{
	struct open_how how = {.flags = flags, .mode = mode, .resolve = resolve};
	mode_t saved;
	int fd, error;

	(void)ctx;
	native_role(role);
	saved = umask(mask);
	fd = syscall(SYS_openat2, directory ? (int)directory : AT_FDCWD,
		     name, &how, sizeof(how));
	error = fd < 0 ? -errno : 0;
	if (!error)
		*handle = fd;
	umask(saved);
	native_role(FS_ROOT);
	return error;
}

static int native_close(void *ctx, int role, uint64_t handle)
{
	int result;

	(void)ctx;
	native_role(role);
	result = close(handle) ? -errno : 0;
	native_role(FS_ROOT);
	return result;
}

static int native_dup(void *ctx, int role, uint64_t handle, uint64_t *duplicate)
{
	int fd, error;

	(void)ctx;
	native_role(role);
	fd = dup(handle);
	error = fd < 0 ? -errno : 0;
	if (!error)
		*duplicate = fd;
	native_role(FS_ROOT);
	return error;
}

static int64_t native_io(void *ctx, int role, uint64_t handle, void *buf,
			 size_t count, int64_t offset, int write_io, int positioned)
{
	ssize_t result;

	(void)ctx;
	native_role(role);
	if (positioned)
		result = write_io ? pwrite(handle, buf, count, offset) :
				    pread(handle, buf, count, offset);
	else
		result = write_io ? write(handle, buf, count) : read(handle, buf, count);
	if (result < 0)
		result = -errno;
	native_role(FS_ROOT);
	return result;
}

static int64_t native_seek(void *ctx, uint64_t handle, int64_t offset, int whence)
{
	off_t result;

	(void)ctx;
	result = lseek(handle, offset, whence);
	return result < 0 ? -errno : result;
}

static int64_t native_getdents(void *ctx, uint64_t handle, void *buffer,
			     unsigned int count)
{
	long result;

	(void)ctx;
	result = syscall(SYS_getdents64, handle, buffer, count);
	return result < 0 ? -errno : result;
}

static int native_stat(void *ctx, const char *name, uint64_t handle, int flags,
		       struct fs_test_stat *result)
{
	struct stat stat;
	int error;

	(void)ctx;
	error = name ? fstatat(AT_FDCWD, name, &stat, flags) : fstat(handle, &stat);
	if (error)
		return -errno;
	*result = (struct fs_test_stat) {
		.size = stat.st_size, .mode = stat.st_mode, .uid = stat.st_uid,
		.gid = stat.st_gid, .nlink = stat.st_nlink,
		.atime = stat.st_atim.tv_sec, .mtime = stat.st_mtim.tv_sec,
		.atime_nsec = stat.st_atim.tv_nsec, .mtime_nsec = stat.st_mtim.tv_nsec,
	};
	return 0;
}

static int native_change(void *ctx, int role, int operation, const char *from,
			 const char *to, unsigned int flags)
{
	int result;

	(void)ctx;
	native_role(role);
	switch (operation) {
	case FS_TEST_MKDIR:
		result = mkdir(from, flags);
		break;
	case FS_TEST_UNLINK:
		result = unlinkat(AT_FDCWD, from, flags);
		break;
	case FS_TEST_RENAME:
		result = syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, flags);
		break;
	case FS_TEST_SYMLINK:
		result = symlink(from, to);
		break;
	case FS_TEST_LINK:
		result = linkat(AT_FDCWD, from, AT_FDCWD, to, flags);
		break;
	default:
		abort();
	}
	result = result < 0 ? -errno : result;
	native_role(FS_ROOT);
	return result;
}

static int64_t native_readlink(void *ctx, const char *name, void *buf, size_t count)
{
	ssize_t result;

	(void)ctx;
	result = readlink(name, buf, count);
	return result < 0 ? -errno : result;
}

static int native_truncate(void *ctx, uint64_t handle, int64_t length)
{
	(void)ctx;
	return ftruncate(handle, length) ? -errno : 0;
}

static int native_sync(void *ctx, uint64_t handle, int data_only)
{
	int result;

	(void)ctx;
	result = data_only ? fdatasync(handle) : fsync(handle);
	return result ? -errno : 0;
}

static int native_metadata(void *ctx, int role, int operation, uint64_t handle,
			   const char *path, uint64_t first, uint64_t second,
			   unsigned int flags)
{
	int fd = handle ? (int)handle : AT_FDCWD;
	long result;

	(void)ctx;
	native_role(role);
	switch (operation) {
	case FS_TEST_CHMOD:
		result = syscall(__NR_fchmodat2, fd, path, first, flags);
		break;
	case FS_TEST_CHOWN:
		result = syscall(SYS_fchownat, fd, path, first, second, flags);
		break;
	case FS_TEST_ACCESS:
		result = syscall(SYS_faccessat2, fd, path, first, flags);
		break;
	case FS_TEST_MKNOD:
		result = syscall(SYS_mknodat, fd, path, first, second);
		break;
	case FS_TEST_SYNCFS:
		result = syscall(SYS_syncfs, fd);
		break;
	default:
		abort();
	}
	if (result < 0)
		result = -errno;
	native_role(FS_ROOT);
	return result;
}

static int64_t native_fcntl(void *ctx, uint64_t handle, unsigned int command,
			    uint64_t argument)
{
	long result;

	(void)ctx;
	result = syscall(SYS_fcntl, handle, command, argument);
	return result < 0 ? -errno : result;
}

static int native_memfd(void *ctx, const char *name, unsigned int flags, uint64_t *handle)
{
	long result;

	(void)ctx;
	result = syscall(SYS_memfd_create, name, flags);
	if (result < 0)
		return -errno;
	*handle = result;
	return 0;
}

static int native_utimes(void *ctx, const char *path, const int64_t *times,
			 unsigned int flags)
{
	struct timespec native[2];
	long result;

	(void)ctx;
	if (times) {
		native[0] = (struct timespec){times[0], times[1]};
		native[1] = (struct timespec){times[2], times[3]};
	}
	result = syscall(SYS_utimensat, AT_FDCWD, path, times ? native : NULL, flags);
	return result < 0 ? -errno : result;
}

static const struct fs_test_ops native_ops = {
	.openat = native_openat,
	.open = native_open, .close = native_close, .dup = native_dup,
	.io = native_io, .seek = native_seek, .getdents = native_getdents,
	.stat = native_stat, .change = native_change, .readlink = native_readlink,
	.truncate = native_truncate, .sync = native_sync,
	.metadata = native_metadata, .fcntl = native_fcntl,
	.memfd = native_memfd, .utimes = native_utimes,
};

int main(int argc, char **argv)
{
	struct fs_test_report report = {0};
	int error;

	if (argc != 3 || getuid() || chroot(argv[2]) || chdir("/")) {
		fprintf(stderr, "usage (root): %s ext4|tmpfs EMPTY_MOUNT\n", argv[0]);
		return 2;
	}
	umask(0);
	error = kobox_fs_workload(&native_ops, NULL, &report);
	printf("FS_RESULT fs=%s checks=%u digest=%llu line=%u actual=%lld expected=%lld\n",
	       argv[1], report.checks, (unsigned long long)report.digest,
	       report.line, (long long)report.actual, (long long)report.expected);
	return error ? 1 : 0;
}
