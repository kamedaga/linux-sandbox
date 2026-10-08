// SPDX-License-Identifier: GPL-2.0-only
#include "fs_workload.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

static int fs_expect(struct fs_test_report *report, unsigned int line,
		     int64_t actual, int64_t expected)
{
	report->checks++;
	report->digest ^= (uint64_t)actual;
	report->digest *= 1099511628211ULL;
	if (actual == expected)
		return 0;
	report->line = line;
	report->actual = actual;
	report->expected = expected;
	return -EINVAL;
}

#define EXPECT(expression, expected) do { \
	if (fs_expect(report, __LINE__, (expression), (expected))) \
		return -EINVAL; \
} while (0)

static int fs_basic(const struct fs_test_ops *ops, void *ctx,
		    struct fs_test_report *report)
{
	struct fs_test_stat stat;
	uint64_t file, duplicate, other;
	char buffer[32];

	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_MKDIR, "suite", NULL, 0777), 0);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/data", O_CREAT | O_EXCL | O_RDWR,
			 0640, 0, &file), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, "abcdef", 6, 0, 1, 0), 6);
	EXPECT(ops->dup(ctx, FS_ROOT, file, &duplicate), 0);
	EXPECT(ops->seek(ctx, duplicate, 0, SEEK_CUR), 6);
	EXPECT(ops->seek(ctx, file, 0, SEEK_SET), 0);
	EXPECT(ops->io(ctx, FS_USER, duplicate, buffer, 2, 0, 0, 0), 2);
	EXPECT(memcmp(buffer, "ab", 2) == 0, 1);
	EXPECT(ops->seek(ctx, file, 0, SEEK_CUR), 2);
	EXPECT(ops->io(ctx, FS_ROOT, file, buffer, 3, 1, 0, 1), 3);
	EXPECT(memcmp(buffer, "bcd", 3) == 0, 1);
	EXPECT(ops->seek(ctx, duplicate, 0, SEEK_CUR), 2);
	EXPECT(ops->io(ctx, FS_ROOT, file, "XY", 2, 10, 1, 1), 2);
	EXPECT(ops->io(ctx, FS_ROOT, file, buffer, 6, 6, 0, 1), 6);
	EXPECT(memcmp(buffer, "\0\0\0\0XY", 6) == 0, 1);
	EXPECT(ops->io(ctx, FS_ROOT, file, buffer, 1, -1, 0, 1), -EINVAL);
	EXPECT(ops->stat(ctx, NULL, file, 0, &stat), 0);
	EXPECT(stat.size, 12);
	EXPECT(stat.mode & 0777, 0640);
	EXPECT(ops->truncate(ctx, duplicate, 4), 0);
	EXPECT(ops->stat(ctx, "suite/data", 0, 0, &stat), 0);
	EXPECT(stat.size, 4);
	EXPECT(ops->truncate(ctx, duplicate, 8), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, buffer, 4, 4, 0, 1), 4);
	EXPECT(memcmp(buffer, "\0\0\0\0", 4) == 0, 1);
	EXPECT(ops->truncate(ctx, duplicate, 4), 0);
	EXPECT(ops->sync(ctx, file, 0), 0);
	EXPECT(ops->sync(ctx, duplicate, 1), 0);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/data", O_RDONLY, 0, 0, &other), 0);
	EXPECT(ops->io(ctx, FS_ROOT, other, "x", 1, 0, 1, 0), -EBADF);
	EXPECT(ops->truncate(ctx, other, 0), -EINVAL);
	EXPECT(ops->getdents(ctx, other, buffer, sizeof(buffer)), -ENOTDIR);
	EXPECT(ops->close(ctx, FS_ROOT, other), 0);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/data", O_PATH, 0, 0, &other), 0);
	EXPECT(ops->io(ctx, FS_ROOT, other, buffer, 1, 0, 0, 0), -EBADF);
	EXPECT(ops->seek(ctx, other, 0, SEEK_CUR), -EBADF);
	EXPECT(ops->getdents(ctx, other, buffer, sizeof(buffer)), -EBADF);
	EXPECT(ops->sync(ctx, other, 0), -EBADF);
	EXPECT(ops->truncate(ctx, other, 0), -EBADF);
	EXPECT(ops->stat(ctx, NULL, other, 0, &stat), 0);
	EXPECT(stat.size, 4);
	EXPECT(ops->close(ctx, FS_ROOT, other), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/data", NULL, 0), 0);
	EXPECT(ops->stat(ctx, "suite/data", 0, 0, &stat), -ENOENT);
	EXPECT(ops->stat(ctx, NULL, file, 0, &stat), 0);
	EXPECT(stat.nlink, 0);
	EXPECT(ops->io(ctx, FS_ROOT, duplicate, buffer, 4, 0, 0, 1), 4);
	EXPECT(memcmp(buffer, "abcd", 4) == 0, 1);
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	EXPECT(ops->close(ctx, FS_ROOT, duplicate), 0);
	EXPECT(ops->close(ctx, FS_ROOT, duplicate), -EBADF);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/append", O_CREAT | O_RDWR | O_APPEND,
			 0640, 0, &file), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, "A", 1, 0, 1, 0), 1);
	EXPECT(ops->io(ctx, FS_ROOT, file, "B", 1, 0, 1, 1), 1);
	EXPECT(ops->seek(ctx, file, 0, SEEK_CUR), 1);
	EXPECT(ops->io(ctx, FS_ROOT, file, buffer, 2, 0, 0, 1), 2);
	EXPECT(memcmp(buffer, "AB", 2) == 0, 1);
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/append", NULL, 0), 0);
	return 0;
}

