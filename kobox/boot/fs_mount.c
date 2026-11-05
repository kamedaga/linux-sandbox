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

#define FS_ROOT_NODE "/kobox-storage-device"
#define FS_ROOT_PATH "/kobox-storage-root"

struct kobox_linux_fs_mount {
	struct path root;
	struct kobox_linux_fs_service *service;
	unsigned int submounts;
};

static const struct {
	const char *path;
	const char *options;
} fs_tmpfs_paths[] = {
	{ "/tmp", NULL }, { "/run", NULL }, { "/dev/shm", NULL },
	/* /sys and DRM nodes describe live device grants. Keeping any of them on
	 * ext4 strands a dead publisher after a host stop/crash, and the next
	 * legitimate publisher must reject those preexisting entries. As on
	 * Linux, all of /sys is a per-boot namespace, so a publisher adding a
	 * new index never needs an entry here; inputd/gpud republish the actual
	 * devices without replacing another live owner's metadata. /dev keeps
	 * the root image's static nodes, so only the published DRM directory is
	 * fresh there. */
	{ "/sys", "mode=0755" },
	{ "/dev/dri", "mode=0755" },
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

static bool fs_partuuid_matches(struct block_device *bdev, const u8 partuuid[16])
{
	guid_t parsed;

	/* The upstream GPT scanner has already validated and decoded this GUID.
	 * guid_parse uses the same mixed-endian representation as the boot policy;
	 * MBR IDs and whole disks must not be accepted as GPT partitions. */
	return bdev_is_partition(bdev) && bdev->bd_meta_info &&
		!guid_parse(bdev->bd_meta_info->uuid, &parsed) &&
		!memcmp(&parsed, partuuid, sizeof(parsed));
}

static int fs_select_root(struct pci_dev * const *devices, size_t device_count,
			  const u8 partuuid[16], struct file **selected)
{
	struct class_dev_iter iter;
	struct device *dev;
	struct block_device *found = NULL;
	struct file *file;
	u64 diskseq = 0;
	sector_t start = 0, sectors = 0;
	int result = -ENODEV;

	*selected = NULL;
	class_dev_iter_init(&iter, &block_class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter))) {
		struct block_device *bdev;

		if (dev->type != &part_type)
			continue;
		bdev = dev_to_bdev(dev);
		if (!disk_live(bdev->bd_disk) || !bdev_nr_bytes(bdev) ||
		    (bdev->bd_disk->flags & GENHD_FL_HIDDEN) ||
		    !fs_disk_allowed(bdev->bd_disk, devices, device_count) ||
		    !fs_partuuid_matches(bdev, partuuid))
			continue;
		if (found) {
			result = -ENOTUNIQ;
			goto out;
		}
		/* Keep the original device alive so reuse of its dev_t cannot pass
		 * the final open. Unrelated candidates must not overwrite diskseq. */
		get_device(dev);
		found = bdev;
		diskseq = bdev->bd_disk->diskseq;
		start = get_start_sect(bdev);
		sectors = bdev_nr_sectors(bdev);
	}
	if (!found)
		goto out;
	file = bdev_file_open_by_dev(found->bd_dev, BLK_OPEN_READ, NULL, NULL);
	if (IS_ERR(file)) {
		result = PTR_ERR(file);
		goto out;
	}
	if (file_bdev(file) != found || !disk_live(found->bd_disk) ||
	    (found->bd_disk->flags & GENHD_FL_HIDDEN) ||
	    !fs_disk_allowed(found->bd_disk, devices, device_count) ||
	    found->bd_disk->diskseq != diskseq || get_start_sect(found) != start ||
	    bdev_nr_sectors(found) != sectors ||
	    !fs_partuuid_matches(found, partuuid)) {
		fput(file);
		result = -ESTALE;
	} else {
		/* Hold the partition open through mount: a partition rescan must
		 * not replace the validated mapping between selection and mount. */
		*selected = file;
		result = 0;
	}
out:
	class_dev_iter_exit(&iter);
	if (found)
		put_device(&found->bd_device);
	return result;
}

