// SPDX-License-Identifier: GPL-2.0-only
#include "fs_mount.h"
#include "fs_service.h"
#include "fs_bench.h"

#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs_struct.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/nsproxy.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/sched/task.h>
#include <linux/task_work.h>
#include <uapi/linux/mount.h>
#include "../../fs/internal.h"
#include "../../fs/ext4/ext4.h"

#define FS_ROOT_NODE "/kobox-storage-device"
#define FS_ROOT_PATH "/kobox-storage-root"

struct kobox_linux_fs_mount {
	struct path root;
	struct kobox_linux_fs_service *service;
	unsigned int submounts;
};

static const char * const fs_tmpfs_paths[] = {
	"/tmp", "/run", "/dev/shm",
};

static bool fs_disk_allowed(struct gendisk *disk,
			    struct pci_dev * const *devices, size_t count)
{
	struct device *dev;

	for (dev = disk_to_dev(disk)->parent; dev; dev = dev->parent)
		for (size_t i = 0; i < count; i++)
			if (dev == &devices[i]->dev)
				return true;
	return false;
}

static int fs_probe_uuid(dev_t dev, const u8 uuid[16], u64 *diskseq)
{
	struct ext4_super_block *super;
	struct file *file;
	loff_t offset = EXT4_MIN_BLOCK_SIZE;
	ssize_t read;
	int result = 0;

	file = bdev_file_open_by_dev(dev, BLK_OPEN_READ, NULL, NULL);
	if (IS_ERR(file))
		return PTR_ERR(file);
	if (bdev_nr_bytes(file_bdev(file)) < offset + sizeof(*super))
		goto out_file;
	super = kmalloc(sizeof(*super), GFP_KERNEL);
	if (!super) {
		result = -ENOMEM;
		goto out_file;
	}
	/* Use upstream's ext4 representation at its format-defined superblock
	 * location; this is a read-only identification probe, not a second VFS. */
	read = kernel_read(file, super, sizeof(*super), &offset);
	if (read < 0)
		result = read;
	else if (read != sizeof(*super))
		result = -EIO;
	else if (le16_to_cpu(super->s_magic) == EXT4_SUPER_MAGIC &&
		 !memcmp(super->s_uuid, uuid, sizeof(super->s_uuid))) {
		*diskseq = file_bdev(file)->bd_disk->diskseq;
		result = 1;
	}
	kfree(super);
out_file:
	fput(file);
	return result;
}

static int fs_select_root(struct pci_dev * const *devices, size_t device_count,
			  const u8 uuid[16], dev_t *selected)
{
	struct class_dev_iter iter;
	struct device *dev;
	dev_t found = 0;
	u64 diskseq = 0;
	int result = -ENODEV;

	class_dev_iter_init(&iter, &block_class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter))) {
		struct block_device *bdev;
		int match;

		if (dev->type != &disk_type && dev->type != &part_type)
			continue;
		bdev = dev_to_bdev(dev);
		if (!disk_live(bdev->bd_disk) || !bdev_nr_bytes(bdev) ||
		    (bdev->bd_disk->flags & GENHD_FL_HIDDEN) ||
		    !fs_disk_allowed(bdev->bd_disk, devices, device_count))
			continue;
		match = fs_probe_uuid(bdev->bd_dev, uuid, &diskseq);
		if (match < 0) {
			result = match;
			goto out;
		}
		if (!match)
			continue;
		if (found) {
			result = -ENOTUNIQ;
			goto out;
		}
		found = bdev->bd_dev;
	}
	if (found) {
		u64 current_seq = 0;
		int match = fs_probe_uuid(found, uuid, &current_seq);

		if (match != 1 || current_seq != diskseq)
			result = match < 0 ? match : -ESTALE;
		else {
			*selected = found;
			result = 0;
		}
	}
out:
	class_dev_iter_exit(&iter);
	return result;
}

static int fs_make_node(const char *name, umode_t mode, dev_t device)
{
	struct path parent;
	struct dentry *entry;
	int result;

	entry = start_creating_path(AT_FDCWD, name, &parent, 0);
	if (IS_ERR(entry))
		return PTR_ERR(entry);
	if (S_ISDIR(mode)) {
		struct dentry *created = vfs_mkdir(mnt_idmap(parent.mnt),
						 d_inode(parent.dentry), entry, mode);

		result = PTR_ERR_OR_ZERO(created);
		entry = IS_ERR(created) ? NULL : created;
	} else
		result = vfs_mknod(mnt_idmap(parent.mnt), d_inode(parent.dentry),
				   entry, mode, device);
	end_creating_path(&parent, entry);
	return result;
}

static struct fs_struct *fs_enter_root(const struct path *root)
{
	struct fs_struct *old = current->fs;
	struct fs_struct *private = copy_fs_struct(old ?: init_task.fs);

	if (!private)
		return ERR_PTR(-ENOMEM);
	set_fs_root(private, root);
	set_fs_pwd(private, root);
	task_lock(current);
	current->fs = private;
	task_unlock(current);
	return old;
}

static void fs_leave_root(struct fs_struct *old)
{
	struct fs_struct *private = current->fs;

	task_lock(current);
	current->fs = old;
	task_unlock(current);
	free_fs_struct(private);
}