static int fs_permissions(const struct fs_test_ops *ops, void *ctx,
			  struct fs_test_report *report)
{
	struct fs_test_stat stat;
	uint64_t file, group;
	char buffer[8];

	EXPECT(ops->open(ctx, FS_USER, "suite/user", O_CREAT | O_RDWR,
			 0640, 0, &file), 0);
	EXPECT(ops->io(ctx, FS_USER, file, "owner", 5, 0, 1, 0), 5);
	EXPECT(ops->stat(ctx, "suite/user", 0, 0, &stat), 0);
	EXPECT(stat.uid, 1001);
	EXPECT(stat.gid, 2001);
	EXPECT(ops->close(ctx, FS_USER, file), 0);
	EXPECT(ops->open(ctx, FS_OTHER, "suite/user", O_RDONLY, 0, 0, &file),
	       -EACCES);
	EXPECT(ops->open(ctx, FS_GROUP, "suite/user", O_WRONLY, 0, 0, &file),
	       -EACCES);
	EXPECT(ops->open(ctx, FS_GROUP, "suite/user", O_RDONLY, 0, 0, &group), 0);
	EXPECT(ops->io(ctx, FS_GROUP, group, buffer, 5, 0, 0, 0), 5);
	EXPECT(memcmp(buffer, "owner", 5) == 0, 1);
	EXPECT(ops->close(ctx, FS_GROUP, group), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_MKDIR, "suite/private", NULL,
			  0700), 0);
	EXPECT(ops->open(ctx, FS_USER, "suite/private/missing", O_RDONLY,
			 0, 0, &file), -EACCES);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_MKDIR, "suite/sticky", NULL,
			  01777), 0);
	EXPECT(ops->open(ctx, FS_USER, "suite/sticky/owned", O_CREAT | O_WRONLY,
			 0600, 0, &file), 0);
	EXPECT(ops->close(ctx, FS_USER, file), 0);
	EXPECT(ops->change(ctx, FS_OTHER, FS_TEST_UNLINK, "suite/sticky/owned",
			  NULL, 0), -EPERM);
	EXPECT(ops->change(ctx, FS_USER, FS_TEST_UNLINK, "suite/sticky/owned",
			  NULL, 0), 0);
	return 0;
}

static int fs_namespace(const struct fs_test_ops *ops, void *ctx,
			struct fs_test_report *report)
{
	struct fs_test_stat stat;
	uint64_t file;
	char buffer[32];

