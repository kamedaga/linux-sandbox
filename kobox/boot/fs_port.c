// SPDX-License-Identifier: GPL-2.0-only

#include "fs_port_internal.h"

#include <linux/dirent.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fs_struct.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uio.h>
#include <linux/sched/xacct.h>
#include <linux/lockref.h>
#include <linux/workqueue.h>
#include <linux/fsnotify.h>
#include <linux/unaligned.h>
#include <linux/xarray.h>
#include <linux/sched/signal.h>
#include <asm/syscall.h>
#include <asm/unistd.h>

/* The pinned VFS owns openat2 validation; do not duplicate its flag rules. */
#include "../../fs/internal.h"
#include "../../fs/mount.h"

struct kobox_linux_fs_table {
	struct kobox_linux_fs_port owner;
	struct path root;
	struct xarray files;
	struct mutex lock;
	unsigned long next;
};

struct fs_parent {
	struct filename *name;
	struct path path;
	struct qstr last;
	int type;
};

struct fs_dirents {
	struct dir_context ctx;
	char *buffer;
	unsigned int used;
	unsigned int previous;
	int error;
};

struct kobox_linux_fs_port *kobox_linux_fs_create(struct vfsmount *mnt)
{
	struct kobox_linux_fs_port *port;
	struct kobox_linux_fs_table *table;

	if (!mnt || !d_is_dir(mnt->mnt_root))
		return ERR_PTR(-ENOTDIR);
	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);
	port = &table->owner;
	port->table = table;
	table->root.mnt = mnt;
	table->root.dentry = mnt->mnt_root;
	path_get(&table->root);
	xa_init(&table->files);
	mutex_init(&table->lock);
	table->next = 1;
	return port;
}

void kobox_linux_fs_destroy(struct kobox_linux_fs_port *port)
{
	struct kobox_linux_fs_table *table;
	struct file *file;
	unsigned long index;

	if (!port)
		return;
	table = port->table;
	/* Filesystem flushes and final mount release may sleep. Teardown is
	 * caller-quiesced, so neither belongs inside the table lock.
	 */
	xa_for_each(&table->files, index, file)
		filp_close(file, NULL);
	xa_destroy(&table->files);
	path_put(&table->root);
	kfree(table);
}

static struct file *fs_file(struct kobox_linux_fs_port *port, u64 handle)
{
	struct file *file;
	unsigned int i;

	if (!port || !handle || handle > ULONG_MAX)
		return ERR_PTR(-EBADF);
	for (i = 0; i < ARRAY_SIZE(port->pinned); i++)
		if (port->handles[i] == handle)
			return get_file(port->pinned[i]);
	mutex_lock(&port->table->lock);
	file = xa_load(&port->table->files, handle);
	if (file)
		get_file(file);
	mutex_unlock(&port->table->lock);
	return file ?: ERR_PTR(-EBADF);
}

void kobox_linux_fs_port_release(struct kobox_linux_fs_port *view)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(view->pinned); i++)
		if (view->pinned[i])
			fput(view->pinned[i]);
	memset(view, 0, sizeof(*view));
}

int kobox_linux_fs_port_snapshot(struct kobox_linux_fs_port *port,
	u64 first, u64 second, struct kobox_linux_fs_port *view)
{
	u64 handles[] = { first, second };
	unsigned int i;
	int error;

	*view = (struct kobox_linux_fs_port) { .table = port->table };
	for (i = 0; i < ARRAY_SIZE(handles); i++) {
		struct file *file;

		if (!handles[i])
			continue;
		file = fs_file(port, handles[i]);
		if (IS_ERR(file)) {
			error = PTR_ERR(file);
			kobox_linux_fs_port_release(view);
			return error;
		}
		view->pinned[i] = file;
		view->handles[i] = handles[i];
	}
	return 0;
}

/* Consume the reference on success. Never reuse an ID: an old request must
 * not acquire authority to a different file opened after its handle closed.
 */
static int fs_install(struct kobox_linux_fs_port *port, struct file *file,
		      u64 *handle)
{
	int error;

	mutex_lock(&port->table->lock);
	if (!port->table->next) {
		error = -EOVERFLOW;
		goto out_unlock;
	}
	error = xa_insert(&port->table->files, port->table->next, file, GFP_KERNEL);
	if (!error)
		*handle = port->table->next++;
out_unlock:
	mutex_unlock(&port->table->lock);
	return error;
}

ssize_t kobox_linux_fs_readlink_handle(struct kobox_linux_fs_port *port,
				      u64 handle, void *buffer, size_t count,
				      unsigned int flags)
{
	struct file *file;
	struct fs_struct *saved, *private;
	char *storage = NULL, *name;
	size_t capacity = 128, length;
	ssize_t result;

	if (flags > 1 || !count || !buffer)
		return -EINVAL;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	if (flags && !S_ISDIR(file_inode(file)->i_mode)) {
		result = -ENOTDIR;
		goto out_file;
	}
	if (flags && d_unlinked(file->f_path.dentry)) {
		result = -ENOENT;
		goto out_file;
	}
	private = copy_fs_struct(current->fs);
	if (!private) {
		result = -ENOMEM;
		goto out_file;
	}
	set_fs_root(private, &port->table->root);
	saved = current->fs;
	current->fs = private;
	/* d_path owns rename/unlink and anonymous-file naming semantics. Grow
	 * its private buffer instead of retaining an obsolete open-time name
	 * or imposing a second, smaller path limit at the IPC boundary.
	 */
	for (;;) {
		storage = kvmalloc(capacity, GFP_KERNEL);
		if (!storage) {
			result = -ENOMEM;
			break;
		}
		name = d_path(&file->f_path, storage, capacity);
		if (!IS_ERR(name)) {
			length = strlen(name);
			if (flags && length >= count)
				result = -ERANGE;
			else {
				result = min(length, count);
				memcpy(buffer, name, result);
			}
			break;
		}
		result = PTR_ERR(name);
		kvfree(storage);
		storage = NULL;
		if (result != -ENAMETOOLONG || capacity == INT_MAX)
			break;
		capacity = min(capacity * 2, (size_t)INT_MAX);
	}
	current->fs = saved;
	free_fs_struct(private);
	kvfree(storage);
out_file:
	fput(file);
	return result;
}

struct fs_scope {
	struct fs_struct *saved;
	struct fs_struct *private;
	bool retained;
	/* Inline only: return pwd to the root while the request still pins the
	 * directory, so a later setter never drops its last reference on the
	 * owner and the owner scope pins no dirfd between requests. */
	const struct path *reset_pwd;
};

static int fs_scope_begin(struct kobox_linux_fs_port *port, u64 directory,
			  const char *name, umode_t mask,
			  struct fs_scope *scope)
{
	struct file *file = NULL;
	const struct path *pwd = &port->table->root;

