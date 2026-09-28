// SPDX-License-Identifier: GPL-2.0-only
#include "firmware_files.h"

#include <crypto/sha2.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#define KOBOX_FIRMWARE_NAME_MAX 127U
#define KOBOX_FIRMWARE_FILE_MAX (8U << 20)
#define KOBOX_FIRMWARE_TOTAL_MAX (64U << 20)

struct kobox_linux_firmware_store {
	size_t count;
	char names[KOBOX_FIRMWARE_MAX_FILES][KOBOX_FIRMWARE_NAME_MAX + 1];
};

static bool valid_name(const char *name)
{
	size_t len, start = 0;

	if (!name)
		return false;
	len = strnlen(name, KOBOX_FIRMWARE_NAME_MAX + 1);
	if (!len || len > KOBOX_FIRMWARE_NAME_MAX)
		return false;
	for (size_t i = 0; i <= len; i++) {
		const char c = name[i];

		if (c == '/' || c == '\0') {
			const size_t part = i - start;

			if (!part || (part == 1 && name[start] == '.') ||
			    (part == 2 && name[start] == '.' &&
			     name[start + 1] == '.'))
				return false;
			start = i + 1;
			continue;
		}
		if (!((c >= 'a' && c <= 'z') ||
		      (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
			return false;
	}
	return true;
}

static int ensure_directory(const char *name)
{
	struct path existing, parent;
	struct dentry *dentry;
	int result;

	result = kern_path(name, LOOKUP_DIRECTORY, &existing);
	if (!result) {
		path_put(&existing);
		return 0;
	}
	if (result != -ENOENT)
		return result;
	dentry = start_creating_path(AT_FDCWD, name, &parent,
				     LOOKUP_DIRECTORY);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);
	dentry = vfs_mkdir(mnt_idmap(parent.mnt), d_inode(parent.dentry),
			    dentry, 0755);
	result = IS_ERR(dentry) ? PTR_ERR(dentry) : 0;
	end_creating_path(&parent, dentry);
	return result;
}

static int ensure_parents(const char *name)
{
	char path[sizeof("/lib/firmware/") + KOBOX_FIRMWARE_NAME_MAX];
	size_t base;
	int result;

	result = ensure_directory("/lib");
	if (result)
		return result;
	result = ensure_directory("/lib/firmware");
	if (result)
		return result;
	strscpy(path, "/lib/firmware/", sizeof(path));
	base = strlen(path);
	strscpy(path + base, name, sizeof(path) - base);
	for (size_t i = base; path[i]; i++) {
		if (path[i] != '/')
			continue;
		path[i] = '\0';
		result = ensure_directory(path);
		path[i] = '/';
		if (result)
			return result;
	}
	return 0;
}

static int remove_file(const char *name)
{
	char path[sizeof("/lib/firmware/") + KOBOX_FIRMWARE_NAME_MAX];
	struct path parent;
	struct dentry *dentry;
	int result;

	snprintf(path, sizeof(path), "/lib/firmware/%s", name);
	dentry = start_removing_path(path, &parent);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);
	result = vfs_unlink(mnt_idmap(parent.mnt), d_inode(parent.dentry),
			    dentry, NULL);
	end_removing_path(&parent, dentry);
	return result;
}

int kobox_linux_firmware_install(const struct kobox_linux_firmware_file *files,
				 size_t count,
				 struct kobox_linux_firmware_store **out)
{
	struct kobox_linux_firmware_store *store;
	size_t total = 0;
	int result;

	if (!out || *out || (!files && count) ||
	    count > KOBOX_FIRMWARE_MAX_FILES)
		return -EINVAL;
	if (!count)
		return 0;
	for (size_t i = 0; i < count; i++) {
		u8 digest[32];

		if (!valid_name(files[i].name) || !files[i].data ||
		    !files[i].size || files[i].size > KOBOX_FIRMWARE_FILE_MAX ||
		    files[i].size > KOBOX_FIRMWARE_TOTAL_MAX - total)
			return -EINVAL;
		for (size_t j = 0; j < i; j++)
			if (!strcmp(files[i].name, files[j].name))
				return -EEXIST;
		sha256(files[i].data, files[i].size, digest);
		if (memcmp(digest, files[i].sha256, sizeof(digest))) {
			pr_err("kobox-firmware: name=%s status=%d sha256=%*phN\n",
			       files[i].name, -EBADMSG, 32, digest);
			return -EBADMSG;
		}
		total += files[i].size;
	}
	store = kzalloc(sizeof(*store), GFP_KERNEL);
	if (!store)
		return -ENOMEM;
	for (size_t i = 0; i < count; i++) {
		char path[sizeof("/lib/firmware/") + KOBOX_FIRMWARE_NAME_MAX];
		struct file *file;
		loff_t position = 0;

		result = ensure_parents(files[i].name);
		if (result)
			goto rollback;
		pr_info("kobox-firmware: staged=%s status=0 size=%zu sha256=%*phN\n",
			files[i].name, files[i].size, 32, files[i].sha256);
		snprintf(path, sizeof(path), "/lib/firmware/%s", files[i].name);
		file = filp_open(path, O_CREAT | O_EXCL | O_WRONLY, 0444);
		if (IS_ERR(file)) {
			result = PTR_ERR(file);
			goto rollback;
		}
		strscpy(store->names[store->count], files[i].name,
			sizeof(store->names[store->count]));
		store->count++;
		while ((size_t)position < files[i].size) {
			ssize_t written = kernel_write(file,
				(const char *)files[i].data + position,
				files[i].size - (size_t)position, &position);

			if (written <= 0) {
				result = written < 0 ? written : -EIO;
				break;
			}
		}
		/* fput() can defer the final close until task_work. The Linux
		 * firmware loader rejects a file with an outstanding writer as
		 * ETXTBSY, so complete the close before any module can request it. */
		__fput_sync(file);
		if (result)
			goto rollback;
	}
	*out = store;
	return 0;
rollback:
	pr_err("kobox-firmware: stage failed status=%d created=%zu\n",
	       result, store->count);
	if (kobox_linux_firmware_remove(&store))
		return -EIO;
	return result;
}

int kobox_linux_firmware_remove(struct kobox_linux_firmware_store **store)
{
	if (!store || !*store)
		return 0;
	for (size_t i = (*store)->count; i; i--) {
		int result = remove_file((*store)->names[i - 1]);

		if (result)
			return result;
		pr_info("kobox-firmware: removed=%s status=0\n",
			(*store)->names[i - 1]);
		(*store)->count--;
	}
	kfree(*store);
	*store = NULL;
	return 0;
}