	EXPECT(ops->open(ctx, FS_ROOT, "suite/a", O_CREAT | O_RDWR, 0600, 0,
			 &file), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, "A", 1, 0, 1, 0), 1);
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/b", O_CREAT | O_RDWR, 0600, 0,
			 &file), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, "B", 1, 0, 1, 0), 1);
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_RENAME, "suite/a", "suite/b",
			  RENAME_NOREPLACE), -EEXIST);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_RENAME, "suite/a", "suite/b",
			  RENAME_EXCHANGE), 0);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/a", O_RDONLY, 0, 0, &file), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, buffer, 1, 0, 0, 0), 1);
	EXPECT(buffer[0], 'B');
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_RENAME, "suite/a", "suite/b",
			  0), 0);
	EXPECT(ops->stat(ctx, "suite/a", 0, 0, &stat), -ENOENT);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_SYMLINK, "/suite/b", "suite/link",
			  0), 0);
	EXPECT(ops->readlink(ctx, "suite/link", buffer, sizeof(buffer)), 8);
	EXPECT(memcmp(buffer, "/suite/b", 8) == 0, 1);
	EXPECT(ops->stat(ctx, "suite/link", 0, AT_SYMLINK_NOFOLLOW, &stat), 0);
	EXPECT(S_ISLNK(stat.mode), 1);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/link", O_RDONLY, 0,
			 RESOLVE_NO_SYMLINKS, &file), -ELOOP);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/link", O_RDONLY, 0, 0, &file), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, buffer, 1, 0, 0, 0), 1);
	EXPECT(buffer[0], 'B');
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	EXPECT(ops->open(ctx, FS_ROOT, "../../suite/b", O_RDONLY, 0,
			 RESOLVE_BENEATH, &file), -EXDEV);
	EXPECT(ops->open(ctx, FS_ROOT, "../suite/b", O_RDONLY, 0, 0, &file), 0);
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/b", O_RDONLY, 0, 1ULL << 63,
			 &file), -EINVAL);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/b", O_RDONLY, 0600, 0, &file),
	       -EINVAL);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/b/", NULL, 0),
	       -ENOTDIR);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/private", NULL, 0),
	       -EISDIR);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite", NULL,
			  AT_REMOVEDIR), -ENOTEMPTY);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/private", NULL,
			  AT_REMOVEDIR), 0);
	return 0;
}

static int fs_last_cookie(const char *buffer, size_t bytes, int64_t *cookie)
{
	uint16_t size;
	size_t offset = 0;

	while (offset < bytes) {
		if (bytes - offset < 24)
			return -EINVAL;
		memcpy(&size, buffer + offset + 16, sizeof(size));
		if (size < 24 || (size & 7) || size > bytes - offset)
			return -EINVAL;
		memcpy(cookie, buffer + offset + 8, sizeof(*cookie));
		offset += size;
	}
	return bytes ? 0 : -EINVAL;
}

static bool fs_equal_entries(const char *a, const char *b, size_t bytes)
{
	uint16_t size;
	size_t offset = 0;
	const char *end;

	while (offset < bytes) {
		if (bytes - offset < 24)
			return false;
		memcpy(&size, a + offset + 16, sizeof(size));
		if (size < 24 || size > bytes - offset)
			return false;
		end = memchr(a + offset + 19, 0, size - 19);
		if (!end || memcmp(a + offset, b + offset, end - a - offset + 1))
			return false;
		offset += size;
	}
	return true;
}

static int fs_directory(const struct fs_test_ops *ops, void *ctx,
			struct fs_test_report *report)
{
	/* Name limit and buffer/cookie checks use raw linux_dirent64 offsets. */
	char name[263] = "suite/";
	char buffer[280];
	char resumed[280];
	uint64_t file, dir;
	uint64_t ino;
	unsigned int found = 0, count = 0, used, size, bit, type;
	uint16_t record_size;
	int64_t result, cookie;