	/* Absolute paths ignore dirfd, except openat2's scoped resolution. The
	 * caller passes NULL for that case so Linux can apply IN_ROOT/BENEATH.
	 */
	if (directory && (!name || *name != '/')) {
		file = fs_file(port, directory);
		if (IS_ERR(file))
			return PTR_ERR(file);
		if (!S_ISDIR(file_inode(file)->i_mode)) {
			fput(file);
			return -ENOTDIR;
		}
		pwd = &file->f_path;
	}
	/* Changing a shared kthread fs_struct would change another request's
	 * root and umask. A private scope uses the actual namei implementation
	 * without keeping path strings or doing a second userspace path walk.
	 */
	scope->saved = current->fs;
	/* A worker owns this scratch fs_struct until join; each operation checks
	 * root/pwd and resets umask. Never reuse it recursively or from another task. Shared
	 * and synchronous callers keep the original fresh-scope path.
	 */
	scope->retained = port->scope && port->scope != current->fs;
	scope->private = scope->retained ? port->scope :
		copy_fs_struct(current->fs ?: init_task.fs);
	if (!scope->private) {
		if (file)
			fput(file);
		return -ENOMEM;
	}
	/* Retained scratch is exclusive and not installed on any task here.
	 * Equal mount+dentry pairs already own the required references: another
	 * get/set/put only contends on the same path and mount. Compare actual
	 * paths, never client IDs; a changed root/dirfd still uses upstream setters.
	 * Fresh/recursive scopes keep the original initialization path.
	 */
	if (!scope->retained ||
	    !path_equal(&scope->private->root, &port->table->root))
		set_fs_root(scope->private, &port->table->root);
	if (!scope->retained || !path_equal(&scope->private->pwd, pwd))
		set_fs_pwd(scope->private, pwd);
	scope->private->umask = mask;
	scope->reset_pwd = scope->retained && port->nowait ? &port->table->root : NULL;
	task_lock(current);
	current->fs = scope->private;
	task_unlock(current);
	if (file)
		fput(file);
	return 0;
}

static void fs_scope_end(struct fs_scope *scope)
{
	task_lock(current);
	current->fs = scope->saved;
	task_unlock(current);
	if (scope->reset_pwd && !path_equal(&scope->private->pwd, scope->reset_pwd))
		set_fs_pwd(scope->private, scope->reset_pwd);
	if (!scope->retained)
		free_fs_struct(scope->private);
}

int kobox_linux_fs_openat(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 directory,
			 const char *name, const struct open_how *how,
			 umode_t mask, u64 *handle)
{
	struct open_flags op;
	struct open_how checked;
	struct filename *filename;
	struct fs_scope scope;
	const struct cred *saved;
	struct file *file;
	int error;

	if (!port || !cred || !name || !how || !handle || mask & ~0777)
		return -EINVAL;
	checked = *how;
	/* Match openat2's large-file policy before upstream flag validation;
	 * otherwise F_GETFL exposes different flags from the native syscall.
	 */
	if (!(checked.flags & O_PATH) && force_o_largefile())
		checked.flags |= O_LARGEFILE;
	error = build_open_flags(&checked, &op);
	if (error)
		return error;
	/* getname_kernel permits empty names for kernel callers, whereas the
	 * syscall's getname rejects them before do_filp_open reaches namei.
	 */
	if (!*name)
		return -ENOENT;
	filename = getname_kernel(name);
	if (IS_ERR(filename))
		return PTR_ERR(filename);
	saved = override_creds(cred);
	error = fs_scope_begin(port, directory, how->resolve &
			       (RESOLVE_IN_ROOT | RESOLVE_BENEATH) ? NULL : name,
			       mask, &scope);
	if (!error) {
		file = do_filp_open(AT_FDCWD, filename, &op);
		error = PTR_ERR_OR_ZERO(file);
		if (!error) {
			error = fs_install(port, file, handle);
			if (error)
				fput(file);
		}
		fs_scope_end(&scope);
	}
	revert_creds(saved);
	putname(filename);
	return error;
}

int kobox_linux_fs_open(struct kobox_linux_fs_port *port,
		       const struct cred *cred, const char *name,
		       const struct open_how *how, u64 *handle)
{
	return kobox_linux_fs_openat(port, cred, 0, name, how,
				   current->fs ? current->fs->umask : 0,
				   handle);
}

