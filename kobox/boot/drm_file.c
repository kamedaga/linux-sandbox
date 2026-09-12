// SPDX-License-Identifier: GPL-2.0-only

#include "drm_file.h"

#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <asm/ptrace.h>

long __x64_sys_ioctl(const struct pt_regs *regs);

struct kobox_linux_drm_file {
	struct vfsmount *mount;
	struct file *guard;
	struct task_struct *owner;
	struct files_struct *files;
	int fd;
};

static bool valid_context(void)
{
	return !in_interrupt() && !irqs_disabled() && !current->mm &&
	       ((current->flags & PF_KTHREAD) || current->pid == 1) &&
	       current->files;
}

static int check_owner(struct kobox_linux_drm_file *file)
{
	struct file *installed;
	bool matches;

	if (!file || !valid_context() || current != file->owner ||
	    current->files != file->files)
		return -EPERM;
	installed = fget(file->fd);
	matches = installed && installed == file->guard;
	if (installed)
		fput(installed);
	return matches ? 0 : -EBADF;
}

static int create_node(struct vfsmount *mount, dev_t render)
{
	struct inode *parent = d_inode(mount->mnt_root);
	struct qstr name = QSTR_INIT("render", 6);
	struct dentry *node;
	int result;

	result = mnt_want_write(mount);
	if (result)
		return result;
	inode_lock(parent);
	node = lookup_one(mnt_idmap(mount), &name, mount->mnt_root);
	if (IS_ERR(node)) {
		result = PTR_ERR(node);
	} else {
		result = vfs_mknod(mnt_idmap(mount), parent, node,
				   S_IFCHR | 0600, render);
		dput(node);
	}
	inode_unlock(parent);
	mnt_drop_write(mount);
	return result;
}

int kobox_linux_drm_distinct(struct kobox_linux_drm_file *left,
			    struct kobox_linux_drm_file *right)
{
	int result = check_owner(left);

	if (!result)
		result = check_owner(right);
	if (result)
		return result;
	if (left == right || left->fd == right->fd || left->guard == right->guard ||
	    left->guard->private_data == right->guard->private_data)
		return -EUCLEAN;
	return 0;
}

int kobox_linux_drm_open(dev_t render, struct kobox_linux_drm_file **out)
{
	struct kobox_linux_drm_file *file;
	struct file_system_type *type;
	struct file *opened;
	int result;

	if (!out || *out || MAJOR(render) != DRM_MAJOR)
		return -EINVAL;
	if (!valid_context())
		return -EPERM;
	file = kzalloc(sizeof(*file), GFP_KERNEL);
	if (!file)
		return -ENOMEM;
	type = get_fs_type("tmpfs");
	if (!type) {
		result = -ENODEV;
		goto free_file;
	}
	file->mount = kern_mount(type);
	put_filesystem(type);
	if (IS_ERR(file->mount)) {
		result = PTR_ERR(file->mount);
		goto free_file;
	}
	result = create_node(file->mount, render);
	if (result)
		goto unmount;
	opened = file_open_root_mnt(file->mount, "render", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(opened)) {
		result = PTR_ERR(opened);
		goto unmount;
	}
	/* The devt is trusted; still verify the actual DRM file before publishing. */
	if (!opened->private_data ||
	    !drm_is_render_client(opened->private_data)) {
		result = -EACCES;
		goto close_opened;
	}
	result = get_unused_fd_flags(O_CLOEXEC);
	if (result < 0)
		goto close_opened;
	file->fd = result;
	file->guard = get_file(opened);
	file->owner = current;
	get_task_struct(file->owner);
	file->files = current->files;
	fd_install(file->fd, opened);
	*out = file;
	return 0;

close_opened:
	__fput_sync(opened);
unmount:
	kern_unmount(file->mount);
free_file:
	kfree(file);
	return result;
}

/* Only the two typed callers below can select commands and construct args.
 * Native uaccess accepts these private buffers; it does not validate peers.
 */
static int private_ioctl(struct kobox_linux_drm_file *file, unsigned int cmd,
			 void *argument)
{
	struct pt_regs regs = {
		.di = file->fd,
		.si = cmd,
		.dx = (unsigned long)argument,
	};

	return __x64_sys_ioctl(&regs);
}

int kobox_linux_drm_version(struct kobox_linux_drm_file *file,
			    const size_t capacity[3],
			    struct kobox_linux_drm_version *out)
{
	struct kobox_linux_drm_version *result;
	struct drm_version argument = {0};
	int error;

	if (!capacity || !out || capacity[0] > sizeof(out->name) ||
	    capacity[1] > sizeof(out->date) ||
	    capacity[2] > sizeof(out->description))
		return -EINVAL;
	error = check_owner(file);
	if (error)
		return error;
	result = kzalloc(sizeof(*result), GFP_KERNEL);
	if (!result)
		return -ENOMEM;
	argument.name_len = capacity[0];
	argument.date_len = capacity[1];
	argument.desc_len = capacity[2];
	argument.name = capacity[0] ? result->name : NULL;
	argument.date = capacity[1] ? result->date : NULL;
	argument.desc = capacity[2] ? result->description : NULL;
	error = private_ioctl(file, DRM_IOCTL_VERSION, &argument);
	if (!error) {
		result->major = argument.version_major;
		result->minor = argument.version_minor;
		result->patchlevel = argument.version_patchlevel;
		result->name_length = argument.name_len;
		result->date_length = argument.date_len;
		result->description_length = argument.desc_len;
		*out = *result;
	}
	kfree(result);
	return error;
}

int kobox_linux_drm_get_cap(struct kobox_linux_drm_file *file, u64 capability,
			   u64 *value)
{
	struct drm_get_cap argument = {.capability = capability};
	int result;

	if (!value)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	result = private_ioctl(file, DRM_IOCTL_GET_CAP, &argument);
	if (!result)
		*value = argument.value;
	return result;
}

int kobox_linux_drm_close(struct kobox_linux_drm_file **owner)
{
	struct kobox_linux_drm_file *file;
	int result;

	if (!owner || !*owner)
		return -EINVAL;
	file = *owner;
	result = check_owner(file);
	if (result)
		return result;
	/* No other task or raw FD consumer can enter an ioctl. The guard keeps
	 * close_fd from deferring the last put to task_work after module unload.
	 * A flush error still consumes the installed FD; never retry close_fd.
	 */
	result = close_fd(file->fd);
	__fput_sync(file->guard);
	kern_unmount(file->mount);
	put_task_struct(file->owner);
	kfree(file);
	*owner = NULL;
	return result;
}