	memset(name + 6, 'n', 255);
	name[261] = 0;
	EXPECT(ops->open(ctx, FS_ROOT, name, O_CREAT | O_RDWR, 0600, 0, &file), 0);
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	name[261] = 'n';
	name[262] = 0;
	EXPECT(ops->open(ctx, FS_ROOT, name, O_CREAT | O_RDWR, 0600, 0, &file),
	       -ENAMETOOLONG);
	name[261] = 0;
	EXPECT(ops->open(ctx, FS_ROOT, "suite", O_RDONLY | O_DIRECTORY,
			 0, 0, &dir), 0);
	EXPECT(ops->getdents(ctx, dir, buffer, 1), -EINVAL);
	/* Enumeration order is unspecified (ext4 uses a per-FS hash seed),
	 * and a failed short-buffer call need not leave its position unchanged.
	 */
	EXPECT(ops->seek(ctx, dir, 0, SEEK_SET), 0);
	result = ops->getdents(ctx, dir, buffer, sizeof(buffer));
	EXPECT(result > 0 && (uint64_t)result <= sizeof(buffer), 1);
	EXPECT(fs_last_cookie(buffer, result, &cookie), 0);
	result = ops->getdents(ctx, dir, buffer, sizeof(buffer));
	EXPECT(result > 0 && (uint64_t)result <= sizeof(buffer), 1);
	EXPECT(ops->seek(ctx, dir, cookie, SEEK_SET) == cookie, 1);
	EXPECT(ops->getdents(ctx, dir, resumed, sizeof(resumed)) == result, 1);
	/* Native getdents leaves record padding in the caller's buffer alone. */
	EXPECT(fs_equal_entries(buffer, resumed, result), 1);
	EXPECT(ops->seek(ctx, dir, 0, SEEK_SET), 0);
	for (;;) {
		result = ops->getdents(ctx, dir, buffer, sizeof(buffer));
		if (result < 0 || (uint64_t)result > sizeof(buffer))
			return fs_expect(report, __LINE__, result, 0);
		if (!result)
			break;
		if (fs_last_cookie(buffer, result, &cookie))
			return fs_expect(report, __LINE__, -EINVAL, 0);
		for (used = 0; used < result; used += size) {
			memcpy(&record_size, buffer + used + 16, sizeof(record_size));
			size = record_size;
			EXPECT(size >= 24 && !(size & 7) && used + size <= result, 1);
			EXPECT(memchr(buffer + used + 19, 0, size - 19) != NULL, 1);
			memcpy(&ino, buffer + used, sizeof(ino));
			EXPECT(ino != 0, 1);
			bit = 0;
			type = 8;
			if (!strcmp(buffer + used + 19, ".")) {
				bit = 1;
				type = 4;
			} else if (!strcmp(buffer + used + 19, "..")) {
				bit = 2;
				type = 4;
			} else if (!strcmp(buffer + used + 19, name + 6)) {
				bit = 4;
			} else if (!strcmp(buffer + used + 19, "user")) {
				bit = 8;
			} else if (!strcmp(buffer + used + 19, "sticky")) {
				bit = 16;
				type = 4;
			} else if (!strcmp(buffer + used + 19, "b")) {
				bit = 32;
			} else if (!strcmp(buffer + used + 19, "link")) {
				bit = 64;
				type = 10;
			}
			EXPECT(bit && !(found & bit), 1);
			EXPECT((unsigned char)buffer[used + 18] == type, 1);
			found |= bit;
			count++;
			EXPECT(count <= 7, 1);
		}
	}
	EXPECT(found, 127);
	EXPECT(count, 7);
	EXPECT(ops->seek(ctx, dir, 0, SEEK_SET), 0);
	EXPECT(ops->getdents(ctx, dir, buffer, sizeof(buffer)) > 0, 1);
	EXPECT(ops->close(ctx, FS_ROOT, dir), 0);
	return 0;
}

