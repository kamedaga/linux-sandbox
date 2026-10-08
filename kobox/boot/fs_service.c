// SPDX-License-Identifier: GPL-2.0-only
#include "fs_service.h"
#include "fs_port_internal.h"
#include "fs_exec.h"

#include <kobox2/filesystem.h>
#include <linux/kdev_t.h>
#include <linux/mount.h>
#include <linux/overflow.h>
#include <linux/refcount.h>
#include <linux/sched/user.h>
#include <linux/securebits.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/task_work.h>
#include <linux/unaligned.h>
#include <linux/xarray.h>

#ifdef KOBOX_FS_SERVICE_PROFILE
#include <linux/atomic.h>
#include <linux/printk.h>

struct fs_profile_row {
	atomic64_t calls;
	atomic64_t cycles[5];
	atomic64_t queued_cycles;
};
#endif

struct fs_profile_call {
#ifdef KOBOX_FS_SERVICE_PROFILE
	u64 stamps[7];
	int row;
#endif
};

struct fs_client {
	struct kobox_linux_fs_port *port;
	const struct cred *cred;
	u64 credential_generation;
	u64 rights;
	refcount_t references;
};

struct kobox_linux_fs_service {
	struct vfsmount *root;
	struct xarray clients;
	struct mutex lock;
	struct kobox_linux_exec_table *exec;
	u64 generation, last_client;
#ifdef KOBOX_FS_SERVICE_PROFILE
	struct fs_profile_row profile[3];
#endif
};

struct kobox_linux_fs_request {
	struct kobox_linux_fs_service *service;
	struct fs_client *client;
	struct fs_client *target;
	const struct cred *cred;
	const struct cred *target_cred;
	struct kobox_linux_fs_port files;
	struct kobox_linux_exec_file *executable;
	u8 *snapshot;
	void *response;
	size_t response_capacity;
	kb2_fs_request_t decoded;
	kb2_fs_response_t reply;
	struct fs_profile_call profile;
	size_t owned_bytes;
	bool executed, control, packed, recycled;
	/* This is a small-request fast path, not a path/payload limit. Larger
	 * borrowed-buffer calls still allocate their complete immutable snapshot.
	 */
	u8 inline_snapshot[2 * KB2_FILESYSTEM_REQUEST_SIZE];
};

static s64 fs_control_execute(struct kobox_linux_fs_request *job);

#ifdef KOBOX_RUNTIME_GATES
static bool fs_request_gate_fail;

void kobox_linux_fs_request_gate_fail_allocation(void)
{
	WRITE_ONCE(fs_request_gate_fail, true);
}
#endif

static void fs_profile_stamp(struct fs_profile_call *call, unsigned int index)
{
#ifdef KOBOX_FS_SERVICE_PROFILE
	u32 lo, hi;

	asm volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
	call->stamps[index] = ((u64)hi << 32) | lo;
#endif
}

static void fs_profile_select(struct fs_profile_call *call, u32 opcode)
{
#ifdef KOBOX_FS_SERVICE_PROFILE
	call->row = opcode == KB2_FILESYSTEM_OP_STATX ? 0 :
		opcode == KB2_FILESYSTEM_OP_PREAD ? 1 :
		opcode == KB2_FILESYSTEM_OP_PWRITE ? 2 : -1;
#endif
}

static void fs_profile_finish(struct kobox_linux_fs_service *service,
			      struct fs_profile_call *call)
{
#ifdef KOBOX_FS_SERVICE_PROFILE
	struct fs_profile_row *row;
	unsigned int i;

	if (call->row < 0)
		return;
	row = &service->profile[call->row];
	atomic64_inc(&row->calls);
	for (i = 0; i < ARRAY_SIZE(row->cycles); i++) {
		unsigned int start = i < 2 ? i : i + 1;

		atomic64_add(call->stamps[start + 1] - call->stamps[start], &row->cycles[i]);
	}
	atomic64_add(call->stamps[3] - call->stamps[2], &row->queued_cycles);
#endif
}

static void fs_profile_dump(struct kobox_linux_fs_service *service)
{
#ifdef KOBOX_FS_SERVICE_PROFILE
	static const u32 opcodes[] = { KB2_FILESYSTEM_OP_STATX,
		KB2_FILESYSTEM_OP_PREAD, KB2_FILESYSTEM_OP_PWRITE };
	unsigned int i;

	/* Dump only after the owner has quiesced callers: logging must not become
	 * part of a timed request, and counters must remain safe with many clients.
	 */
	for (i = 0; i < ARRAY_SIZE(opcodes); i++) {
		struct fs_profile_row *row = &service->profile[i];

		pr_info("FS_SERVICE_PROFILE opcode=%u calls=%llu snapshot=%llu auth=%llu execute=%llu release=%llu task_work=%llu queue=%llu\n",
			opcodes[i], (u64)atomic64_read(&row->calls),
			(u64)atomic64_read(&row->cycles[0]),
			(u64)atomic64_read(&row->cycles[1]),
			(u64)atomic64_read(&row->cycles[2]),
			(u64)atomic64_read(&row->cycles[3]),
			(u64)atomic64_read(&row->cycles[4]),
			(u64)atomic64_read(&row->queued_cycles));
	}
#endif
}

#if defined(KOBOX_FS_INTERNAL_BENCH) && defined(KOBOX_FS_SERVICE_PROFILE)
void kobox_linux_fs_profile_reset(struct kobox_linux_fs_service *service)
{
	unsigned int i, j;

	/* Diagnostics call this only after every measured request is released. */
	for (i = 0; i < ARRAY_SIZE(service->profile); i++) {
		atomic64_set(&service->profile[i].calls, 0);
		atomic64_set(&service->profile[i].queued_cycles, 0);
		for (j = 0; j < ARRAY_SIZE(service->profile[i].cycles); j++)
			atomic64_set(&service->profile[i].cycles[j], 0);
	}
}

void kobox_linux_fs_profile_report(struct kobox_linux_fs_service *service)
{
	fs_profile_dump(service);
}
#endif

static void fs_client_put(struct fs_client *client)
{
	if (client && refcount_dec_and_test(&client->references)) {
		kobox_linux_fs_destroy(client->port);
		put_cred(client->cred);
		kfree(client);
	}
}

static struct fs_client *fs_client_get(struct kobox_linux_fs_service *service,
				      u64 id, u64 credential_generation,
				      const struct cred **cred, u64 *rights)
{
	struct fs_client *client;

	if (!id || id > ULONG_MAX)
		return ERR_PTR(-EACCES);
	mutex_lock(&service->lock);
	client = xa_load(&service->clients, id);
	if (!client)
		client = ERR_PTR(-EACCES);
	else if (credential_generation &&
		 credential_generation != client->credential_generation)
		client = ERR_PTR(-ESTALE);
	else {
		refcount_inc(&client->references);
		*cred = get_cred(client->cred);
		if (rights)
			*rights = client->rights;
	}
	mutex_unlock(&service->lock);
	return client;
}