static bool fs_root_fstype_valid(const char type[8])
{
	return !memcmp(type, "ext4\0\0\0", 8) ||
		!memcmp(type, "xfs\0\0\0\0", 8) ||
		!memcmp(type, "btrfs\0\0", 8);
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
	struct file *selected = NULL;
	int result;
	unsigned int i;

	if (!out || !config || config->size != sizeof(*config) ||
	    !config->generation || config->root_kind > KOBOX_FS_ROOT_TMPFS ||
	    (config->root_kind == KOBOX_FS_ROOT_DISK ?
	     (!devices || !device_count || !memchr_inv(config->root_partuuid, 0, 16) ||
	      !memchr_inv(config->root_uuid, 0, 16) ||
	      !fs_root_fstype_valid(config->root_fstype)) :
	     (devices || device_count || memchr_inv(config->root_partuuid, 0, 16) ||
	      memchr_inv(config->root_uuid, 0, 16) ||
	      memchr_inv(config->root_fstype, 0, sizeof(config->root_fstype)))))
		return -EINVAL;
	for (size_t j = 0; j < device_count; j++)
		if (!devices[j])
			return -EINVAL;
	*out = NULL;
	result = config->root_kind == KOBOX_FS_ROOT_DISK ?
		fs_select_root(devices, device_count, config->root_partuuid, &selected) : 0;
	if (result)
		return result;
	mount = kzalloc(sizeof(*mount), GFP_KERNEL);
	if (!mount) {
		if (selected)
			fput(selected);
		return -ENOMEM;
	}
	result = config->root_kind == KOBOX_FS_ROOT_DISK ?
		fs_make_node(FS_ROOT_NODE, S_IFBLK | 0600, file_bdev(selected)->bd_dev) : 0;
	if (!result)
		result = fs_make_node(FS_ROOT_PATH, S_IFDIR | 0700, 0);
	if (!result)
		result = kern_path(FS_ROOT_PATH, LOOKUP_DIRECTORY, &point);
	if (result)
		goto free_mount;
	if (config->root_kind == KOBOX_FS_ROOT_TMPFS) {
		/* The installer imports a verified archive and stages its selected
		 * payload here. A RAM root has no PCI authority and no persistent
		 * namespace requiring additional tmpfs mounts. */
		char *options = (char *)get_zeroed_page(GFP_KERNEL);
		if (!options)
			result = -ENOMEM;
		else {
			strscpy(options, "size=75%,mode=0755", PAGE_SIZE);
			result = path_mount(NULL, &point, "tmpfs", MS_NOSUID | MS_NODEV, options);
			free_page((unsigned long)options);
		}
	} else
		result = path_mount(FS_ROOT_NODE, &point, config->root_fstype, MS_NOATIME, NULL);
	path_put(&point);
	if (selected) {
		fput(selected);
		selected = NULL;
	}
	if (result)
		goto free_mount;
	result = kern_path(FS_ROOT_PATH, LOOKUP_DIRECTORY, &mount->root);
	if (result)
		goto unmount_root;
	/* PARTUUID selects the partition independently of its contents. FileD's
	 * executable cache still uses the FS UUID, so reject stale boot policy
	 * after a reformat before exposing any files under that cache identity. */
	if (config->root_kind == KOBOX_FS_ROOT_DISK &&
	    memcmp(mount->root.dentry->d_sb->s_uuid.b, config->root_uuid, 16)) {
		result = -ESTALE;
		goto put_root;
	}
	old = fs_enter_root(&mount->root);
	if (IS_ERR(old)) {
		result = PTR_ERR(old);
		goto put_root;
	}
	/* Runtime tmpfs semantics and accounting belong to upstream. The root
	 * image only supplies the namespace directories, never copied contents. */
	const char * const parents[] = { "/dev" };
	for (i = 0; !result && i < ARRAY_SIZE(parents); i++) {
		result = fs_make_node(parents[i], S_IFDIR | 0755, 0);
		if (result == -EEXIST)
			result = 0;
	}
	for (i = 0; !result && config->root_kind == KOBOX_FS_ROOT_DISK &&
	     i < ARRAY_SIZE(fs_tmpfs_paths); i++) {
		char *options;

		result = fs_make_node(fs_tmpfs_paths[i].path, S_IFDIR | 01777, 0);
		if (result == -EEXIST)
			result = 0;
		if (!result)
			result = kern_path(fs_tmpfs_paths[i].path, LOOKUP_DIRECTORY, &point);
		if (result)
			break;
		/* path_mount always terminates data_page[PAGE_SIZE - 1], even for
		 * a short option string. Supply its full writable page, as the
		 * upstream mount syscall does; kstrdup would corrupt another slab. */
		options = fs_tmpfs_paths[i].options ?
			(char *)get_zeroed_page(GFP_KERNEL) : NULL;
		if (fs_tmpfs_paths[i].options && !options) {
			path_put(&point);
			result = -ENOMEM;
			break;
		}
		if (options)
			strscpy(options, fs_tmpfs_paths[i].options, PAGE_SIZE);
		result = path_mount(NULL, &point, "tmpfs", MS_NOSUID | MS_NODEV,
				    options);
		if (options)
			free_page((unsigned long)options);
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
	if (selected)
		fput(selected);
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
		result = kobox_linux_fs_service_shared_close(mount->service);
		if (result)
			return result;
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
			result = fs_unmount_path(fs_tmpfs_paths[mount->submounts - 1].path);
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