static int fs_dirfd(const struct fs_test_ops *ops, void *ctx,
		    struct fs_test_report *report)
{
	struct fs_test_stat stat;
	uint64_t directory, file, opened;
	char buffer[8];

	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_MKDIR, "suite/at", NULL, 0777), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_MKDIR, "suite/at/nested", NULL, 0777), 0);
	EXPECT(ops->open(ctx, FS_ROOT, "suite/at", O_PATH | O_DIRECTORY, 0, 0,
			 &directory), 0);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "nested/file", O_CREAT | O_RDWR,
			   0777, 0, 0027, &file), 0);
	EXPECT(ops->stat(ctx, NULL, file, 0, &stat), 0);
	EXPECT(stat.mode & 0777, 0750);
	EXPECT(ops->io(ctx, FS_ROOT, file, "dirfd", 5, 0, 1, 0), 5);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "nested/file", O_RDONLY,
			   0, RESOLVE_BENEATH, 0, &opened), 0);
	EXPECT(ops->io(ctx, FS_ROOT, opened, buffer, 5, 0, 0, 0), 5);
	EXPECT(memcmp(buffer, "dirfd", 5) == 0, 1);
	EXPECT(ops->close(ctx, FS_ROOT, opened), 0);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "/nested/file", O_RDONLY,
			   0, RESOLVE_IN_ROOT, 0, &opened), 0);
	EXPECT(ops->close(ctx, FS_ROOT, opened), 0);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "/nested/file", O_RDONLY,
			   0, RESOLVE_BENEATH, 0, &opened), -EXDEV);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "../../suite/b", O_RDONLY,
			   0, RESOLVE_BENEATH, 0, &opened), -EXDEV);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "../../suite/b", O_RDONLY,
			   0, 0, 0, &opened), 0);
	EXPECT(ops->close(ctx, FS_ROOT, opened), 0);
	EXPECT(ops->openat(ctx, FS_ROOT, ~0ULL, "/suite/b", O_RDONLY,
			   0, 0, 0, &opened), 0);
	EXPECT(ops->close(ctx, FS_ROOT, opened), 0);
	EXPECT(ops->openat(ctx, FS_ROOT, ~0ULL, "/suite/b", O_RDONLY,
			   0, RESOLVE_IN_ROOT, 0, &opened), -EBADF);
	EXPECT(ops->openat(ctx, FS_ROOT, file, "relative", O_RDONLY,
			   0, 0, 0, &opened), -ENOTDIR);
	EXPECT(ops->openat(ctx, FS_ROOT, file, "/suite/b", O_RDONLY,
			   0, 0, 0, &opened), 0);
	EXPECT(ops->close(ctx, FS_ROOT, opened), 0);
	EXPECT(ops->openat(ctx, FS_OTHER, directory, "nested/file", O_RDONLY,
			   0, 0, 0, &opened), -EACCES);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_SYMLINK, "/suite/b", "suite/at/link", 0), 0);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "link", O_RDONLY,
			   0, 0, 0, &opened), 0);
	EXPECT(ops->close(ctx, FS_ROOT, opened), 0);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "link", O_RDONLY,
			   0, RESOLVE_NO_SYMLINKS, 0, &opened), -ELOOP);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "link", O_RDONLY,
			   0, RESOLVE_IN_ROOT, 0, &opened), -ENOENT);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "", O_RDONLY,
			   0, 0, 0, &opened), -ENOENT);
	EXPECT(ops->close(ctx, FS_ROOT, directory), 0);
	EXPECT(ops->openat(ctx, FS_ROOT, directory, "nested/file", O_RDONLY,
			   0, 0, 0, &opened), -EBADF);
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/at/link", NULL, 0), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/at/nested/file", NULL, 0), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/at/nested", NULL, AT_REMOVEDIR), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/at", NULL, AT_REMOVEDIR), 0);
	return 0;
}

static int fs_metadata(const struct fs_test_ops *ops, void *ctx,
		       struct fs_test_report *report)
{
	struct fs_test_stat stat;
	uint64_t file, duplicate;
	int64_t times[4] = {123456789, 123456789, 223456789, 987654321};
	char buffer[8], name[251];