int kobox_linux_fs_close(struct kobox_linux_fs_port *port,
			const struct cred *cred, u64 handle)
{
	const struct cred *saved;
	struct file *file;
	int error;

	if (!cred)
		return -EINVAL;
	if (!port || !handle || handle > ULONG_MAX)
		return -EBADF;
	mutex_lock(&port->table->lock);
	file = xa_erase(&port->table->files, handle);
	mutex_unlock(&port->table->lock);
	if (!file)
		return -EBADF;
	saved = override_creds(cred);
	error = filp_close(file, NULL);
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_dup(struct kobox_linux_fs_port *port,
		      const struct cred *cred, u64 handle, u64 *duplicate)
{
	const struct cred *saved;
	struct file *file;
	int error;

	if (!cred || !duplicate)
		return -EINVAL;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	saved = override_creds(cred);
	error = fs_install(port, file, duplicate);
	if (error)
		fput(file);
	revert_creds(saved);
	return error;
}

/* Largest pread done inline: a cached copy still occupies the owner's CPU. */
#define FS_INLINE_READ_MAX (128 * 1024)

/* The inline counterpart of kernel_read() for a regular file: the same
 * permission check, MAX_RW_COUNT clamp and accounting, but only completing
 * from the page cache. Anything that would sleep or start I/O returns -EAGAIN
 * before an effect: block/char devices (no i_size to tell a short read from
 * EOF), O_DIRECT, an atime update (it may start a journal), and uncached pages
 * or readahead (IOCB_NOIO). A short read that stopped before EOF is discarded
 * and repeated whole by a worker; a read has nothing to undo, and IN_ACCESS is
 * reported only for the result that is actually returned.
 */
static ssize_t fs_read_nowait(struct file *file, void *buffer, size_t count,
			      loff_t offset)
{
	struct inode *inode = file_inode(file);
	struct kvec kvec;
	struct iov_iter iter;
	struct kiocb kiocb;
	ssize_t result;

	if (!S_ISREG(inode->i_mode) || count > FS_INLINE_READ_MAX ||
	    !file->f_op->read_iter || !(file->f_mode & FMODE_CAN_READ) ||
	    atime_needs_update(&file->f_path, inode))
		return -EAGAIN;
	result = rw_verify_area(READ, file, &offset, count);
	if (result)
		return result;
	if (count > MAX_RW_COUNT)
		count = MAX_RW_COUNT;
	init_sync_kiocb(&kiocb, file);
	if (kiocb.ki_flags & IOCB_DIRECT)
		return -EAGAIN;
	kiocb.ki_pos = offset;
	kiocb.ki_flags |= IOCB_NOWAIT | IOCB_NOIO;
	kvec = (struct kvec) { .iov_base = buffer, .iov_len = count };
	iov_iter_kvec(&iter, ITER_DEST, &kvec, 1, count);
	result = file->f_op->read_iter(&kiocb, &iter);
	if (result == -EAGAIN || result == -EOPNOTSUPP)
		return -EAGAIN;
	if (result >= 0 && (size_t)result < count &&
	    offset + result < i_size_read(inode))
		return -EAGAIN;
	if (result > 0) {
		fsnotify_access(file);
		add_rchar(current, result);
	}
	inc_syscr(current);
	return result;
}

/* With an inline result in hand, drop the request's file pins only while the
 * table still holds each file: then no fput here can be the final one, whose
 * __fput (release, eviction of an unlinked inode) could block the owner. A
 * concurrent close or a busy table returns false and the caller discards the
 * idempotent result for a worker, which keeps the pins and releases them.
 */
bool kobox_linux_fs_port_release_nonfinal(struct kobox_linux_fs_port *view)
{
	unsigned int i;

	if (!view->pinned[0] && !view->pinned[1]) {
		memset(view, 0, sizeof(*view));
		return true;
	}
	if (!mutex_trylock(&view->table->lock))
		return false;
	for (i = 0; i < ARRAY_SIZE(view->pinned); i++) {
		if (view->pinned[i] &&
		    xa_load(&view->table->files, view->handles[i]) != view->pinned[i]) {
			mutex_unlock(&view->table->lock);
			return false;
		}
	}
	for (i = 0; i < ARRAY_SIZE(view->pinned); i++)
		if (view->pinned[i])
			fput(view->pinned[i]);
	mutex_unlock(&view->table->lock);
	memset(view, 0, sizeof(*view));
	return true;
}

struct fs_put_work {
	struct work_struct work;
	struct path path;
};

static void fs_put_work_run(struct work_struct *work)
{
	struct fs_put_work *put = container_of(work, struct fs_put_work, work);

	path_put(&put->path);
	kfree(put);
}

/* A concurrent unlink can leave an inline stat holding the last dentry
 * reference, whose dput would evict (truncate) the inode on the owner. Drop a
 * shared reference directly; hand a possibly final one to a kworker.
 */
static void fs_path_put_nowait(struct path *path)
{
	struct fs_put_work *put;

	if (lockref_put_or_lock(&path->dentry->d_lockref)) {
		mntput(path->mnt);
		return;
	}
	/* Possibly the last reference: drop the lock without changing it. */
	spin_unlock(&path->dentry->d_lock);
	put = kmalloc(sizeof(*put), GFP_KERNEL);
	if (!put) {
		path_put(path);
		return;
	}
	put->path = *path;
	INIT_WORK(&put->work, fs_put_work_run);
	schedule_work(&put->work);
}

static ssize_t fs_io(struct kobox_linux_fs_port *port, const struct cred *cred,
		     u64 handle, void *buffer, size_t count, loff_t offset,
		     bool positioned, bool write)
{
	const struct cred *saved;
	struct file *file;
	loff_t *position;
	ssize_t result;
	bool lock_position;

	if (!cred || (!buffer && count))
		return -EINVAL;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	saved = override_creds(cred);
	/* kernel_read/write require access modes already checked by their
	 * caller. Reject an untrusted wrong-mode request before those helpers
	 * can WARN; open-time DAC itself remains entirely in the upstream VFS.
	 */
	if (!(file->f_mode & (write ? FMODE_WRITE : FMODE_READ))) {
		result = -EBADF;
		goto out;
	}
	if (S_ISDIR(file_inode(file)->i_mode)) {
		result = -EISDIR;
		goto out;
	}
	if (positioned && offset < 0) {
		result = -EINVAL;
		goto out;
	}
	if (positioned && !(file->f_mode & (write ? FMODE_PWRITE : FMODE_PREAD))) {
		result = -ESPIPE;
		goto out;
	}
	if (port->nowait) {
		/* f_pos, writes and files whose read_iter ignores IOCB_NOWAIT
		 * (tmpfs among them) could sleep or leave an effect: punt. */
		result = positioned && !write && (file->f_mode & FMODE_NOWAIT) ?
			 fs_read_nowait(file, buffer, count, offset) : -EAGAIN;
		goto out;
	}
	/* Nonseekable files may reuse f_pos_lock's union storage. Follow the
	 * same FMODE_ATOMIC_POS contract as the upstream fd_pos helpers.
	 */
	lock_position = !positioned && (file->f_mode & FMODE_ATOMIC_POS);
	if (lock_position)
		mutex_lock(&file->f_pos_lock);
	position = positioned ? &offset :
		   file->f_mode & FMODE_STREAM ? NULL : &file->f_pos;
	result = write ? kernel_write(file, buffer, count, position) :
			 kernel_read(file, buffer, count, position);
	if (lock_position)
		mutex_unlock(&file->f_pos_lock);
out:
	fput(file);
	revert_creds(saved);
	return result;
}

ssize_t kobox_linux_fs_read(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 handle,
			   void *buffer, size_t count)
{
	return fs_io(port, cred, handle, buffer, count, 0, false, false);
}

ssize_t kobox_linux_fs_write(struct kobox_linux_fs_port *port,
			    const struct cred *cred, u64 handle,
			    const void *buffer, size_t count)
{
	return fs_io(port, cred, handle, (void *)buffer, count, 0, false, true);
}

ssize_t kobox_linux_fs_pread(struct kobox_linux_fs_port *port,
			    const struct cred *cred, u64 handle,
			    void *buffer, size_t count, loff_t offset)
{
	return fs_io(port, cred, handle, buffer, count, offset, true, false);
}

ssize_t kobox_linux_fs_pwrite(struct kobox_linux_fs_port *port,
			     const struct cred *cred, u64 handle,
			     const void *buffer, size_t count, loff_t offset)
{
	return fs_io(port, cred, handle, (void *)buffer, count, offset, true, true);
}

loff_t kobox_linux_fs_seek(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 handle,
			  loff_t offset, unsigned int whence)
{
	const struct cred *saved;
	struct file *file;
	loff_t result;

	if (!cred)
		return -EINVAL;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	saved = override_creds(cred);
	if (file->f_mode & FMODE_PATH) {
		result = -EBADF;
	} else {
		if (file->f_mode & FMODE_ATOMIC_POS)
			mutex_lock(&file->f_pos_lock);
		result = whence <= SEEK_MAX ? vfs_llseek(file, offset, whence) : -EINVAL;
		if (file->f_mode & FMODE_ATOMIC_POS)
			mutex_unlock(&file->f_pos_lock);
	}
	fput(file);
	revert_creds(saved);
	return result;
}

static bool fs_filldir(struct dir_context *ctx, const char *name, int length,
		       loff_t offset, u64 ino, unsigned int type)
{
	struct fs_dirents *entries = container_of(ctx, struct fs_dirents, ctx);
	unsigned int size = ALIGN(offsetof(struct linux_dirent64, d_name) +
				  length + 1, sizeof(u64));
	char *record;

	if (length <= 0 || length >= PATH_MAX || memchr(name, '/', length)) {
		entries->error = -EIO;
		return false;
	}
	entries->error = -EINVAL;
	if (size > ctx->count)
		return false;
	record = entries->buffer + entries->used;
	if (entries->used && !(type & FILLDIR_FLAG_NOINTR) &&
	    signal_pending(current))
		return false;
	/* Like getdents64, the following entry supplies the previous cookie. */
	if (entries->used)
		put_unaligned(offset, (s64 *)(entries->buffer + entries->previous +
			      offsetof(struct linux_dirent64, d_off)));
	memset(record, 0, size);
	put_unaligned(ino, (u64 *)record);
	put_unaligned((u16)size, (u16 *)(record +
		      offsetof(struct linux_dirent64, d_reclen)));
	record[offsetof(struct linux_dirent64, d_type)] = type & S_DT_MASK;
	memcpy(record + offsetof(struct linux_dirent64, d_name), name, length);
	entries->previous = entries->used;
	entries->used += size;
	ctx->count -= size;
	return true;
}

ssize_t kobox_linux_fs_getdents(struct kobox_linux_fs_port *port,
			      const struct cred *cred, u64 handle,
			      void *buffer, unsigned int count)
{
	struct fs_dirents entries = {
		.ctx.actor = fs_filldir,
		.ctx.count = count,
		.ctx.dt_flags_mask = FILLDIR_FLAG_NOINTR,
		.buffer = buffer,
	};
	const struct cred *saved;
	struct file *file;
	ssize_t result;

	if (!cred || (!buffer && count))
		return -EINVAL;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	saved = override_creds(cred);
	if (file->f_mode & FMODE_PATH) {
		result = -EBADF;
		goto out;
	}
	if (!S_ISDIR(file_inode(file)->i_mode)) {
		result = -ENOTDIR;
		goto out;
	}
	mutex_lock(&file->f_pos_lock);
	result = iterate_dir(file, &entries.ctx);
	if (result >= 0)
		result = entries.error;
	if (entries.used) {
		put_unaligned(entries.ctx.pos,
			      (s64 *)(entries.buffer + entries.previous +
			      offsetof(struct linux_dirent64, d_off)));
		result = entries.used;
	}
	mutex_unlock(&file->f_pos_lock);
out:
	fput(file);
	revert_creds(saved);
	return result;
}

static int fs_stat_flags(int flags, u32 mask)
{
	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_STATX_SYNC_TYPE))
		return -EINVAL;
	if ((flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE ||
	    mask & STATX__RESERVED)
		return -EINVAL;
	return 0;
}