struct kobox_linux_fs_service *
kobox_linux_fs_service_create(struct vfsmount *root, u64 generation)
{
	struct kobox_linux_fs_service *service;

	if (!root || !generation)
		return ERR_PTR(-EINVAL);
	service = kzalloc(sizeof(*service), GFP_KERNEL);
	if (!service)
		return ERR_PTR(-ENOMEM);
	service->root = mntget(root);
	service->generation = generation;
	service->exec = kobox_linux_exec_create();
	if (IS_ERR(service->exec)) {
		int error = PTR_ERR(service->exec);

		mntput(service->root);
		kfree(service);
		return ERR_PTR(error);
	}
	xa_init(&service->clients);
	mutex_init(&service->lock);
	return service;
}

void kobox_linux_fs_service_destroy(struct kobox_linux_fs_service *service)
{
	struct fs_client *client;
	unsigned long id;

	if (!service)
		return;
	fs_profile_dump(service);
	xa_for_each(&service->clients, id, client)
		fs_client_put(client);
	xa_destroy(&service->clients);
	kobox_linux_exec_destroy(service->exec);
	mntput(service->root);
	kfree(service);
}

int kobox_linux_fs_service_register(struct kobox_linux_fs_service *service,
				   u64 id, u64 credential_generation,
				   const struct cred *cred, u64 rights)
{
	struct fs_client *client;
	int error;

	if (!service || !id || id > ULONG_MAX || !credential_generation || !cred ||
	    rights & ~(u64)KB2_FILESYSTEM_RIGHTS_VALID_MASK)
		return -EINVAL;
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	client->port = kobox_linux_fs_create(service->root);
	if (IS_ERR(client->port)) {
		error = PTR_ERR(client->port);
		kfree(client);
		return error;
	}
	client->cred = get_cred(cred);
	client->credential_generation = credential_generation;
	client->rights = rights;
	refcount_set(&client->references, 1);
	mutex_lock(&service->lock);
	/* Reusing an ID would let a queued request from a dead client act on
	 * an unrelated new client's handles, even if no table slot was reused.
	 */
	error = id <= service->last_client ? -EEXIST :
		xa_insert(&service->clients, id, client, GFP_KERNEL);
	if (!error)
		service->last_client = id;
	mutex_unlock(&service->lock);
	if (error)
		fs_client_put(client);
	return error;
}

int kobox_linux_fs_service_credentials(struct kobox_linux_fs_service *service,
				      u64 id, u64 generation,
				      const struct cred *cred, u64 rights)
{
	struct fs_client *client;
	const struct cred *old = NULL;
	int error = 0;

	if (!service || !id || id > ULONG_MAX || !generation || !cred ||
	    rights & ~(u64)KB2_FILESYSTEM_RIGHTS_VALID_MASK)
		return -EINVAL;
	mutex_lock(&service->lock);
	client = xa_load(&service->clients, id);
	if (!client)
		error = -ENOENT;
	else if (generation <= client->credential_generation)
		error = -ESTALE;
	else {
		old = client->cred;
		client->cred = get_cred(cred);
		client->credential_generation = generation;
		client->rights = rights;
	}
	mutex_unlock(&service->lock);
	if (old)
		put_cred(old);
	return error;
}

int kobox_linux_fs_service_unregister(struct kobox_linux_fs_service *service,
				     u64 id)
{
	struct fs_client *client;

	if (!service || !id || id > ULONG_MAX)
		return -EINVAL;
	mutex_lock(&service->lock);
	client = xa_erase(&service->clients, id);
	mutex_unlock(&service->lock);
	if (!client)
		return -ENOENT;
	kobox_linux_exec_release_client(service->exec, id);
	fs_client_put(client);
	return 0;
}

int kobox_linux_fs_service_transfer(struct kobox_linux_fs_service *service,
				   u64 source, u64 target, u64 handle,
				   u64 *duplicate)
{
	struct fs_client *from, *to;
	const struct cred *from_cred, *to_cred;
	int error;

	if (!service || !duplicate)
		return -EINVAL;
	from = fs_client_get(service, source, 0, &from_cred, NULL);
	if (IS_ERR(from))
		return PTR_ERR(from);
	to = fs_client_get(service, target, 0, &to_cred, NULL);
	error = PTR_ERR_OR_ZERO(to);
	if (!error) {
		error = kobox_linux_fs_dup_to(from->port, to->port, from_cred,
					      handle, duplicate);
		put_cred(to_cred);
		fs_client_put(to);
	}
	put_cred(from_cred);
	fs_client_put(from);
	return error;
}

static void fs_timestamp(u8 *bytes, struct timespec64 time)
{
	put_unaligned_le64(time.tv_sec,
		bytes + KB2_FILESYSTEM_STATX_TIMESTAMP_SECONDS_OFFSET);
	put_unaligned_le32(time.tv_nsec,
		bytes + KB2_FILESYSTEM_STATX_TIMESTAMP_NANOSECONDS_OFFSET);
}

