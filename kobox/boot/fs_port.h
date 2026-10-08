/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_PORT_H
#define KOBOX_BOOT_FS_PORT_H

#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <uapi/linux/openat2.h>

struct kobox_linux_fs_port;
/* Owner-loader only. The returned open_exec file must be paired with
 * exe_file_allow_write_access before its final fput; never install it into
 * an ordinary client handle table. */
struct file *kobox_linux_fs_open_exec(struct kobox_linux_fs_port *,
    const struct cred *, u64 directory, const char *name);
int kobox_linux_fs_exec_getattr(struct file *, const struct cred *,
    int flags, u32 mask, struct kstat *);

/* Process-local GPL API, not a wire ABI. All paths use the granted mount as
 * root, including absolute paths and symlinks. Credentials are immutable and
 * caller-owned. Quiesce callers before destroy; buffers are kernel-accessible.
 */
struct kobox_linux_fs_port *kobox_linux_fs_create(struct vfsmount *mnt);
void kobox_linux_fs_destroy(struct kobox_linux_fs_port *port);
int kobox_linux_fs_openat(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 directory,
			 const char *name, const struct open_how *how,
			 umode_t mask, u64 *handle);
int kobox_linux_fs_statat(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 directory,
			 const char *name, int flags, u32 mask,
			 struct kstat *stat);
/* Side-effect-free native-device classification. Validate the original open
 * flags and its DAC access, but never call a device driver's open method. */
int kobox_linux_fs_device_route(struct kobox_linux_fs_port *port,
	const struct cred *cred, u64 directory, const char *name,
	const struct open_how *how, struct kstat *stat, u64 *handle);
int kobox_linux_fs_mkdirat(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 directory,
			  const char *name, umode_t mode, umode_t mask);
int kobox_linux_fs_unlinkat(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 directory,
			   const char *name, int flags);
int kobox_linux_fs_symlinkat(struct kobox_linux_fs_port *port,
			    const struct cred *cred, const char *target,
			    u64 directory, const char *name);
int kobox_linux_fs_renameat(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 old_directory,
			   const char *from, u64 new_directory,
			   const char *to, unsigned int flags);
ssize_t kobox_linux_fs_readlinkat(struct kobox_linux_fs_port *port,
				 const struct cred *cred, u64 directory,
				 const char *name, void *buffer, size_t count);
int kobox_linux_fs_dup_to(struct kobox_linux_fs_port *port,
			 struct kobox_linux_fs_port *target,
			 const struct cred *cred, u64 handle, u64 *duplicate);
struct kstatfs;
ssize_t kobox_linux_fs_readlink_handle(struct kobox_linux_fs_port *port,
				      u64 handle, void *buffer, size_t count,
				      unsigned int flags);
int kobox_linux_fs_statfs(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 handle,
			 struct kstatfs *stat);
int kobox_linux_fs_chmodat(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 directory,
			  const char *name, umode_t mode, unsigned int flags);
int kobox_linux_fs_chownat(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 directory,
			  const char *name, u32 uid, u32 gid, unsigned int flags);
int kobox_linux_fs_utimensat(struct kobox_linux_fs_port *port,
			    const struct cred *cred, u64 directory,
			    const char *name, struct timespec64 *times,
			    unsigned int flags);
long kobox_linux_fs_fcntl(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 handle,
			 unsigned int command, unsigned long argument);
/* sync has no fd operand and applies to this sandbox's Linux instance. */
int kobox_linux_fs_sync(const struct cred *cred);
int kobox_linux_fs_syncfs(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 handle);
int kobox_linux_fs_linkat(struct kobox_linux_fs_port *port,
			 const struct cred *cred, u64 old_directory,
			 const char *from, u64 new_directory,
			 const char *to, unsigned int flags);
int kobox_linux_fs_mknodat(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 directory,
			  const char *name, umode_t mode, umode_t mask,
			  unsigned int device);
int kobox_linux_fs_accessat(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 directory,
			   const char *name, unsigned int mode,
			   unsigned int flags);
int kobox_linux_fs_memfd_create(struct kobox_linux_fs_port *port,
				const struct cred *cred, const char *name,
				unsigned int flags, u64 *handle);
int kobox_linux_fs_open(struct kobox_linux_fs_port *port,
		       const struct cred *cred, const char *name,
		       const struct open_how *how, u64 *handle);
int kobox_linux_fs_close(struct kobox_linux_fs_port *port,
			const struct cred *cred, u64 handle);
int kobox_linux_fs_dup(struct kobox_linux_fs_port *port,
		      const struct cred *cred, u64 handle, u64 *duplicate);
ssize_t kobox_linux_fs_read(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 handle,
			   void *buffer, size_t count);
ssize_t kobox_linux_fs_write(struct kobox_linux_fs_port *port,
			    const struct cred *cred, u64 handle,
			    const void *buffer, size_t count);
ssize_t kobox_linux_fs_pread(struct kobox_linux_fs_port *port,
			    const struct cred *cred, u64 handle,
			    void *buffer, size_t count, loff_t offset);
ssize_t kobox_linux_fs_pwrite(struct kobox_linux_fs_port *port,
			     const struct cred *cred, u64 handle,
			     const void *buffer, size_t count, loff_t offset);
loff_t kobox_linux_fs_seek(struct kobox_linux_fs_port *port,
			  const struct cred *cred, u64 handle,
			  loff_t offset, unsigned int whence);
ssize_t kobox_linux_fs_getdents(struct kobox_linux_fs_port *port,
			      const struct cred *cred, u64 handle,
			      void *buffer, unsigned int count);
int kobox_linux_fs_getattr(struct kobox_linux_fs_port *port,
			  const struct cred *cred, const char *name,
			  int flags, u32 mask, struct kstat *stat);
int kobox_linux_fs_fgetattr(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 handle,
			   int flags, u32 mask, struct kstat *stat);
int kobox_linux_fs_mkdir(struct kobox_linux_fs_port *port,
			const struct cred *cred, const char *name, umode_t mode);
int kobox_linux_fs_unlink(struct kobox_linux_fs_port *port,
			 const struct cred *cred, const char *name, int flags);
int kobox_linux_fs_rename(struct kobox_linux_fs_port *port,
			 const struct cred *cred, const char *from,
			 const char *to, unsigned int flags);
int kobox_linux_fs_symlink(struct kobox_linux_fs_port *port,
			  const struct cred *cred, const char *target,
			  const char *name);
ssize_t kobox_linux_fs_readlink(struct kobox_linux_fs_port *port,
			       const struct cred *cred, const char *name,
			       void *buffer, size_t count);
int kobox_linux_fs_truncate(struct kobox_linux_fs_port *port,
			   const struct cred *cred, u64 handle, loff_t length);
int kobox_linux_fs_fsync(struct kobox_linux_fs_port *port,
			const struct cred *cred, u64 handle, bool data_only);

#endif /* KOBOX_BOOT_FS_PORT_H */