static int fs_lookup_at(struct kobox_linux_fs_port *port, u64 directory,
			const char *name, unsigned int flags, struct path *path)
{
	struct filename *filename;
	struct fs_scope scope;
	int error;

	filename = getname_kernel(name);
	if (IS_ERR(filename))
		return PTR_ERR(filename);
	error = fs_scope_begin(port, directory, name, 0, &scope);
	if (!error) {
		/* An inline attempt walks only the dcache under RCU; anything
		 * needing I/O or a blocking lock returns -EAGAIN for a worker. */
		error = filename_lookup(AT_FDCWD, filename,
					flags | (port->nowait ? LOOKUP_CACHED : 0), path, NULL);
		fs_scope_end(&scope);
	}
	putname(filename);
	return error;
}

static int fs_stat_path(const struct path *path, int flags, u32 mask,
			struct kstat *stat)
{
	int error = vfs_getattr(path, stat, mask, flags);

	if (error)
		return error;
	/* vfs_statx_path is private to upstream stat.c. Its mount augmentation
	 * uses the real pinned mount types; no numeric structure offsets or
	 * invented filesystem attributes belong in the port.
	 */
	if (mask & STATX_MNT_ID_UNIQUE) {
		stat->mnt_id = real_mount(path->mnt)->mnt_id_unique;
		stat->result_mask |= STATX_MNT_ID_UNIQUE;
	} else {
		stat->mnt_id = real_mount(path->mnt)->mnt_id;
		stat->result_mask |= STATX_MNT_ID;
	}
	if (path_mounted(path))
		stat->attributes |= STATX_ATTR_MOUNT_ROOT;
	stat->attributes_mask |= STATX_ATTR_MOUNT_ROOT;
	return 0;
}

int kobox_linux_fs_device_route(struct kobox_linux_fs_port *port,
	const struct cred *cred, u64 directory, const char *name,
	const struct open_how *how, struct kstat *stat, u64 *handle)
{
	struct open_flags original;
	struct open_how probe;
	const struct cred *saved;
	struct file *file;
	u64 opened;
	int error;

	if (!port || !cred || !name || !how || !stat || !handle)
		return -EINVAL;
	error = build_open_flags(how, &original);
	if (error)
		return error;
	/* O_PATH cannot create/truncate a regular file or invoke a driver's open.
	 * Check the original access on that exact pinned inode, using fsuid/fsgid,
	 * rather than faccessat's different real/effective-ID selection rules.
	 */
	probe = (struct open_how) {
		.flags = O_PATH | (how->flags & (O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC)),
		.resolve = how->resolve,
	};
	error = kobox_linux_fs_openat(port, cred, directory, name, &probe, 0, &opened);
	if (error)
		return error;
	file = fs_file(port, opened);
	error = PTR_ERR_OR_ZERO(file);
	if (!IS_ERR(file)) {
		saved = override_creds(cred);
		error = S_ISCHR(file_inode(file)->i_mode) ?
			inode_permission(mnt_idmap(file->f_path.mnt), file_inode(file),
					 original.acc_mode) : -ENODEV;
		if (!error)
			error = fs_stat_path(&file->f_path, 0, STATX_BASIC_STATS, stat);
		revert_creds(saved);
		fput(file);
	}
	if (error)
		kobox_linux_fs_close(port, cred, opened);
	else
		*handle = opened;
	return error;
}

int kobox_linux_fs_statat(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 directory,
			 const char *name, int flags, u32 mask,
			 struct kstat *stat)
{
	const struct cred *saved;
	struct path path;
	struct file *file;
	unsigned int lookup = 0;
	int error;