static void fs_statx_encode(u8 *bytes, const struct kstat *stat)
{
	memset(bytes, 0, KB2_FILESYSTEM_STATX_SIZE);
#define STAT32(field, value) \
	put_unaligned_le32(value, bytes + KB2_FILESYSTEM_STATX_##field##_OFFSET)
#define STAT64(field, value) \
	put_unaligned_le64(value, bytes + KB2_FILESYSTEM_STATX_##field##_OFFSET)
	STAT32(MASK, stat->result_mask);
	STAT32(BLKSIZE, stat->blksize);
	STAT64(ATTRIBUTES, stat->attributes);
	STAT32(NLINK, stat->nlink);
	STAT32(UID, from_kuid_munged(current_user_ns(), stat->uid));
	STAT32(GID, from_kgid_munged(current_user_ns(), stat->gid));
	put_unaligned_le16(stat->mode, bytes + KB2_FILESYSTEM_STATX_MODE_OFFSET);
	STAT64(INO, stat->ino);
	STAT64(SIZE, stat->size);
	STAT64(BLOCKS, stat->blocks);
	STAT64(ATTRIBUTES_MASK, stat->attributes_mask);
	fs_timestamp(bytes + KB2_FILESYSTEM_STATX_ATIME_OFFSET, stat->atime);
	fs_timestamp(bytes + KB2_FILESYSTEM_STATX_BTIME_OFFSET, stat->btime);
	fs_timestamp(bytes + KB2_FILESYSTEM_STATX_CTIME_OFFSET, stat->ctime);
	fs_timestamp(bytes + KB2_FILESYSTEM_STATX_MTIME_OFFSET, stat->mtime);
	STAT32(RDEV_MAJOR, MAJOR(stat->rdev));
	STAT32(RDEV_MINOR, MINOR(stat->rdev));
	STAT32(DEV_MAJOR, MAJOR(stat->dev));
	STAT32(DEV_MINOR, MINOR(stat->dev));
	STAT64(MNT_ID, stat->mnt_id);
	STAT32(DIO_MEM_ALIGN, stat->dio_mem_align);
	STAT32(DIO_OFFSET_ALIGN, stat->dio_offset_align);
	STAT32(DIO_READ_OFFSET_ALIGN, stat->dio_read_offset_align);
	STAT64(SUBVOL, stat->subvol);
	STAT32(ATOMIC_WRITE_UNIT_MIN, stat->atomic_write_unit_min);
	STAT32(ATOMIC_WRITE_UNIT_MAX, stat->atomic_write_unit_max);
	STAT32(ATOMIC_WRITE_UNIT_MAX_OPT, stat->atomic_write_unit_max_opt);
	STAT32(ATOMIC_WRITE_SEGMENTS_MAX, stat->atomic_write_segments_max);
#undef STAT32
#undef STAT64
}

static void fs_statfs_encode(u8 *bytes, const struct kstatfs *stat)
{
	memset(bytes, 0, KB2_FILESYSTEM_STATFS_SIZE);
#define STAT64(field, value) \
	put_unaligned_le64(value, bytes + KB2_FILESYSTEM_STATFS_##field##_OFFSET)
	STAT64(TYPE, stat->f_type);
	STAT64(BSIZE, stat->f_bsize);
	STAT64(BLOCKS, stat->f_blocks);
	STAT64(BFREE, stat->f_bfree);
	STAT64(BAVAIL, stat->f_bavail);
	STAT64(FILES, stat->f_files);
	STAT64(FFREE, stat->f_ffree);
	put_unaligned_le32(stat->f_fsid.val[0],
		bytes + KB2_FILESYSTEM_STATFS_FSID_OFFSET);
	put_unaligned_le32(stat->f_fsid.val[1],
		bytes + KB2_FILESYSTEM_STATFS_FSID_OFFSET + sizeof(u32));
	STAT64(NAMELEN, stat->f_namelen);
	STAT64(FRSIZE, stat->f_frsize);
	STAT64(FLAGS, stat->f_flags);
#undef STAT64
}

static const char *fs_path(const u8 *bytes, kb2_fs_span_t span)
{
	return span.length ? (const char *)bytes + span.offset : NULL;
}

static s64 fs_execute(struct kobox_linux_fs_port *port, const struct cred *cred,
		      const kb2_fs_request_t *r, const u8 *bytes,
		      u8 *data, size_t capacity, kb2_fs_response_t *reply)
{
	const char *path = fs_path(bytes, r->path);
	const char *second = fs_path(bytes, r->second_path);
	struct kstat stat = {};
	struct kstatfs statfs = {};
	struct open_how how;
	struct timespec64 times[2], *time_arg = NULL;
	s64 result;
	size_t count;

	switch (r->opcode) {
	case KB2_FILESYSTEM_OP_EXEC_OPEN:
		return -EPERM;
	case KB2_FILESYSTEM_OP_DEVICE_ROUTE:
		if (capacity < KB2_FILESYSTEM_STATX_SIZE)
			return -EMSGSIZE;
		how = (struct open_how) {
			.flags = r->flags, .mode = r->mode, .resolve = r->resolve,
		};
		result = kobox_linux_fs_device_route(port, cred, r->handle, path,
						  &how, &stat, &reply->handle);
		if (!result) {
			fs_statx_encode(data, &stat);
			reply->data_length = KB2_FILESYSTEM_STATX_SIZE;
		}
		return result;
	case KB2_FILESYSTEM_OP_OPENAT2:
		how = (struct open_how) {
			.flags = r->flags, .mode = r->mode, .resolve = r->resolve,
		};
		return kobox_linux_fs_openat(port, cred, r->handle, path, &how,
					     r->umask, &reply->handle);
	case KB2_FILESYSTEM_OP_CLOSE:
		return kobox_linux_fs_close(port, cred, r->handle);
	case KB2_FILESYSTEM_OP_DUP:
		return kobox_linux_fs_dup(port, cred, r->handle, &reply->handle);
	case KB2_FILESYSTEM_OP_READ:
	case KB2_FILESYSTEM_OP_PREAD:
		if (r->length > capacity)
			return -EMSGSIZE;
		count = r->length;
		result = r->opcode == KB2_FILESYSTEM_OP_READ ?
			kobox_linux_fs_read(port, cred, r->handle, data, count) :
			kobox_linux_fs_pread(port, cred, r->handle, data,
					     count, r->offset);
		if (result > 0)
			reply->data_length = result;
		return result;
	case KB2_FILESYSTEM_OP_WRITE:
	case KB2_FILESYSTEM_OP_PWRITE:
		if (r->length != r->data.length)
			return -EINVAL;
		return r->opcode == KB2_FILESYSTEM_OP_WRITE ?
			kobox_linux_fs_write(port, cred, r->handle,
				bytes + r->data.offset, r->length) :
			kobox_linux_fs_pwrite(port, cred, r->handle,
				bytes + r->data.offset, r->length, r->offset);
	case KB2_FILESYSTEM_OP_LSEEK:
		if (r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_seek(port, cred, r->handle,
					   r->offset, r->flags);
	case KB2_FILESYSTEM_OP_GETDENTS64:
		if (r->length > capacity)
			return -EMSGSIZE;
		if (r->length > UINT_MAX)
			return -EINVAL;
		result = kobox_linux_fs_getdents(port, cred, r->handle,
						 data, r->length);
		if (result > 0)
			reply->data_length = result;
		return result;
	case KB2_FILESYSTEM_OP_STATX:
		if (capacity < KB2_FILESYSTEM_STATX_SIZE)
			return -EMSGSIZE;
		if (r->flags > UINT_MAX)
			return -EINVAL;
		result = kobox_linux_fs_statat(port, cred, r->handle, path,
					       r->flags, r->mask, &stat);
		if (!result) {
			fs_statx_encode(data, &stat);
			reply->data_length = KB2_FILESYSTEM_STATX_SIZE;
		}
		return result;
	case KB2_FILESYSTEM_OP_STATFS:
		if (capacity < KB2_FILESYSTEM_STATFS_SIZE)
			return -EMSGSIZE;
		result = kobox_linux_fs_statfs(port, cred, r->handle, &statfs);
		if (!result) {
			fs_statfs_encode(data, &statfs);
			reply->data_length = KB2_FILESYSTEM_STATFS_SIZE;
		}
		return result;
	case KB2_FILESYSTEM_OP_FCNTL:
		if (r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_fcntl(port, cred, r->handle,
					    r->flags, r->length);
	case KB2_FILESYSTEM_OP_FSYNC:
		if (r->flags > 1)
			return -EINVAL;
		return kobox_linux_fs_fsync(port, cred, r->handle, r->flags);
	case KB2_FILESYSTEM_OP_FTRUNCATE:
		return kobox_linux_fs_truncate(port, cred, r->handle, r->offset);
	case KB2_FILESYSTEM_OP_MKDIRAT:
		return kobox_linux_fs_mkdirat(port, cred, r->handle, path,
					      r->mode, r->umask);
	case KB2_FILESYSTEM_OP_UNLINKAT:
		if (r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_unlinkat(port, cred, r->handle,
					       path, r->flags);
	case KB2_FILESYSTEM_OP_RENAMEAT2:
		if (r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_renameat(port, cred, r->handle, path,
					       r->second_handle, second, r->flags);
	case KB2_FILESYSTEM_OP_LINKAT:
		if (r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_linkat(port, cred, r->handle, path,
					     r->second_handle, second, r->flags);
	case KB2_FILESYSTEM_OP_SYMLINKAT:
		return kobox_linux_fs_symlinkat(port, cred, path,
						r->handle, second);
	case KB2_FILESYSTEM_OP_READLINKAT:
		if (r->length > capacity)
			return -EMSGSIZE;
		result = kobox_linux_fs_readlinkat(port, cred, r->handle, path,
						   data, r->length);
		if (result > 0)
			reply->data_length = result;
		return result;
	case KB2_FILESYSTEM_OP_READLINK_HANDLE:
		if (r->length > capacity)
			return -EMSGSIZE;
		if (r->flags > 1)
			return -EINVAL;
		result = kobox_linux_fs_readlink_handle(port, r->handle, data,
						      r->length, r->flags);
		if (result > 0)
			reply->data_length = result;
		return result;
	case KB2_FILESYSTEM_OP_FCHMODAT:
		if (r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_chmodat(port, cred, r->handle, path,
					      r->mode, r->flags);
	case KB2_FILESYSTEM_OP_FCHOWNAT:
		if (r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_chownat(port, cred, r->handle, path,
					      r->uid, r->gid, r->flags);
	case KB2_FILESYSTEM_OP_MKNODAT:
		if (r->length > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_mknodat(port, cred, r->handle, path,
					      r->mode, r->umask, r->length);
	case KB2_FILESYSTEM_OP_FACCESSAT2:
		if (r->mode > UINT_MAX || r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_accessat(port, cred, r->handle, path,
					       r->mode, r->flags);
	case KB2_FILESYSTEM_OP_MEMFD_CREATE:
		if (r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_memfd_create(port, cred, path, r->flags,
						  &reply->handle);
	case KB2_FILESYSTEM_OP_UTIMENSAT:
		if (r->data.length) {
			if (r->data.length != KB2_FILESYSTEM_TIMES_SIZE)
				return -EINVAL;
			times[0].tv_sec = (s64)get_unaligned_le64(bytes +
				r->data.offset + KB2_FILESYSTEM_TIMES_ATIME_SECONDS_OFFSET);
			times[0].tv_nsec = (s64)get_unaligned_le64(bytes +
				r->data.offset + KB2_FILESYSTEM_TIMES_ATIME_NANOSECONDS_OFFSET);
			times[1].tv_sec = (s64)get_unaligned_le64(bytes +
				r->data.offset + KB2_FILESYSTEM_TIMES_MTIME_SECONDS_OFFSET);
			times[1].tv_nsec = (s64)get_unaligned_le64(bytes +
				r->data.offset + KB2_FILESYSTEM_TIMES_MTIME_NANOSECONDS_OFFSET);
			time_arg = times;
		}
		if (r->flags > UINT_MAX)
			return -EINVAL;
		return kobox_linux_fs_utimensat(port, cred, r->handle, path,
						time_arg, r->flags);
	case KB2_FILESYSTEM_OP_SYNC:
		return kobox_linux_fs_sync(cred);
	case KB2_FILESYSTEM_OP_SYNCFS:
		return kobox_linux_fs_syncfs(port, cred, r->handle);
	default:
		/* Owner/exec/session operations cannot be performed by a storage
		 * client. Never silently acknowledge a catalog entry.
		 */
		return -EOPNOTSUPP;
	}
}

static u64 fs_request_directory(u64 handle, const char *name)
{
	return name && *name == '/' ? 0 : handle;
}

static int fs_request_bind_files(struct kobox_linux_fs_request *job)
{
	const kb2_fs_request_t *r = &job->decoded;
	const char *path = fs_path(job->snapshot, r->path);
	const char *second = fs_path(job->snapshot, r->second_path);
	u64 first = 0, other = 0;

	/* Bind file/dirfd operands at admission, before a queued CLOSE or owner
	 * transfer can change the table. Absolute paths ignore dirfd exactly as
	 * namei does, except openat2's explicit rooted/beneath resolution.
	 */
	switch (r->opcode) {
	case KB2_FILESYSTEM_OP_CLOSE:
	case KB2_FILESYSTEM_OP_DUP:
	case KB2_FILESYSTEM_OP_READ:
	case KB2_FILESYSTEM_OP_PREAD:
	case KB2_FILESYSTEM_OP_WRITE:
	case KB2_FILESYSTEM_OP_PWRITE:
	case KB2_FILESYSTEM_OP_LSEEK:
	case KB2_FILESYSTEM_OP_GETDENTS64:
	case KB2_FILESYSTEM_OP_STATFS:
	case KB2_FILESYSTEM_OP_FCNTL:
	case KB2_FILESYSTEM_OP_FSYNC:
	case KB2_FILESYSTEM_OP_FTRUNCATE:
	case KB2_FILESYSTEM_OP_READLINK_HANDLE:
	case KB2_FILESYSTEM_OP_SYNCFS:
	case KB2_FILESYSTEM_OP_TRANSFER_DUP:
		first = r->handle;
		break;
	case KB2_FILESYSTEM_OP_OPENAT2:
	case KB2_FILESYSTEM_OP_DEVICE_ROUTE:
		first = r->resolve & (RESOLVE_IN_ROOT | RESOLVE_BENEATH) ?
			r->handle : fs_request_directory(r->handle, path);
		break;
	case KB2_FILESYSTEM_OP_RENAMEAT2:
	case KB2_FILESYSTEM_OP_LINKAT:
		other = fs_request_directory(r->second_handle, second);
		fallthrough;
	case KB2_FILESYSTEM_OP_EXEC_OPEN:
	case KB2_FILESYSTEM_OP_STATX:
	case KB2_FILESYSTEM_OP_MKDIRAT:
	case KB2_FILESYSTEM_OP_UNLINKAT:
	case KB2_FILESYSTEM_OP_READLINKAT:
	case KB2_FILESYSTEM_OP_FCHMODAT:
	case KB2_FILESYSTEM_OP_FCHOWNAT:
	case KB2_FILESYSTEM_OP_MKNODAT:
	case KB2_FILESYSTEM_OP_FACCESSAT2:
	case KB2_FILESYSTEM_OP_UTIMENSAT:
		first = fs_request_directory(r->handle, path);
		break;
	case KB2_FILESYSTEM_OP_SYMLINKAT:
		first = fs_request_directory(r->handle, second);
		break;
	default:
		break;
	}
	return kobox_linux_fs_port_snapshot(job->client->port, first, other,
		&job->files);
}

static int fs_request_prepare(struct kobox_linux_fs_request *job,
	struct kobox_linux_fs_service *service, u64 authenticated_client,
	const void *request, size_t request_size, void *response,
	size_t response_capacity)
{
	kb2_fs_request_t *decoded = &job->decoded;
	kb2_fs_response_t *reply = &job->reply;
	struct fs_profile_call *profile = &job->profile;
	struct fs_client *client;
	const struct cred *cred;
	u64 rights, required;
	int error;

	if (!service || !request || !response ||
	    request_size < KB2_FILESYSTEM_REQUEST_SIZE ||
	    response_capacity < KB2_FILESYSTEM_RESPONSE_SIZE)
		return -EINVAL;
	job->service = service;
	job->response = response;
	job->response_capacity = response_capacity;
	fs_profile_select(profile, 0);
	fs_profile_stamp(profile, 0);
	/* Decode only private bytes: a peer must not change a path or client
	 * identity between range checks and Linux's namei/permission checks.
	 */
	if (!job->snapshot)
		job->snapshot = request_size <= sizeof(job->inline_snapshot) ?
			job->inline_snapshot : kvmalloc(request_size, GFP_KERNEL);
	if (!job->snapshot) {
		u8 header[KB2_FILESYSTEM_REQUEST_SIZE];

		/* Resource exhaustion is an operation failure, not a dead sandbox.
		 * A private header permits an authenticated, side-effect-free error
		 * even when the complete immutable payload cannot be allocated.
		 */
		memcpy(header, request, sizeof(header));
		error = kb2_fs_request_decode(header, sizeof(header), request_size,
					      decoded);
		if (error)
			return -EPROTO;
		*reply = (kb2_fs_response_t) {
			.opcode = decoded->opcode, .generation = decoded->generation,
			.sequence = decoded->sequence, .result = -ENOMEM,
		};
		if (decoded->generation != service->generation)
			reply->result = -ESTALE;
		else if (decoded->client && decoded->client != authenticated_client)
			reply->result = -EACCES;
		else {
			client = fs_client_get(service, authenticated_client,
				decoded->credential_generation, &cred, NULL);
			if (IS_ERR(client))
				reply->result = PTR_ERR(client);
			else {
				put_cred(cred);
				fs_client_put(client);
			}
		}
		return 0;
	}
	memcpy(job->snapshot, request, request_size);
	error = kb2_fs_request_decode(job->snapshot, request_size, request_size,
				      decoded);
	if (!error)
		error = kb2_fs_request_validate_paths(job->snapshot, request_size, decoded);
	if (error) {
		if (!job->packed && job->snapshot != job->inline_snapshot)
			kvfree(job->snapshot);
		job->snapshot = NULL;
		return -EPROTO;
	}
	fs_profile_stamp(profile, 1);
	*reply = (kb2_fs_response_t) {
		.opcode = decoded->opcode, .generation = decoded->generation,
		.sequence = decoded->sequence,
	};
	if (decoded->generation != service->generation)
		reply->result = -ESTALE;
	else if (decoded->client && decoded->client != authenticated_client)
		reply->result = -EACCES;
	else {
		client = fs_client_get(service, authenticated_client,
				       decoded->credential_generation, &cred, &rights);
		reply->result = PTR_ERR_OR_ZERO(client);
		if (!IS_ERR(client)) {
			job->client = client;
			job->cred = cred;
			/* The pair was retained under the owner-update lock. Neither
			 * claimed capability bits nor a concurrent credential update
			 * can change the authority between this check and execution.
			 */
			if (kb2_fs_request_policy(decoded, &required) !=
			    KB2_FS_OPERATION_DATA)
				reply->result = -EPERM;
			else if (required & ~rights)
				reply->result = -EACCES;
			if (!reply->result)
				reply->result = fs_request_bind_files(job);
		}
	}
	/* Admission includes operand pins and policy, not just client lookup.
	 * Otherwise the queue interval would incorrectly include that CPU work.
	 */
	fs_profile_stamp(profile, 2);
	return 0;
}

static void fs_request_release(struct kobox_linux_fs_request *job)
{
	kobox_linux_exec_put(job->executable);
	job->executable = NULL;
	kobox_linux_fs_port_release(&job->files);
	if (job->target_cred) {
		put_cred(job->target_cred);
		job->target_cred = NULL;
	}
	fs_client_put(job->target);
	job->target = NULL;
	if (job->cred) {
		put_cred(job->cred);
		job->cred = NULL;
	}
	fs_client_put(job->client);
	job->client = NULL;
	if (!job->packed && job->snapshot != job->inline_snapshot)
		kvfree(job->snapshot);
	job->snapshot = NULL;
}

/* Publish an executed result: encode, drop request scopes and run the final
 * fput task work before a worker or the owner hands the job back. */
static int fs_request_complete(struct kobox_linux_fs_request *job, size_t *used)
{
	kb2_fs_response_t *reply = &job->reply;
	int error;

	fs_profile_stamp(&job->profile, 4);
	if (!job->control && reply->result >= 0)
		fs_profile_select(&job->profile, job->decoded.opcode);
	if (reply->result < 0) {
		reply->handle = 0;
		reply->data_length = 0;
	}
	error = kb2_fs_response_encode(job->response, job->response_capacity, reply);
	if (!error)
		*used = KB2_FILESYSTEM_RESPONSE_SIZE + reply->data_length;
	fs_request_release(job);
	fs_profile_stamp(&job->profile, 5);
	/* This task will not return through Linux's userspace exit path. Complete
	 * final fput here, after dropping request scopes, before publishing a job.
	 */
	task_work_run();
	fs_profile_stamp(&job->profile, 6);
	if (!error)
		fs_profile_finish(job->service, &job->profile);
	return error;
}

int kobox_linux_fs_request_execute(struct kobox_linux_fs_request *job,
	size_t *used)
{
	kb2_fs_response_t *reply;

	if (!job || !used)
		return -EINVAL;
	*used = 0;
	if (job->executed)
		return -EALREADY;
	job->executed = true;
	reply = &job->reply;
	/* Admission and worker execution can be separated by a sleep. Do not
	 * report queue residence as time spent in the Linux VFS operation.
	 */
	fs_profile_stamp(&job->profile, 3);
	if (job->control && !reply->result)
		reply->result = fs_control_execute(job);
	else if (job->client && !reply->result)
		reply->result = fs_execute(&job->files, job->cred, &job->decoded,
			job->snapshot, job->response + KB2_FILESYSTEM_RESPONSE_SIZE,
			job->response_capacity - KB2_FILESYSTEM_RESPONSE_SIZE, reply);
	return fs_request_complete(job, used);
}

/* Execute on the calling (ring owner) task only if the operation finishes
 * without sleeping. Only STATX (dcache-only lookup) and PREAD (IOCB_NOWAIT)
 * are attempted; both give up with -EAGAIN before any visible effect. Then the
 * reply is restored and the job stays unexecuted for a worker, which repeats
 * it exactly as if it had never been tried. Other results complete here.
 */
bool kobox_linux_fs_request_executed(const struct kobox_linux_fs_request *job)
{
	return job && job->executed;
}

int kobox_linux_fs_request_try_nowait(struct kobox_linux_fs_request *job,
	struct fs_struct *scope, size_t *used)
{
	kb2_fs_response_t saved;
	s64 result;

	if (!job || !used)
		return -EINVAL;
	*used = 0;
#ifdef KOBOX_FS_INLINE_DISABLED
	return -EAGAIN;
#endif
	if (job->executed || job->control || !job->client || job->reply.result ||
	    (job->decoded.opcode != KB2_FILESYSTEM_OP_STATX &&
	     job->decoded.opcode != KB2_FILESYSTEM_OP_PREAD))
		return -EAGAIN;
	saved = job->reply;
	fs_profile_stamp(&job->profile, 3);
	job->files.scope = scope;
	job->files.nowait = true;
	result = fs_execute(&job->files, job->cred, &job->decoded, job->snapshot,
		job->response + KB2_FILESYSTEM_RESPONSE_SIZE,
		job->response_capacity - KB2_FILESYSTEM_RESPONSE_SIZE, &job->reply);
	job->files.nowait = false;
	/* Keeping a result also means releasing the request's file pins here;
	 * if that could be a final fput (a concurrent close), give the request
	 * to a worker instead. PREAD and STATX are idempotent to repeat. */
	if (result == -EAGAIN || !kobox_linux_fs_port_release_nonfinal(&job->files)) {
		job->reply = saved;
		job->files.scope = NULL;
		/* A failed attempt may still have deferred an fput; it must not
		 * outlive this task's turn even though the job is not published. */
		task_work_run();
		return -EAGAIN;
	}
	job->executed = true;
	job->reply.result = result;
	return fs_request_complete(job, used);
}

int kobox_linux_fs_request_prepare(struct kobox_linux_fs_service *service,
	uint64_t authenticated_client, const void *input, size_t input_size,
	void *response, size_t response_capacity,
	struct kobox_linux_fs_request **request)
{
	struct kobox_linux_fs_request *job;
	int error;

	if (!request)
		return -EINVAL;
	*request = NULL;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	error = fs_request_prepare(job, service, authenticated_client, input,
		input_size, response, response_capacity);
	if (!error && !job->client && job->reply.result == -EACCES)
		error = -EACCES;
	if (error) {
		fs_request_release(job);
		kfree(job);
		return error;
	}
	*request = job;
	return 0;
}

int kobox_linux_fs_request_execute_scope(struct kobox_linux_fs_request *job,
	struct fs_struct *scope, size_t *used)
{
	if (!job || job->executed)
		return job ? -EALREADY : -EINVAL;
	job->files.scope = scope;
	return kobox_linux_fs_request_execute(job, used);
}

int kobox_linux_fs_request_prepare_owned(struct kobox_linux_fs_service *service,
	u64 client, const void *input, size_t input_size, size_t capacity,
	struct kobox_linux_fs_request *storage,
	struct kobox_linux_fs_request **request, void **response)
{
	struct kobox_linux_fs_request *job = storage;
	void *output;
	size_t allocation, bytes;
	int error;

	if (!request || !response) {
		error = -EINVAL;
		goto out_storage;
	}
	*request = NULL;
	*response = NULL;
	if (input_size < KB2_FILESYSTEM_REQUEST_SIZE ||
	    capacity < KB2_FILESYSTEM_RESPONSE_SIZE) {
		error = -EINVAL;
		goto out_storage;
	}
	if (check_add_overflow(input_size, capacity, &bytes) ||
	    check_add_overflow(sizeof(*job), bytes, &allocation)) {
		error = -EOVERFLOW;
		goto out_storage;
	}
	/* The pool has already charged input/output and fixed request metadata.
	 * One owned allocation survives through completion/release; it cannot be
	 * reused by another request while a worker or the owner still references it.
	 */
#ifdef KOBOX_RUNTIME_GATES
	if (!storage && xchg(&fs_request_gate_fail, false))
		return -ENOMEM;
#endif
	if (storage && (!storage->packed || !storage->recycled ||
			storage->owned_bytes != bytes)) {
		error = -EINVAL;
		goto out_storage;
	}
	if (!job)
		job = kvmalloc(allocation, GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	if (!storage)
		memset(job, 0, sizeof(*job));
	job->packed = true;
	job->recycled = false;
	job->owned_bytes = bytes;
	job->snapshot = (u8 *)(job + 1);
	output = job->snapshot + input_size;

	error = fs_request_prepare(job, service, client, input, input_size,
		output, capacity);
	if (!error && !job->client && job->reply.result == -EACCES)
		error = -EACCES;
	if (error) {
		fs_request_release(job);
		task_work_run();
		kvfree(job);
		return error;
	}
	*request = job;
	*response = output;
	return 0;

out_storage:
	kobox_linux_fs_request_destroy(storage);
	return error;
}

size_t kobox_linux_fs_request_recycle(struct kobox_linux_fs_request *job)
{
	size_t bytes = job->owned_bytes;

	/* Only the owner after completion lease release. Scrub old payloads and
	 * all authority pointers before storage can cross client boundaries.
	 * Deferred final fput must finish before that memory becomes reusable.
	 */
	fs_request_release(job);
	task_work_run();
	memset(job, 0, sizeof(*job) + bytes);
	job->packed = true;
	job->recycled = true;
	job->owned_bytes = bytes;
	return bytes;
}

#ifdef KOBOX_RUNTIME_GATES
int kobox_linux_fs_request_gate_recycled(struct kobox_linux_fs_request *job,
	size_t bytes)
{
	struct kobox_linux_fs_request empty;

	memset(&empty, 0, sizeof(empty));
	empty.packed = true;
	empty.recycled = true;
	empty.owned_bytes = bytes;

	return !job || memcmp(job, &empty, sizeof(empty)) ||
		memchr_inv(job + 1, 0, bytes) ? -EINVAL : 0;
}
#endif

void kobox_linux_fs_request_destroy(struct kobox_linux_fs_request *job)
{
	if (!job)
		return;
	fs_request_release(job);
	task_work_run();
	if (job->packed)
		kvfree(job);
	else
		kfree(job);
}

int kobox_linux_fs_service_dispatch(struct kobox_linux_fs_service *service,
	uint64_t authenticated_client, const void *request, size_t request_size,
	void *response, size_t response_capacity, size_t *used)
{
	struct kobox_linux_fs_request job = {};
	int error;

	if (!used)
		return -EINVAL;
	*used = 0;
	error = fs_request_prepare(&job, service, authenticated_client, request,
		request_size, response, response_capacity);
	if (!error)
		return kobox_linux_fs_request_execute(&job, used);
	/* Malformed requests must not strand deferred work from a previous call. */
	task_work_run();
	return error;
}

static struct cred *fs_credentials_decode(const kb2_fs_request_t *r,
					 const u8 *snapshot)
{
	kb2_fs_credentials_t wire;
	struct group_info *groups;
	struct user_struct *user;
	struct cred *cred;
	size_t i, count = r->groups.length / sizeof(u32);
	int error;

	/* Credential capability bits are the pinned Linux UAPI, not a host
	 * policy invention. A baseline update must update the shared catalog.
	 */
	BUILD_BUG_ON(KB2_FILESYSTEM_CREDENTIAL_CAP_VALID_MASK != CAP_VALID_MASK);
	if (kb2_fs_credentials_decode(snapshot + r->data.offset,
				     r->data.length, &wire))
		return ERR_PTR(-EINVAL);
	if (count > NGROUPS_MAX || wire.securebits &
	    ~(SECURE_ALL_BITS | SECURE_ALL_LOCKS) ||
	    (wire.cap_permitted | wire.cap_effective |
	     wire.cap_inheritable | wire.cap_ambient) & ~CAP_VALID_MASK ||
	    wire.cap_effective & ~wire.cap_permitted ||
	    wire.cap_ambient & ~(wire.cap_permitted & wire.cap_inheritable))
		return ERR_PTR(-EINVAL);
	cred = prepare_kernel_cred(current);
	if (!cred)
		return ERR_PTR(-ENOMEM);
#define UID(field) do { \
	cred->field = make_kuid(&init_user_ns, wire.field); \
	if (!uid_valid(cred->field)) { \
		error = -EINVAL; \
		goto out_cred; \
	} \
} while (0)
#define GID(field) do { \
	cred->field = make_kgid(&init_user_ns, wire.field); \
	if (!gid_valid(cred->field)) { \
		error = -EINVAL; \
		goto out_cred; \
	} \
} while (0)
	UID(uid);
	UID(euid);
	UID(suid);
	UID(fsuid);
	GID(gid);
	GID(egid);
	GID(sgid);
	GID(fsgid);
#undef UID
#undef GID
	groups = groups_alloc(count);
	if (!groups) {
		error = -ENOMEM;
		goto out_cred;
	}
	for (i = 0; i < count; i++) {
		groups->gid[i] = make_kgid(&init_user_ns,
			get_unaligned_le32(snapshot + r->groups.offset + i * sizeof(u32)));
		if (!gid_valid(groups->gid[i])) {
			put_group_info(groups);
			error = -EINVAL;
			goto out_cred;
		}
	}
	groups_sort(groups);
	set_groups(cred, groups);
	put_group_info(groups);
	cred->cap_permitted = (kernel_cap_t){wire.cap_permitted};
	cred->cap_effective = (kernel_cap_t){wire.cap_effective};
	cred->cap_inheritable = (kernel_cap_t){wire.cap_inheritable};
	cred->cap_ambient = (kernel_cap_t){wire.cap_ambient};
	cred->securebits = wire.securebits;
	user = alloc_uid(cred->uid);
	if (!user) {
		error = -ENOMEM;
		goto out_cred;
	}
	free_uid(cred->user);
	cred->user = user;
	error = set_cred_ucounts(cred);
	if (!error)
		return cred;
out_cred:
	put_cred(cred);
	return ERR_PTR(error);
}

static s64 fs_exec_control(struct kobox_linux_fs_request *job)
{
	struct kobox_linux_fs_service *service = job->service;
	const kb2_fs_request_t *r = &job->decoded;
	const u8 *bytes = job->snapshot;
	u8 *data = job->response + KB2_FILESYSTEM_RESPONSE_SIZE;
	size_t capacity = job->response_capacity - KB2_FILESYSTEM_RESPONSE_SIZE;
	kb2_fs_response_t *reply = &job->reply;
	struct fs_client *client = job->client;
	struct kobox_linux_exec_file *file;
	const struct cred *cred = job->cred;
	struct kstat stat;
	s64 result;

	if (r->opcode == KB2_FILESYSTEM_OP_EXEC_OPEN) {
		if (!r->path.length || r->flags || r->resolve || r->data.length ||
		    r->groups.length)
			result = -EINVAL;
		else {
			result = kobox_linux_exec_open(service->exec, &job->files, cred,
				r->client, r->handle, (const char *)bytes + r->path.offset,
				&reply->handle);
			if (!result) {
				/* A concurrently retired client cannot leave a guard that
				 * was installed after unregister's no-allocation sweep.
				 */
				mutex_lock(&service->lock);
				if (xa_load(&service->clients, r->client) != client)
					result = -EACCES;
				mutex_unlock(&service->lock);
				if (result)
					kobox_linux_exec_close(service->exec, r->client, reply->handle);
			}
		}
		goto out_client;
	}
	if (r->opcode == KB2_FILESYSTEM_OP_CLOSE) {
		result = kobox_linux_exec_close(service->exec, r->client, r->handle);
		goto out_client;
	}
	file = job->executable;
	result = file ? 0 : -EBADF;
	if (file) {
		if (r->opcode == KB2_FILESYSTEM_OP_PREAD) {
			result = r->length > capacity ? -EMSGSIZE :
				kobox_linux_exec_pread(file, cred, data, r->length, r->offset);
			if (result > 0)
				reply->data_length = result;
		} else {
			result = capacity < KB2_FILESYSTEM_STATX_SIZE ? -EMSGSIZE :
				r->flags > INT_MAX ? -EINVAL :
				kobox_linux_exec_stat(file, cred, r->flags, r->mask, &stat);
			if (!result) {
				fs_statx_encode(data, &stat);
				reply->data_length = KB2_FILESYSTEM_STATX_SIZE;
			}
		}
	}
out_client:
	return result;
}

static int fs_control_prepare(struct kobox_linux_fs_request *job,
	struct kobox_linux_fs_service *service,
	const void *request, size_t request_size, void *response,
	size_t response_capacity)
{
	kb2_fs_request_t *r = &job->decoded;
	struct fs_client *client;
	const struct cred *cred;
	u64 rights, required;
	int error;

	if (!service || !request || !response ||
	    request_size < KB2_FILESYSTEM_REQUEST_SIZE ||
	    response_capacity < KB2_FILESYSTEM_RESPONSE_SIZE)
		return -EINVAL;
	job->service = service;
	job->response = response;
	job->response_capacity = response_capacity;
	job->control = true;
	fs_profile_select(&job->profile, 0);
	fs_profile_stamp(&job->profile, 0);
	job->snapshot = kvmalloc(request_size, GFP_KERNEL);
	if (!job->snapshot) {
		u8 header[KB2_FILESYSTEM_REQUEST_SIZE];

		memcpy(header, request, sizeof(header));
		if (kb2_fs_request_decode(header, sizeof(header), request_size, r))
			return -EPROTO;
		job->reply = (kb2_fs_response_t){.opcode = r->opcode,
			.generation = r->generation, .sequence = r->sequence,
			.result = r->generation == service->generation ? -ENOMEM : -ESTALE};
		return 0;
	}
	memcpy(job->snapshot, request, request_size);
	error = kb2_fs_request_decode(job->snapshot, request_size, request_size, r);
	if (!error)
		error = kb2_fs_request_validate_paths(job->snapshot, request_size, r);
	if (error)
		return -EPROTO;
	fs_profile_stamp(&job->profile, 1);
	job->reply = (kb2_fs_response_t){.opcode = r->opcode,
		.generation = r->generation, .sequence = r->sequence};
	if (r->generation != service->generation) {
		job->reply.result = -ESTALE;
		return 0;
	}
	switch (r->opcode) {
	case KB2_FILESYSTEM_OP_EXEC_OPEN:
	case KB2_FILESYSTEM_OP_PREAD:
	case KB2_FILESYSTEM_OP_STATX:
	case KB2_FILESYSTEM_OP_CLOSE:
		if (!r->credential_generation) {
			job->reply.result = -EACCES;
			break;
		}
		fallthrough;
	case KB2_FILESYSTEM_OP_DEVICE_ROUTE:
	case KB2_FILESYSTEM_OP_TRANSFER_DUP:
		client = fs_client_get(service, r->client, r->credential_generation,
			&cred, &rights);
		job->reply.result = PTR_ERR_OR_ZERO(client);
		if (!IS_ERR(client)) {
			job->client = client;
			job->cred = cred;
			if (r->opcode == KB2_FILESYSTEM_OP_DEVICE_ROUTE) {
				kb2_fs_request_policy(r, &required);
				if (required & ~rights)
					job->reply.result = -EACCES;
			}
			if (!job->reply.result &&
			    (r->opcode == KB2_FILESYSTEM_OP_EXEC_OPEN ||
			     r->opcode == KB2_FILESYSTEM_OP_DEVICE_ROUTE ||
			     r->opcode == KB2_FILESYSTEM_OP_TRANSFER_DUP))
				job->reply.result = fs_request_bind_files(job);
			if (!job->reply.result &&
			    r->opcode == KB2_FILESYSTEM_OP_TRANSFER_DUP) {
				client = fs_client_get(service, r->second_handle, 0,
					&cred, NULL);
				job->reply.result = PTR_ERR_OR_ZERO(client);
				if (!IS_ERR(client)) {
					job->target = client;
					job->target_cred = cred;
				}
			}
			if (!job->reply.result &&
			    (r->opcode == KB2_FILESYSTEM_OP_PREAD ||
			     r->opcode == KB2_FILESYSTEM_OP_STATX ||
			     r->opcode == KB2_FILESYSTEM_OP_CLOSE)) {
				struct kobox_linux_exec_file *file;

				file = kobox_linux_exec_get(service->exec,
					r->client, r->handle);
				job->reply.result = PTR_ERR_OR_ZERO(file);
				if (!IS_ERR(file))
					job->executable = file;
			}
		}
		break;
	default:
		break;
	}
	fs_profile_stamp(&job->profile, 2);
	return 0;
}

int kobox_linux_fs_request_prepare_control(struct kobox_linux_fs_service *service,
	const void *input, size_t input_size, void *response,
	size_t response_capacity, struct kobox_linux_fs_request **request)
{
	struct kobox_linux_fs_request *job;
	int error;

	if (!request)
		return -EINVAL;
	*request = NULL;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	error = fs_control_prepare(job, service, input, input_size, response,
		response_capacity);
	if (error) {
		fs_request_release(job);
		kfree(job);
		return error;
	}
	*request = job;
	return 0;
}

static s64 fs_control_execute(struct kobox_linux_fs_request *job)
{
	struct kobox_linux_fs_service *service = job->service;
	const kb2_fs_request_t *r = &job->decoded;
	struct cred *cred;
	s64 result;

	switch (r->opcode) {
	case KB2_FILESYSTEM_OP_HELLO:
		return 0;
	case KB2_FILESYSTEM_OP_DEVICE_ROUTE:
		return fs_execute(&job->files, job->cred, r, job->snapshot,
			job->response + KB2_FILESYSTEM_RESPONSE_SIZE,
			job->response_capacity - KB2_FILESYSTEM_RESPONSE_SIZE, &job->reply);
	case KB2_FILESYSTEM_OP_EXEC_OPEN:
	case KB2_FILESYSTEM_OP_PREAD:
	case KB2_FILESYSTEM_OP_STATX:
	case KB2_FILESYSTEM_OP_CLOSE:
		return fs_exec_control(job);
	case KB2_FILESYSTEM_OP_CLIENT_REGISTER:
	case KB2_FILESYSTEM_OP_CLIENT_CREDENTIALS:
		cred = fs_credentials_decode(r, job->snapshot);
		result = PTR_ERR_OR_ZERO(cred);
		if (!IS_ERR(cred)) {
			result = r->opcode == KB2_FILESYSTEM_OP_CLIENT_REGISTER ?
				kobox_linux_fs_service_register(service, r->client,
					r->credential_generation, cred, r->capabilities) :
				kobox_linux_fs_service_credentials(service, r->client,
					r->credential_generation, cred, r->capabilities);
			put_cred(cred);
		}
		return result;
	case KB2_FILESYSTEM_OP_CLIENT_UNREGISTER:
		return kobox_linux_fs_service_unregister(service, r->client);
	case KB2_FILESYSTEM_OP_TRANSFER_DUP:
		return kobox_linux_fs_dup_to(&job->files, job->target->port,
			job->cred, r->handle, &job->reply.handle);
	default:
		return -EOPNOTSUPP;
	}
}

int kobox_linux_fs_service_control(struct kobox_linux_fs_service *service,
	const void *request, size_t request_size, void *response,
	size_t response_capacity, size_t *used)
{
	struct kobox_linux_fs_request job = {};
	int error;

	if (!used)
		return -EINVAL;
	*used = 0;
	error = fs_control_prepare(&job, service, request, request_size, response,
		response_capacity);
	if (!error)
		return kobox_linux_fs_request_execute(&job, used);
	fs_request_release(&job);

	/* Owner retirement also drops ordinary and executable file references.
	 * It has the same no-user-return boundary as the client request path.
	 */
	task_work_run();
	return error;
}