	EXPECT(ops->open(ctx, FS_ROOT, "suite/meta", O_CREAT | O_EXCL | O_RDWR,
			 0600, 0, &file), 0);
	EXPECT(ops->metadata(ctx, FS_OTHER, FS_TEST_CHMOD, 0,
			    "suite/meta", 0666, 0, 0), -EPERM);
	EXPECT(ops->metadata(ctx, FS_ROOT, FS_TEST_CHOWN, 0,
			    "suite/meta", 1001, 2001, 0), 0);
	EXPECT(ops->metadata(ctx, FS_USER, FS_TEST_CHMOD, file,
			    "", 0640, 0, AT_EMPTY_PATH), 0);
	EXPECT(ops->metadata(ctx, FS_USER, FS_TEST_CHOWN, 0,
			    "suite/meta", 1003, 2003, 0), -EPERM);
	EXPECT(ops->metadata(ctx, FS_OTHER, FS_TEST_ACCESS, 0,
			    "suite/meta", 4, 0, 0), -EACCES);
	EXPECT(ops->metadata(ctx, FS_GROUP, FS_TEST_ACCESS, 0,
			    "suite/meta", 4, 0, AT_EACCESS), 0);
	EXPECT(ops->metadata(ctx, FS_GROUP, FS_TEST_ACCESS, 0,
			    "suite/meta", 2, 0, AT_EACCESS), -EACCES);
	EXPECT(ops->metadata(ctx, FS_ROOT, FS_TEST_ACCESS, file,
			    "", 4, 0, AT_EMPTY_PATH), 0);
	EXPECT(ops->metadata(ctx, FS_ROOT, FS_TEST_ACCESS, 0,
			    "suite/meta", 8, 0, 0), -EINVAL);
	EXPECT(ops->utimes(ctx, "suite/meta", times, 0), 0);
	EXPECT(ops->stat(ctx, "suite/meta", 0, 0, &stat), 0);
	EXPECT(stat.atime, times[0]);
	EXPECT(stat.atime_nsec, times[1]);
	EXPECT(stat.mtime, times[2]);
	EXPECT(stat.mtime_nsec, times[3]);
	times[1] = -1;
	EXPECT(ops->utimes(ctx, "suite/meta", times, 0), -EINVAL);
	times[1] = times[3] = UTIME_OMIT;
	EXPECT(ops->utimes(ctx, "suite/nonexistent", times, 0), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_LINK, "suite/meta", "suite/hard", 0), 0);
	EXPECT(ops->stat(ctx, "suite/hard", 0, 0, &stat), 0);
	EXPECT(stat.nlink, 2);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/hard", NULL, 0), 0);
	EXPECT(ops->fcntl(ctx, file, F_GETFL, 0) & O_ACCMODE, O_RDWR);
	EXPECT(ops->dup(ctx, FS_ROOT, file, &duplicate), 0);
	EXPECT(ops->fcntl(ctx, duplicate, F_SETFL, O_APPEND), 0);
	EXPECT(ops->fcntl(ctx, file, F_GETFL, 0) & O_APPEND, O_APPEND);
	EXPECT(ops->io(ctx, FS_ROOT, file, "abc", 3, 0, 1, 0), 3);
	EXPECT(ops->seek(ctx, duplicate, 0, SEEK_SET), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, "Z", 1, 0, 1, 0), 1);
	EXPECT(ops->seek(ctx, duplicate, 0, SEEK_CUR), 4);
	EXPECT(ops->io(ctx, FS_ROOT, file, buffer, 4, 0, 0, 1), 4);
	EXPECT(memcmp(buffer, "abcZ", 4) == 0, 1);
	EXPECT(ops->metadata(ctx, FS_ROOT, FS_TEST_SYNCFS, file, NULL, 0, 0, 0), 0);
	EXPECT(ops->close(ctx, FS_ROOT, duplicate), 0);
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/meta", NULL, 0), 0);
	EXPECT(ops->metadata(ctx, FS_ROOT, FS_TEST_MKNOD, 0,
			    "suite/fifo", S_IFIFO | 0600, 0, 0), 0);
	EXPECT(ops->stat(ctx, "suite/fifo", 0, 0, &stat), 0);
	EXPECT(stat.mode & S_IFMT, S_IFIFO);
	EXPECT(ops->metadata(ctx, FS_ROOT, FS_TEST_MKNOD, 0,
			    "suite/not-dir", S_IFDIR | 0600, 0, 0), -EPERM);
	EXPECT(ops->change(ctx, FS_ROOT, FS_TEST_UNLINK, "suite/fifo", NULL, 0), 0);
	memset(name, 'm', sizeof(name));
	name[128] = 0;
	EXPECT(ops->memfd(ctx, name, MFD_ALLOW_SEALING, &file), 0);
	EXPECT(ops->fcntl(ctx, file, F_GET_SEALS, 0), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, "abc", 3, 0, 1, 0), 3);
	EXPECT(ops->fcntl(ctx, file, F_ADD_SEALS, F_SEAL_SHRINK), 0);
	EXPECT(ops->truncate(ctx, file, 2), -EPERM);
	EXPECT(ops->fcntl(ctx, file, F_ADD_SEALS, F_SEAL_GROW), 0);
	EXPECT(ops->truncate(ctx, file, 4), -EPERM);
	EXPECT(ops->fcntl(ctx, file, F_ADD_SEALS, F_SEAL_WRITE), 0);
	EXPECT(ops->io(ctx, FS_ROOT, file, "x", 1, 0, 1, 0), -EPERM);
	EXPECT(ops->fcntl(ctx, file, F_GET_SEALS, 0),
	       F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
	EXPECT(ops->fcntl(ctx, file, F_ADD_SEALS, F_SEAL_SEAL), 0);
	EXPECT(ops->close(ctx, FS_ROOT, file), 0);
	name[128] = 'm';
	name[250] = 0;
	EXPECT(ops->memfd(ctx, name, MFD_ALLOW_SEALING, &file), -EINVAL);
	return 0;
}

int kobox_fs_workload(const struct fs_test_ops *ops, void *ctx,
		      struct fs_test_report *report)
{
	int error;

	report->digest = 14695981039346656037ULL;
	error = fs_basic(ops, ctx, report);
	if (!error)
		error = fs_permissions(ops, ctx, report);
	if (!error)
		error = fs_namespace(ops, ctx, report);
	if (!error)
		error = fs_dirfd(ops, ctx, report);
	if (!error)
		error = fs_metadata(ops, ctx, report);
	if (!error)
		error = fs_directory(ops, ctx, report);
	return error;
}