static int fs_unmount_path(const char *name)
{
	struct path path;
	int result = kern_path(name, LOOKUP_FOLLOW, &path);

	if (result)
		return result;
	/* path_umount consumes both path references, also on failure. */
	return path_umount(&path, 0);
}

int kobox_linux_fs_mount_open(struct kobox_linux_fs_mount **out,
	const struct kobox_linux_fs_mount_config *config,
	struct pci_dev * const *devices, size_t device_count)
{
	struct kobox_linux_fs_mount *mount;
	struct fs_struct *old;
	struct path point;
	dev_t selected;
	int result;
	unsigned int i;

	if (!out || !config || config->size != sizeof(*config) ||
	    !config->generation || !devices || !device_count ||
	    !memchr_inv(config->root_uuid, 0, 16))
		return -EINVAL;
	for (size_t j = 0; j < device_count; j++)
		if (!devices[j])
			return -EINVAL;
	*out = NULL;
	result = fs_select_root(devices, device_count, config->root_uuid, &selected);
	if (result)
		return result;
	mount = kzalloc(sizeof(*mount), GFP_KERNEL);
	if (!mount)
		return -ENOMEM;
	result = fs_make_node(FS_ROOT_NODE, S_IFBLK | 0600, selected);
	if (!result)
		result = fs_make_node(FS_ROOT_PATH, S_IFDIR | 0700, 0);
	if (!result)
		result = kern_path(FS_ROOT_PATH, LOOKUP_DIRECTORY, &point);
	if (result)
		goto free_mount;
	result = path_mount(FS_ROOT_NODE, &point, "ext4", MS_NOATIME, NULL);
	path_put(&point);
	if (result)
		goto free_mount;
	result = kern_path(FS_ROOT_PATH, LOOKUP_DIRECTORY, &mount->root);
	if (result)
		goto unmount_root;
	old = fs_enter_root(&mount->root);
	if (IS_ERR(old)) {
		result = PTR_ERR(old);
		goto put_root;
	}
	/* Runtime tmpfs semantics and accounting belong to upstream. The root
	 * image only supplies the namespace directories, never copied contents. */
	result = fs_make_node("/dev", S_IFDIR | 0755, 0);
	if (result == -EEXIST)
		result = 0;
	for (i = 0; !result && i < ARRAY_SIZE(fs_tmpfs_paths); i++) {
		result = fs_make_node(fs_tmpfs_paths[i], S_IFDIR | 01777, 0);
		if (result == -EEXIST)
			result = 0;
		if (!result)
			result = kern_path(fs_tmpfs_paths[i], LOOKUP_DIRECTORY, &point);
		if (result)
			break;
		result = path_mount(NULL, &point, "tmpfs", MS_NOSUID | MS_NODEV, NULL);
		path_put(&point);
		if (!result)
			mount->submounts++;
	}
	if (!result)
		result = kobox_linux_fs_benchmark(mount->root.mnt);
	if (!result) {
		mount->service = kobox_linux_fs_service_create(mount->root.mnt,
							     config->generation);
		if (IS_ERR(mount->service)) {
			result = PTR_ERR(mount->service);
			mount->service = NULL;
		}
	}
	fs_leave_root(old);
	if (!result) {
		*out = mount;
		return 0;
	}
	/* Never unload drivers while any failed-to-unmount filesystem survives.
	 * Return retained ownership so the caller retires the entire process. */
	*out = mount;
	return result;
put_root:
	path_put(&mount->root);
	memset(&mount->root, 0, sizeof(mount->root));
unmount_root:
	if (fs_unmount_path(FS_ROOT_PATH)) {
		*out = mount;
		return result;
	}
free_mount:
	kfree(mount);
	return result;
}

struct kobox_linux_fs_service *
kobox_linux_fs_mount_service(struct kobox_linux_fs_mount *mount)
{
	return mount ? mount->service : NULL;
}

int kobox_linux_fs_mount_close(struct kobox_linux_fs_mount *mount)
{
	struct fs_struct *old;
	int result;

	if (!mount)
		return 0;
	if (mount->service) {
		kobox_linux_fs_service_destroy(mount->service);
		mount->service = NULL;
	}
	/* PID 1 need not return to userspace after closing service files. Run
	 * upstream deferred fputs before unmount instead of misreporting EBUSY
	 * or letting a driver's last references outlive its unload boundary. */
	task_work_run();
	flush_delayed_fput();
	if (mount->root.mnt) {
		old = fs_enter_root(&mount->root);
		if (IS_ERR(old))
			return PTR_ERR(old);
		while (mount->submounts) {
			result = fs_unmount_path(fs_tmpfs_paths[mount->submounts - 1]);
			if (result) {
				fs_leave_root(old);
				return result;
			}
			mount->submounts--;
		}
		fs_leave_root(old);
		path_put(&mount->root);
		memset(&mount->root, 0, sizeof(mount->root));
	}
	result = fs_unmount_path(FS_ROOT_PATH);
	if (result)
		return result;
	/* Non-internal mntput queues __cleanup_mnt on PID 1's task_work.
	 * This hosted task never returns to userspace between unmount and
	 * delete_module; finish superblock/filesystem references here instead
	 * of making the immediately following ext4 unload fail with EAGAIN.
	 */
	task_work_run();
	kfree(mount);
	return 0;
}