	if (!port || !cred || !stat)
		return -EINVAL;
	if (!name || !*name) {
		if (!(flags & AT_EMPTY_PATH))
			return -ENOENT;
		flags &= ~AT_EMPTY_PATH;
		error = fs_stat_flags(flags, mask);
		if (error)
			return error;
		if (port->nowait && (flags & AT_STATX_SYNC_TYPE) == AT_STATX_FORCE_SYNC)
			return -EAGAIN;
		file = directory ? fs_file(port, directory) : NULL;
		if (IS_ERR(file))
			return PTR_ERR(file);
		saved = override_creds(cred);
		error = fs_stat_path(file ? &file->f_path : &port->table->root,
				     flags, mask, stat);
		if (file)
			fput(file);
		revert_creds(saved);
		return error;
	}
	flags &= ~AT_EMPTY_PATH;
	error = fs_stat_flags(flags, mask);
	if (error)
		return error;
	if (!(flags & AT_SYMLINK_NOFOLLOW))
		lookup |= LOOKUP_FOLLOW;
	if (!(flags & AT_NO_AUTOMOUNT))
		lookup |= LOOKUP_AUTOMOUNT;
	/* A forced sync may wait for a remote filesystem: never inline. */
	if (port->nowait && (flags & AT_STATX_SYNC_TYPE) == AT_STATX_FORCE_SYNC)
		return -EAGAIN;
	saved = override_creds(cred);
	error = fs_lookup_at(port, directory, name, lookup, &path);
	if (!error) {
		error = fs_stat_path(&path, flags, mask, stat);
		if (port->nowait)
			fs_path_put_nowait(&path);
		else
			path_put(&path);
	}
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_getattr(struct kobox_linux_fs_port *port,
			  const struct cred *cred, const char *name,
			  int flags, u32 mask, struct kstat *stat)
{
	return kobox_linux_fs_statat(port, cred, 0, name, flags, mask, stat);
}

int kobox_linux_fs_fgetattr(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 handle,
			   int flags, u32 mask, struct kstat *stat)
{
	const struct cred *saved;
	struct file *file;
	int error;

	if (!cred || !stat)
		return -EINVAL;
	error = fs_stat_flags(flags, mask);
	if (error)
		return error;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	saved = override_creds(cred);
	error = fs_stat_path(&file->f_path, flags, mask, stat);
	fput(file);
	revert_creds(saved);
	return error;
}

static int fs_parent_lookup(const char *name, struct fs_parent *parent)
{
	int error;

	parent->name = getname_kernel(name);
	if (IS_ERR(parent->name))
		return PTR_ERR(parent->name);
	error = vfs_path_parent_lookup(parent->name, 0, &parent->path,
				       &parent->last, &parent->type, NULL);
	if (error)
		putname(parent->name);
	return error;
}

static void fs_parent_put(struct fs_parent *parent)
{
	path_put(&parent->path);
	putname(parent->name);
}

static int fs_parent_lookup_at(struct kobox_linux_fs_port *port, u64 directory,
			       const char *name, struct fs_parent *parent)
{
	struct fs_scope scope;
	int error;

	error = fs_scope_begin(port, directory, name, 0, &scope);
	if (error)
		return error;
	error = fs_parent_lookup(name, parent);
	fs_scope_end(&scope);
	return error;
}

enum fs_mutation {
	FS_MKDIR,
	FS_UNLINK,
	FS_RMDIR,
	FS_SYMLINK,
};

static int fs_mutate(struct kobox_linux_fs_port *port, const struct cred *cred,
		     u64 directory, const char *name, enum fs_mutation operation,
		     umode_t mode, umode_t mask, const char *target)
{
	struct fs_parent parent;
	struct fs_scope scope;
	const struct cred *saved;
	struct dentry *entry, *created;
	struct mnt_idmap *idmap;
	unsigned int flags;
	int error;

	if (!port || !cred || !name)
		return -EINVAL;
	saved = override_creds(cred);
	error = fs_scope_begin(port, directory, name, mask, &scope);
	if (error)
		goto out_creds;
	error = fs_parent_lookup(name, &parent);
	if (error)
		goto out_scope;
	if (parent.type != LAST_NORM) {
		error = operation == FS_MKDIR ? -EEXIST : -EINVAL;
		goto out_parent;
	}
	error = mnt_want_write(parent.path.mnt);
	if (error)
		goto out_parent;
	inode_lock_nested(d_inode(parent.path.dentry), I_MUTEX_PARENT);
	flags = operation == FS_MKDIR || operation == FS_SYMLINK ?
		LOOKUP_CREATE | LOOKUP_EXCL : 0;
	entry = lookup_one_qstr_excl(&parent.last, parent.path.dentry, flags);
	error = PTR_ERR_OR_ZERO(entry);
	if (error)
		goto out_unlock;
	idmap = mnt_idmap(parent.path.mnt);
	if (operation == FS_MKDIR) {
		mode &= 0777 | S_ISVTX;
		error = security_path_mkdir(&parent.path, entry, mode);
		if (!error) {
			created = vfs_mkdir(idmap, d_inode(parent.path.dentry),
					    entry, mode);
			error = PTR_ERR_OR_ZERO(created);
			/* vfs_mkdir consumes its input even on failure. */
			entry = IS_ERR(created) ? NULL : created;
		}
	} else if (operation == FS_SYMLINK) {
		error = parent.last.name[parent.last.len] ? -ENOENT :
			security_path_symlink(&parent.path, entry, target);
		if (!error)
			error = vfs_symlink(idmap, d_inode(parent.path.dentry),
					    entry, target);
	} else if (operation == FS_RMDIR) {
		error = d_is_negative(entry) ? -ENOENT :
			security_path_rmdir(&parent.path, entry);
		if (!error)
			error = vfs_rmdir(idmap, d_inode(parent.path.dentry), entry);
	} else {
		error = d_is_negative(entry) ? -ENOENT : 0;
		if (parent.last.name[parent.last.len])
			error = d_is_negative(entry) ? -ENOENT :
				d_is_dir(entry) ? -EISDIR : -ENOTDIR;
		if (!error)
			error = security_path_unlink(&parent.path, entry);
		if (!error)
			error = vfs_unlink(idmap, d_inode(parent.path.dentry),
					   entry, NULL);
	}
	dput(entry);
out_unlock:
	inode_unlock(d_inode(parent.path.dentry));
	mnt_drop_write(parent.path.mnt);
out_parent:
	fs_parent_put(&parent);
out_scope:
	fs_scope_end(&scope);
out_creds:
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_mkdir(struct kobox_linux_fs_port *port,
			const struct cred *cred, const char *name, umode_t mode)
{
	return fs_mutate(port, cred, 0, name, FS_MKDIR, mode,
			 current->fs ? current->fs->umask : 0, NULL);
}

int kobox_linux_fs_mkdirat(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 directory,
			  const char *name, umode_t mode, umode_t mask)
{
	if (mask & ~0777)
		return -EINVAL;
	return fs_mutate(port, cred, directory, name, FS_MKDIR, mode, mask, NULL);
}

int kobox_linux_fs_unlink(struct kobox_linux_fs_port *port,
			 const struct cred *cred, const char *name, int flags)
{
	if (flags & ~AT_REMOVEDIR)
		return -EINVAL;
	return fs_mutate(port, cred, 0, name, flags ? FS_RMDIR : FS_UNLINK,
			 0, 0, NULL);
}

int kobox_linux_fs_unlinkat(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 directory,
			   const char *name, int flags)
{
	if (flags & ~AT_REMOVEDIR)
		return -EINVAL;
	return fs_mutate(port, cred, directory, name,
			 flags ? FS_RMDIR : FS_UNLINK, 0, 0, NULL);
}

int kobox_linux_fs_symlink(struct kobox_linux_fs_port *port,
			  const struct cred *cred, const char *target,
			  const char *name)
{
	if (!target)
		return -EINVAL;
	return fs_mutate(port, cred, 0, name, FS_SYMLINK, 0, 0, target);
}

int kobox_linux_fs_symlinkat(struct kobox_linux_fs_port *port,
			    const struct cred *cred, const char *target,
			    u64 directory, const char *name)
{
	if (!target)
		return -EINVAL;
	return fs_mutate(port, cred, directory, name, FS_SYMLINK, 0, 0, target);
}

static int fs_rename_locked(struct fs_parent *from, struct fs_parent *to,
			    unsigned int flags)
{
	struct dentry *old, *new, *trap;
	struct renamedata rd;
	unsigned int lookup = LOOKUP_RENAME_TARGET | LOOKUP_CREATE;
	int error;

	if (flags & RENAME_EXCHANGE)
		lookup = 0;
	if (flags & RENAME_NOREPLACE)
		lookup |= LOOKUP_EXCL;
	trap = lock_rename(to->path.dentry, from->path.dentry);
	if (IS_ERR(trap))
		return PTR_ERR(trap);
	old = lookup_one_qstr_excl(&from->last, from->path.dentry, 0);
	error = PTR_ERR_OR_ZERO(old);
	if (error)
		goto out_unlock;
	new = lookup_one_qstr_excl(&to->last, to->path.dentry, lookup);
	error = PTR_ERR_OR_ZERO(new);
	if (error)
		goto out_old;
	error = -ENOTDIR;
	if (!d_is_dir(old) && (from->last.name[from->last.len] ||
	    (!(flags & RENAME_EXCHANGE) && to->last.name[to->last.len])))
		goto out_new;
	if ((flags & RENAME_EXCHANGE) && !d_is_dir(new) &&
	    to->last.name[to->last.len])
		goto out_new;
	error = -EINVAL;
	if (old == trap)
		goto out_new;
	error = flags & RENAME_EXCHANGE ? -EINVAL : -ENOTEMPTY;
	if (new == trap)
		goto out_new;
	error = security_path_rename(&from->path, old, &to->path, new, flags);
	if (error)
		goto out_new;
	rd = (struct renamedata) {
		.mnt_idmap = mnt_idmap(from->path.mnt),
		.old_parent = from->path.dentry,
		.old_dentry = old,
		.new_parent = to->path.dentry,
		.new_dentry = new,
		.flags = flags,
	};
	error = vfs_rename(&rd);
out_new:
	dput(new);
out_old:
	dput(old);
out_unlock:
	unlock_rename(to->path.dentry, from->path.dentry);
	return error;
}

int kobox_linux_fs_renameat(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 old_directory,
			   const char *from, u64 new_directory,
			   const char *to, unsigned int flags)
{
	struct fs_parent old, new;
	const struct cred *saved;
	int error;

	if (!port || !cred || !from || !to ||
	    flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT) ||
	    ((flags & RENAME_EXCHANGE) &&
	     (flags & (RENAME_NOREPLACE | RENAME_WHITEOUT))))
		return -EINVAL;
	saved = override_creds(cred);
	error = fs_parent_lookup_at(port, old_directory, from, &old);
	if (error)
		goto out_creds;
	error = fs_parent_lookup_at(port, new_directory, to, &new);
	if (error)
		goto out_old;
	error = -EXDEV;
	if (old.path.mnt != new.path.mnt)
		goto out_new;
	error = -EBUSY;
	if (old.type != LAST_NORM)
		goto out_new;
	error = flags & RENAME_NOREPLACE ? -EEXIST : -EBUSY;
	if (new.type != LAST_NORM)
		goto out_new;
	error = mnt_want_write(old.path.mnt);
	if (!error) {
		error = fs_rename_locked(&old, &new, flags);
		mnt_drop_write(old.path.mnt);
	}
out_new:
	fs_parent_put(&new);
out_old:
	fs_parent_put(&old);
out_creds:
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_rename(struct kobox_linux_fs_port *port,
			 const struct cred *cred, const char *from,
			 const char *to, unsigned int flags)
{
	return kobox_linux_fs_renameat(port, cred, 0, from, 0, to, flags);
}

ssize_t kobox_linux_fs_readlinkat(struct kobox_linux_fs_port *port,
				 const struct cred *cred, u64 directory,
				 const char *name, void *buffer, size_t count)
{
	DEFINE_DELAYED_CALL(done);
	const struct cred *saved;
	const char *link;
	struct path path;
	ssize_t result;

	if (!port || !cred || !name || !buffer || !count)
		return -EINVAL;
	saved = override_creds(cred);
	if (!*name) {
		struct file *file = fs_file(port, directory);

		result = PTR_ERR_OR_ZERO(file);
		if (result)
			goto out_creds;
		path = file->f_path;
		path_get(&path);
		fput(file);
	} else {
		result = fs_lookup_at(port, directory, name, 0, &path);
	}
	if (result)
		goto out_creds;
	link = vfs_get_link(path.dentry, &done);
	result = PTR_ERR_OR_ZERO(link);
	if (!IS_ERR(link)) {
		result = min(count, strlen(link));
		memcpy(buffer, link, result);
		touch_atime(&path);
	}
	do_delayed_call(&done);
	path_put(&path);
out_creds:
	revert_creds(saved);
	return result;
}

ssize_t kobox_linux_fs_readlink(struct kobox_linux_fs_port *port,
			       const struct cred *cred, const char *name,
			       void *buffer, size_t count)
{
	return kobox_linux_fs_readlinkat(port, cred, 0, name, buffer, count);
}

int kobox_linux_fs_dup_to(struct kobox_linux_fs_port *port,
			 struct kobox_linux_fs_port *target,
			 const struct cred *cred, u64 handle, u64 *duplicate)
{
	const struct cred *saved;
	struct file *file;
	int error;

	if (!target || !cred || !duplicate)
		return -EINVAL;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	saved = override_creds(cred);
	error = fs_install(target, file, duplicate);
	if (error)
		fput(file);
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_statfs(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 handle,
			 struct kstatfs *stat)
{
	const struct cred *saved;
	struct file *file;
	int error;

	if (!port || !cred || !stat)
		return -EINVAL;
	file = handle ? fs_file(port, handle) : NULL;
	if (IS_ERR(file))
		return PTR_ERR(file);
	saved = override_creds(cred);
	error = vfs_statfs(file ? &file->f_path : &port->table->root, stat);
	if (file)
		fput(file);
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_truncate(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 handle, loff_t length)
{
	const struct cred *saved;
	struct file *file;
	int error;

	if (!cred || length < 0)
		return -EINVAL;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	saved = override_creds(cred);
	error = file->f_mode & FMODE_PATH ? -EBADF : do_ftruncate(file, length, 0);
	fput(file);
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_fsync(struct kobox_linux_fs_port *port,
			const struct cred *cred, u64 handle, bool data_only)
{
	const struct cred *saved;
	struct file *file;
	int error;

	if (!cred)
		return -EINVAL;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	saved = override_creds(cred);
	error = file->f_mode & FMODE_PATH ? -EBADF : vfs_fsync(file, data_only);
	fput(file);
	revert_creds(saved);
	return error;
}

static int fs_metadata_path(struct kobox_linux_fs_port *port, u64 directory,
			    const char *name, unsigned int flags,
			    struct path *path)
{
	struct file *file;

	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
		return -EINVAL;
	if (name && *name)
		return fs_lookup_at(port, directory, name,
			(flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW, path);
	if (!(flags & AT_EMPTY_PATH))
		return -ENOENT;
	file = directory ? fs_file(port, directory) : NULL;
	if (IS_ERR(file))
		return PTR_ERR(file);
	*path = file ? file->f_path : port->table->root;
	path_get(path);
	if (file)
		fput(file);
	return 0;
}

int kobox_linux_fs_chmodat(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 directory,
			  const char *name, umode_t mode, unsigned int flags)
{
	const struct cred *saved;
	struct path path;
	int error;

	if (!port || !cred)
		return -EINVAL;
	saved = override_creds(cred);
	error = fs_metadata_path(port, directory, name, flags, &path);
	if (!error) {
		error = chmod_common(&path, mode);
		path_put(&path);
	}
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_chownat(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 directory,
			  const char *name, u32 uid, u32 gid, unsigned int flags)
{
	const struct cred *saved;
	struct path path;
	int error;

	if (!port || !cred)
		return -EINVAL;
	saved = override_creds(cred);
	error = fs_metadata_path(port, directory, name, flags, &path);
	if (!error) {
		error = chown_common(&path, uid, gid);
		path_put(&path);
	}
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_utimensat(struct kobox_linux_fs_port *port,
			    const struct cred *cred, u64 directory,
			    const char *name, struct timespec64 *times,
			    unsigned int flags)
{
	const struct cred *saved;
	struct path path;
	int error;

	if (!port || !cred)
		return -EINVAL;
	/* Linux skips pathname lookup when both timestamps are UTIME_OMIT. */
	if (times && times[0].tv_nsec == UTIME_OMIT &&
	    times[1].tv_nsec == UTIME_OMIT)
		return 0;
	if (!name) {
		if (flags)
			return -EINVAL;
		if (!directory)
			return -EBADF;
		flags = AT_EMPTY_PATH;
	}
	saved = override_creds(cred);
	error = fs_metadata_path(port, directory, name, flags, &path);
	if (!error) {
		error = vfs_utimes(&path, times);
		path_put(&path);
	}
	revert_creds(saved);
	return error;
}

struct fs_fd_scope {
	struct files_struct *saved, *private;
};

static void fs_fd_scope_end(struct fs_fd_scope *scope)
{
	struct fdtable *table = files_fdtable(current->files);
	unsigned int fd;

	/* These are borrowed references, not application closes. filp_close
	 * would flush and remove POSIX locks merely for calling fcntl/linkat.
	 */
	for (fd = 0; fd < table->max_fds; fd++) {
		struct file *file = file_close_fd(fd);

		if (file)
			fput(file);
	}
	task_lock(current);
	current->files = scope->saved;
	task_unlock(current);
	put_files_struct(scope->private);
}

static int fs_fd_scope_begin(struct fs_fd_scope *scope)
{
	scope->saved = current->files;
	scope->private = dup_fd(current->files ?: init_task.files, NULL);
	if (IS_ERR(scope->private))
		return PTR_ERR(scope->private);
	task_lock(current);
	current->files = scope->private;
	task_unlock(current);
	return 0;
}

static int fs_borrow_fd(struct kobox_linux_fs_port *port, u64 handle)
{
	struct file *file = fs_file(port, handle);
	int fd;

	if (IS_ERR(file))
		return PTR_ERR(file);
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		fput(file);
	else
		fd_install(fd, file);
	return fd;
}

long kobox_linux_fs_fcntl(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 handle,
			 unsigned int command, unsigned long argument)
{
	struct fs_fd_scope scope;
	const struct cred *saved;
	struct pt_regs regs = {};
	long result;
	int fd;

	if (!port || !cred)
		return -EINVAL;
	/* Descriptor flags and dup belong to the personality's descriptor
	 * table, not this temporary fd. Pointer arguments need a user mapping.
	 * Use the actual syscall for shared file status flags and memfd seals.
	 */
	if (command != F_GETFL && command != F_SETFL &&
	    command != F_GET_SEALS && command != F_ADD_SEALS)
		return -EOPNOTSUPP;
	result = fs_fd_scope_begin(&scope);
	if (result)
		return result;
	fd = fs_borrow_fd(port, handle);
	result = fd;
	if (fd >= 0) {
		saved = override_creds(cred);
		regs.di = fd;
		regs.si = command;
		regs.dx = argument;
		result = x64_sys_call(&regs, __NR_fcntl);
		revert_creds(saved);
	}
	fs_fd_scope_end(&scope);
	return result;
}

int kobox_linux_fs_sync(const struct cred *cred)
{
	const struct cred *saved;

	if (!cred)
		return -EINVAL;
	/* Linux sync is global and unprivileged. Reusing syncfs(root) would omit
	 * other mounted superblocks and invent an fd for a no-argument syscall. */
	saved = override_creds(cred);
	ksys_sync();
	revert_creds(saved);
	return 0;
}

int kobox_linux_fs_syncfs(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 handle)
{
	struct file *file;
	struct super_block *sb;
	const struct cred *saved;
	int error, writeback;

	if (!port || !cred)
		return -EINVAL;
	file = fs_file(port, handle);
	if (IS_ERR(file))
		return PTR_ERR(file);
	if (file->f_mode & FMODE_PATH) {
		fput(file);
		return -EBADF;
	}
	saved = override_creds(cred);
	sb = file_inode(file)->i_sb;
	down_read(&sb->s_umount);
	error = sync_filesystem(sb);
	up_read(&sb->s_umount);
	writeback = errseq_check_and_advance(&sb->s_wb_err, &file->f_sb_err);
	fput(file);
	revert_creds(saved);
	return error ?: writeback;
}

static int fs_directory_fd(struct kobox_linux_fs_port *port, u64 directory,
			   const char *name)
{
	return !directory || (name && *name == '/') ? AT_FDCWD :
		fs_borrow_fd(port, directory);
}

int kobox_linux_fs_linkat(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 old_directory,
			 const char *from, u64 new_directory,
			 const char *to, unsigned int flags)
{
	struct fs_fd_scope fds;
	struct fs_scope fs;
	struct filename *old, *new;
	const struct cred *saved;
	int oldfd, newfd, error;

	if (!port || !cred || !from || !to)
		return -EINVAL;
	if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW))
		return -EINVAL;
	if (!*to || (!*from && !(flags & AT_EMPTY_PATH)))
		return -ENOENT;
	error = fs_fd_scope_begin(&fds);
	if (error)
		return error;
	oldfd = fs_directory_fd(port, old_directory, from);
	newfd = fs_directory_fd(port, new_directory, to);
	if ((oldfd < 0 && oldfd != AT_FDCWD) ||
	    (newfd < 0 && newfd != AT_FDCWD)) {
		error = oldfd < 0 && oldfd != AT_FDCWD ? oldfd : newfd;
		goto out_fds;
	}
	saved = override_creds(cred);
	error = fs_scope_begin(port, 0, NULL, 0, &fs);
	if (!error) {
		old = getname_kernel(from);
		new = getname_kernel(to);
		/* do_linkat owns both names, including error pointers. It retains
		 * upstream protected-hardlink, delegation and empty-path rules.
		 */
		error = do_linkat(oldfd, old, newfd, new, flags);
		fs_scope_end(&fs);
	}
	revert_creds(saved);
out_fds:
	fs_fd_scope_end(&fds);
	return error;
}

static bool fs_native_input_allowed(void)
{
	/* The existing software-uaccess port admits native pointers only for
	 * mm-less kernel/boot tasks. Never turn a client task's failed Linux
	 * address lookup into an access to the sandbox's native address space.
	 */
	return !current->mm &&
		((current->flags & PF_KTHREAD) || task_pid_nr(current) == 1);
}

int kobox_linux_fs_mknodat(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 directory,
			  const char *name, umode_t mode, umode_t mask,
			  unsigned int device)
{
	struct fs_scope scope;
	const struct cred *saved;
	struct pt_regs regs = {.di = AT_FDCWD, .si = (unsigned long)name,
		.dx = mode, .r10 = device};
	int error;

	if (!port || !cred || !name || mask & ~0777)
		return -EINVAL;
	if (!fs_native_input_allowed())
		return -EOPNOTSUPP;
	saved = override_creds(cred);
	error = fs_scope_begin(port, directory, name, mask, &scope);
	if (!error) {
		/* The private native string is already bounded and immutable.
		 * Calling upstream retains may_mknod, ACL/umask, LSM, encoded
		 * device-number and creation/delegation rules without a copy here.
		 */
		error = x64_sys_call(&regs, __NR_mknodat);
		fs_scope_end(&scope);
	}
	revert_creds(saved);
	return error;
}

int kobox_linux_fs_accessat(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 directory,
			   const char *name, unsigned int mode,
			   unsigned int flags)
{
	struct fs_scope fs;
	struct fs_fd_scope fds;
	const struct cred *saved;
	struct pt_regs regs = {.si = (unsigned long)name, .dx = mode,
		.r10 = flags};
	int fd, error;

	if (!port || !cred || !name)
		return -EINVAL;
	if (!fs_native_input_allowed())
		return -EOPNOTSUPP;
	error = fs_fd_scope_begin(&fds);
	if (error)
		return error;
	fd = fs_directory_fd(port, directory, name);
	if (fd < 0 && fd != AT_FDCWD) {
		error = fd;
		goto out_fds;
	}
	saved = override_creds(cred);
	error = fs_scope_begin(port, 0, NULL, 0, &fs);
	if (!error) {
		regs.di = fd;
		/* In particular, faccessat's real/effective-ID selection and
		 * capability adjustment must remain Linux's decision.
		 */
		error = x64_sys_call(&regs, __NR_faccessat2);
		fs_scope_end(&fs);
	}
	revert_creds(saved);
out_fds:
	fs_fd_scope_end(&fds);
	return error;
}

int kobox_linux_fs_memfd_create(struct kobox_linux_fs_port *port,
				const struct cred *cred, const char *name,
				unsigned int flags, u64 *handle)
{
	struct fs_fd_scope scope;
	const struct cred *saved;
	struct pt_regs regs = {.di = (unsigned long)name, .si = flags};
	struct file *file;
	int fd, error;

	if (!port || !cred || !name || !handle)
		return -EINVAL;
	if (!fs_native_input_allowed())
		return -EOPNOTSUPP;
	error = fs_fd_scope_begin(&scope);
	if (error)
		return error;
	saved = override_creds(cred);
	fd = x64_sys_call(&regs, __NR_memfd_create);
	error = fd;
	if (fd >= 0) {
		/* Move the actual sealed shmem file out of the ephemeral fd table.
		 * No fake memfd inode or second seal state exists in the service.
		 */
		file = file_close_fd(fd);
		error = file ? fs_install(port, file, handle) : -EIO;
		if (file && error)
			fput(file);
	}
	revert_creds(saved);
	fs_fd_scope_end(&scope);
	return error;
}

struct file *kobox_linux_fs_open_exec(struct kobox_linux_fs_port *port,
				    const struct cred *cred, u64 directory,
				    const char *name)
{
	struct fs_scope scope;
	const struct cred *saved;
	struct file *file;
	int error;

	if (!port || !cred || !name)
		return ERR_PTR(-EINVAL);
	if (!*name)
		return ERR_PTR(-ENOENT);
	saved = override_creds(cred);
	error = fs_scope_begin(port, directory, name, 0, &scope);
	if (error)
		file = ERR_PTR(error);
	else {
		/* open_exec owns MAY_EXEC, regular-file and noexec checks and the
		 * executable deny-write reference. O_RDONLY would wrongly require
		 * MAY_READ and reject an executable mode-0111 inode.
		 */
		file = open_exec(name);
		fs_scope_end(&scope);
	}
	revert_creds(saved);
	return file;
}

int kobox_linux_fs_exec_getattr(struct file *file, const struct cred *cred,
			      int flags, u32 mask, struct kstat *stat)
{
	const struct cred *saved;
	int error;

	if (!file || !cred || !stat)
		return -EINVAL;
	flags &= ~AT_EMPTY_PATH;
	error = fs_stat_flags(flags, mask);
	if (error)
		return error;
	saved = override_creds(cred);
	error = fs_stat_path(&file->f_path, flags, mask, stat);
	revert_creds(saved);
	return error;
}
