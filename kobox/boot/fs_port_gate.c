// SPDX-License-Identifier: GPL-2.0-only
#include "host.h"
#include "fs_port.h"
#include "fs_service.h"
#include "fs_exec.h"
#include "fs_port_gate.h"
#include "fs_worker_gate.h"
#include "lifecycle_gate.h"
#include <kobox2/filesystem.h>
#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/file.h>
#include <linux/fs_context.h>
#include <linux/fs_struct.h>
#include <linux/kdev_t.h>
#include <linux/kthread.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/unaligned.h>

#define FS_RAM_BYTES (32UL << 20)
#define FS_HANDLES 10000

struct fs_gate {
	struct kobox_linux_fs_port *port;
	struct kobox_linux_fs_service *service;
	struct fs_struct *scope;
	const struct cred *creds[FS_ROLES];
	u64 sequence, credential_generation;
	int role;
};

struct fs_reader {
	struct fs_gate *gate;
	struct completion *start;
	struct completion done;
	u64 handle;
	unsigned int cpu;
	unsigned int calls;
	int error;
};

static int gate_open(void *ctx, int role, const char *name, uint64_t flags,
		     uint64_t mode, uint64_t resolve, uint64_t *handle)
{
	struct fs_gate *gate = ctx;
	struct open_how how = {.flags = flags, .mode = mode, .resolve = resolve};

	return kobox_linux_fs_open(gate->port, gate->creds[role], name, &how,
				   handle);
}

static int gate_close(void *ctx, int role, uint64_t handle)
{
	struct fs_gate *gate = ctx;

	return kobox_linux_fs_close(gate->port, gate->creds[role], handle);
}

static int gate_dup(void *ctx, int role, uint64_t handle, uint64_t *duplicate)
{
	struct fs_gate *gate = ctx;

	return kobox_linux_fs_dup(gate->port, gate->creds[role], handle, duplicate);
}

static int64_t gate_io(void *ctx, int role, uint64_t handle, void *buf,
		       size_t count, int64_t offset, int write, int positioned)
{
	struct fs_gate *gate = ctx;
	const struct cred *cred = gate->creds[role];

	if (positioned)
		return write ? kobox_linux_fs_pwrite(gate->port, cred, handle,
						    buf, count, offset) :
			       kobox_linux_fs_pread(gate->port, cred, handle,
						   buf, count, offset);
	return write ? kobox_linux_fs_write(gate->port, cred, handle, buf, count) :
		       kobox_linux_fs_read(gate->port, cred, handle, buf, count);
}

static int64_t gate_seek(void *ctx, uint64_t handle, int64_t offset, int whence)
{
	struct fs_gate *gate = ctx;

	return kobox_linux_fs_seek(gate->port, gate->creds[FS_ROOT], handle,
				   offset, whence);
}

static int gate_change(void *ctx, int role, int operation, const char *from,
		       const char *to, unsigned int flags)
{
	struct fs_gate *gate = ctx;
	const struct cred *cred = gate->creds[role];

	switch (operation) {
	case FS_TEST_MKDIR:
		return kobox_linux_fs_mkdir(gate->port, cred, from, flags);
	case FS_TEST_UNLINK:
		return kobox_linux_fs_unlink(gate->port, cred, from, flags);
	case FS_TEST_RENAME:
		return kobox_linux_fs_rename(gate->port, cred, from, to, flags);
	case FS_TEST_SYMLINK:
		return kobox_linux_fs_symlink(gate->port, cred, from, to);
	default:
		return -EINVAL;
	}
}

static s64 gate_wire_call(struct fs_gate *gate, int role, kb2_fs_request_t r,
			 const char *path, const char *second,
			 const void *input, size_t input_size,
			 void *output, size_t output_size, u64 *handle)
{
	struct kobox_linux_fs_request *prepared = NULL;
	size_t path_size = path ? strlen(path) + 1 : 0;
	size_t second_size = second ? strlen(second) + 1 : 0;
	size_t request_size = KB2_FILESYSTEM_REQUEST_SIZE + path_size +
		second_size + input_size;
	size_t response_size = KB2_FILESYSTEM_RESPONSE_SIZE + output_size;
	kb2_fs_response_t reply;
	u8 *request, *response;
	size_t used = 0, cursor = KB2_FILESYSTEM_REQUEST_SIZE;
	s64 result;

	/* This test represents one process changing its fsuid, not separate
	 * processes handing unowned handles to each other.
	 */
	if (gate->role != role) {
		result = kobox_linux_fs_service_credentials(gate->service, 1,
			++gate->credential_generation, gate->creds[role],
			KB2_FILESYSTEM_RIGHTS_VALID_MASK);
		if (result)
			return result;
		gate->role = role;
	}
	request = kmalloc(request_size, GFP_KERNEL);
	response = kmalloc(response_size, GFP_KERNEL);
	if (!request || !response) {
		result = -ENOMEM;
		goto out;
	}
	r.client = 1;
	r.generation = 1;
	r.sequence = ++gate->sequence;
	r.credential_generation = gate->credential_generation;
	/* Claimed credentials must not elevate the immutable owner state. */
	r.capabilities = U64_MAX;
	if (path_size) {
		r.path = (kb2_fs_span_t){cursor, path_size};
		memcpy(request + cursor, path, path_size);
		cursor += path_size;
	}
	if (second_size) {
		r.second_path = (kb2_fs_span_t){cursor, second_size};
		memcpy(request + cursor, second, second_size);
		cursor += second_size;
	}
	if (input_size) {
		r.data = (kb2_fs_span_t){cursor, input_size};
		memcpy(request + cursor, input, input_size);
	}
	result = kb2_fs_request_encode(request, request_size, request_size, &r);
	if (result)
		goto out;
	if (gate->scope) {
		result = kobox_linux_fs_request_prepare(gate->service, 1, request,
			request_size, response, response_size, &prepared);
		if (!result)
			result = kobox_linux_fs_request_execute_scope(prepared,
				gate->scope, &used);
		kobox_linux_fs_request_destroy(prepared);
	} else {
		result = kobox_linux_fs_service_dispatch(gate->service, 1, request,
			request_size, response, response_size, &used);
	}
	if (result)
		goto out;
	if (kb2_fs_response_decode(response, used, &reply) ||
	    !kb2_fs_response_matches(&reply, &r) ||
	    reply.data_length > output_size) {
		result = -EPROTO;
		goto out;
	}
	result = reply.result;
	if (result >= 0) {
		if (reply.data_length)
			memcpy(output, response + KB2_FILESYSTEM_RESPONSE_SIZE,
			       reply.data_length);
		if (handle)
			*handle = reply.handle;
	}
out:
	kfree(response);
	kfree(request);
	return result;
}

static int wire_openat(void *ctx, int role, u64 directory, const char *name,
		       u64 flags, u64 mode, u64 resolve, unsigned int mask,
		       u64 *handle)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
		.handle = directory, .flags = flags, .mode = mode,
		.resolve = resolve, .umask = mask};

	return gate_wire_call(ctx, role, r, name, NULL, NULL, 0, NULL, 0, handle);
}

static int wire_open(void *ctx, int role, const char *name, u64 flags,
		     u64 mode, u64 resolve, u64 *handle)
{
	return wire_openat(ctx, role, 0, name, flags, mode, resolve, 0, handle);
}

static int wire_close(void *ctx, int role, u64 handle)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};

	return gate_wire_call(ctx, role, r, NULL, NULL, NULL, 0, NULL, 0, NULL);
}

static int wire_dup(void *ctx, int role, u64 handle, u64 *duplicate)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_DUP, .handle = handle};

	return gate_wire_call(ctx, role, r, NULL, NULL, NULL, 0, NULL, 0, duplicate);
}

static s64 wire_io(void *ctx, int role, u64 handle, void *buffer, size_t count,
		   s64 offset, int write, int positioned)
{
	kb2_fs_request_t r = {.opcode = write ?
		(positioned ? KB2_FILESYSTEM_OP_PWRITE : KB2_FILESYSTEM_OP_WRITE) :
		(positioned ? KB2_FILESYSTEM_OP_PREAD : KB2_FILESYSTEM_OP_READ),
		.handle = handle, .length = count, .offset = offset};

	return gate_wire_call(ctx, role, r, NULL, NULL,
		write ? buffer : NULL, write ? count : 0,
		write ? NULL : buffer, write ? 0 : count, NULL);
}

static s64 wire_seek(void *ctx, u64 handle, s64 offset, int whence)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_LSEEK,
		.handle = handle, .offset = offset, .flags = whence};

	return gate_wire_call(ctx, FS_ROOT, r, NULL, NULL, NULL, 0, NULL, 0, NULL);
}

static s64 wire_getdents(void *ctx, u64 handle, void *buffer, unsigned int count)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_GETDENTS64,
		.handle = handle, .length = count};

	return gate_wire_call(ctx, FS_ROOT, r, NULL, NULL, NULL, 0,
			      buffer, count, NULL);
}

static int wire_stat(void *ctx, const char *name, u64 handle, int flags,
		     struct fs_test_stat *stat)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_STATX, .handle = handle,
		.flags = flags | (name ? 0 : AT_EMPTY_PATH), .mask = STATX_BASIC_STATS};
	u8 bytes[KB2_FILESYSTEM_STATX_SIZE];
	int error;

	error = gate_wire_call(ctx, FS_ROOT, r, name, NULL, NULL, 0,
			       bytes, sizeof(bytes), NULL);
	if (!error)
		*stat = (struct fs_test_stat) {
			.size = get_unaligned_le64(bytes + KB2_FILESYSTEM_STATX_SIZE_OFFSET),
			.mode = get_unaligned_le16(bytes + KB2_FILESYSTEM_STATX_MODE_OFFSET),
			.uid = get_unaligned_le32(bytes + KB2_FILESYSTEM_STATX_UID_OFFSET),
			.gid = get_unaligned_le32(bytes + KB2_FILESYSTEM_STATX_GID_OFFSET),
			.nlink = get_unaligned_le32(bytes + KB2_FILESYSTEM_STATX_NLINK_OFFSET),
			.atime = get_unaligned_le64(bytes + KB2_FILESYSTEM_STATX_ATIME_OFFSET),
			.mtime = get_unaligned_le64(bytes + KB2_FILESYSTEM_STATX_MTIME_OFFSET),
			.atime_nsec = get_unaligned_le32(bytes + KB2_FILESYSTEM_STATX_ATIME_OFFSET + 8),
			.mtime_nsec = get_unaligned_le32(bytes + KB2_FILESYSTEM_STATX_MTIME_OFFSET + 8),
		};
	return error;
}

static int wire_change(void *ctx, int role, int operation, const char *from,
		       const char *to, unsigned int flags)
{
	kb2_fs_request_t r = {0};

	switch (operation) {
	case FS_TEST_MKDIR:
		r.opcode = KB2_FILESYSTEM_OP_MKDIRAT;
		r.mode = flags;
		break;
	case FS_TEST_UNLINK:
		r.opcode = KB2_FILESYSTEM_OP_UNLINKAT;
		r.flags = flags;
		break;
	case FS_TEST_RENAME:
		r.opcode = KB2_FILESYSTEM_OP_RENAMEAT2;
		r.flags = flags;
		break;
	case FS_TEST_SYMLINK:
		r.opcode = KB2_FILESYSTEM_OP_SYMLINKAT;
		break;
	case FS_TEST_LINK:
		r.opcode = KB2_FILESYSTEM_OP_LINKAT;
		r.flags = flags;
		break;
	default:
		return -EINVAL;
	}
	return gate_wire_call(ctx, role, r, from, to, NULL, 0, NULL, 0, NULL);
}

static s64 wire_readlink(void *ctx, const char *name, void *buf, size_t count)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_READLINKAT, .length = count};

	return gate_wire_call(ctx, FS_ROOT, r, name, NULL, NULL, 0, buf, count, NULL);
}

static int wire_truncate(void *ctx, u64 handle, s64 length)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_FTRUNCATE,
		.handle = handle, .offset = length};

	return gate_wire_call(ctx, FS_ROOT, r, NULL, NULL, NULL, 0, NULL, 0, NULL);
}

static int wire_sync(void *ctx, u64 handle, int data_only)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_FSYNC,
		.handle = handle, .flags = data_only};

	return gate_wire_call(ctx, FS_ROOT, r, NULL, NULL, NULL, 0, NULL, 0, NULL);
}

static int wire_metadata(void *ctx, int role, int operation, u64 handle,
			 const char *path, u64 first, u64 second,
			 unsigned int flags)
{
	kb2_fs_request_t r = {.handle = handle, .flags = flags};

	switch (operation) {
	case FS_TEST_CHMOD:
		r.opcode = KB2_FILESYSTEM_OP_FCHMODAT;
		r.mode = first;
		break;
	case FS_TEST_CHOWN:
		r.opcode = KB2_FILESYSTEM_OP_FCHOWNAT;
		r.uid = first;
		r.gid = second;
		break;
	case FS_TEST_ACCESS:
		r.opcode = KB2_FILESYSTEM_OP_FACCESSAT2;
		r.mode = first;
		break;
	case FS_TEST_MKNOD:
		r.opcode = KB2_FILESYSTEM_OP_MKNODAT;
		r.mode = first;
		r.length = second;
		break;
	case FS_TEST_SYNCFS:
		r.opcode = KB2_FILESYSTEM_OP_SYNCFS;
		break;
	default:
		return -EINVAL;
	}
	return gate_wire_call(ctx, role, r, path, NULL, NULL, 0, NULL, 0, NULL);
}

static s64 wire_fcntl(void *ctx, u64 handle, unsigned int command, u64 argument)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_FCNTL,
		.handle = handle, .flags = command, .length = argument};

	return gate_wire_call(ctx, FS_ROOT, r, NULL, NULL, NULL, 0, NULL, 0, NULL);
}

static int wire_memfd(void *ctx, const char *name, unsigned int flags, u64 *handle)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_MEMFD_CREATE, .flags = flags};

	return gate_wire_call(ctx, FS_ROOT, r, name, NULL, NULL, 0, NULL, 0, handle);
}

static int wire_utimes(void *ctx, const char *path, const s64 *times, unsigned int flags)
{
	kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_UTIMENSAT, .flags = flags};
	u8 bytes[KB2_FILESYSTEM_TIMES_SIZE];
	unsigned int i;

	if (times)
		for (i = 0; i < 4; i++)
			put_unaligned_le64(times[i], bytes + i * sizeof(u64));
	return gate_wire_call(ctx, FS_ROOT, r, path, NULL,
		times ? bytes : NULL, times ? sizeof(bytes) : 0, NULL, 0, NULL);
}

static const struct fs_test_ops wire_ops = {
	.openat = wire_openat, .open = wire_open, .close = wire_close,
	.dup = wire_dup, .io = wire_io, .seek = wire_seek,
	.getdents = wire_getdents, .stat = wire_stat, .change = wire_change,
	.readlink = wire_readlink, .truncate = wire_truncate, .sync = wire_sync,
	.metadata = wire_metadata, .fcntl = wire_fcntl,
	.memfd = wire_memfd, .utimes = wire_utimes,
};

static struct cred *gate_cred(int role)
{
	struct group_info *groups;
	struct cred *cred = prepare_kernel_cred(current);

	if (!cred)
		return ERR_PTR(-ENOMEM);
	groups = groups_alloc(role == FS_GROUP ? 1 : 0);
	if (!groups) {
		put_cred(cred);
		return ERR_PTR(-ENOMEM);
	}
	if (role == FS_GROUP)
		groups->gid[0] = make_kgid(&init_user_ns, 2001);
	set_groups(cred, groups);
	put_group_info(groups);
	cred->uid = cred->euid = cred->suid = cred->fsuid =
		make_kuid(&init_user_ns, role ? 1000 + role : 0);
	cred->gid = cred->egid = cred->sgid = cred->fsgid =
		make_kgid(&init_user_ns, role ? 2000 + role : 0);
	if (role) {
		cred->cap_inheritable = CAP_EMPTY_SET;
		cred->cap_permitted = CAP_EMPTY_SET;
		cred->cap_effective = CAP_EMPTY_SET;
		cred->cap_ambient = CAP_EMPTY_SET;
	}
	return cred;
}

static s64 gate_authority_call(struct fs_gate *gate, bool owner, u64 route,
			       kb2_fs_request_t r, const void *tail,
			       size_t tail_size, kb2_fs_response_t *reply,
			       void *data, size_t capacity)
{
	size_t request_size = KB2_FILESYSTEM_REQUEST_SIZE + tail_size;
	size_t response_size = KB2_FILESYSTEM_RESPONSE_SIZE + capacity;
	u8 *request = kvmalloc(request_size, GFP_KERNEL);
	u8 *response = kvmalloc(response_size, GFP_KERNEL);
	size_t used = 0;
	s64 result = -ENOMEM;

	if (!request || !response)
		goto out;
	if (!r.generation)
		r.generation = 1;
	r.sequence = ++gate->sequence;
	if (tail_size)
		memcpy(request + KB2_FILESYSTEM_REQUEST_SIZE, tail, tail_size);
	result = kb2_fs_request_encode(request, request_size, request_size, &r);
	if (result)
		goto out;
	result = owner ? kobox_linux_fs_service_control(gate->service,
		request, request_size, response, response_size, &used) :
		kobox_linux_fs_service_dispatch(gate->service, route,
		request, request_size, response, response_size, &used);
	if (result)
		goto out;
	if (kb2_fs_response_decode(response, used, reply) ||
	    !kb2_fs_response_matches(reply, &r) || reply->data_length > capacity) {
		result = -EPROTO;
		goto out;
	}
	if (reply->data_length)
		memcpy(data, response + KB2_FILESYSTEM_RESPONSE_SIZE,
		       reply->data_length);
	result = reply->result;
out:
	kvfree(request);
	kvfree(response);
	return result;
}

static int gate_authority(struct fs_gate *gate)
{
	static const char private_name[] = "suite/abi-secret";
	static const char public_name[] = "suite/user";
	kb2_fs_credentials_t credentials = {
		.cap_permitted = KB2_FILESYSTEM_CREDENTIAL_CAP_VALID_MASK,
		.cap_effective = KB2_FILESYSTEM_CREDENTIAL_CAP_VALID_MASK,
		.umask = 0022,
	};
	u8 wire[KB2_FILESYSTEM_CREDENTIALS_SIZE];
	kb2_fs_response_t reply;
	kb2_fs_request_t r = {
		.opcode = KB2_FILESYSTEM_OP_CLIENT_REGISTER, .client = 10,
		.credential_generation = 1,
		.capabilities = KB2_FILESYSTEM_RIGHTS_VALID_MASK,
		.data = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(wire)},
	};
	u64 source, recipient;
	char path[64], *large;
	int error = 0;

/* A gate failure is deliberately fatal, rather than making authority tests
 * look like a successful filesystem workload with fewer checks.
 */
#define AUTH_EXPECT(call, expected) do { \
	s64 actual = (call); \
	if (actual != (expected)) { \
		pr_err("fs authority line=%u actual=%lld expected=%lld\n", \
			__LINE__, actual, (s64)(expected)); \
		return -EINVAL; \
	} \
} while (0)
	AUTH_EXPECT(kb2_fs_credentials_encode(wire, sizeof(wire), &credentials), 0);
	AUTH_EXPECT(gate_authority_call(gate, true, 0, r, wire, sizeof(wire),
		&reply, NULL, 0), 0);
	credentials = (kb2_fs_credentials_t){.uid = 1001, .euid = 1001,
		.suid = 1001, .fsuid = 1001, .gid = 2001, .egid = 2001,
		.sgid = 2001, .fsgid = 2001, .umask = 0022};
	AUTH_EXPECT(kb2_fs_credentials_encode(wire, sizeof(wire), &credentials), 0);
	r.client = 11;
	AUTH_EXPECT(gate_authority_call(gate, true, 0, r, wire, sizeof(wire),
		&reply, NULL, 0), 0);
	/* A normal data route must never turn a claimed root UID/capability or
	 * an owner opcode into an authority grant.
	 */
	AUTH_EXPECT(gate_authority_call(gate, false, 11, r, wire, sizeof(wire),
		&reply, NULL, 0), -EPERM);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_OPENAT2,
		.client = 10, .credential_generation = 1,
		.flags = O_CREAT | O_RDWR, .mode = 0600,
		.path = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(private_name)}};
	AUTH_EXPECT(gate_authority_call(gate, false, 10, r, private_name,
		sizeof(private_name), &reply, NULL, 0), 0);
	source = reply.handle;
	r.client = 11;
	r.mode = 0;
	r.flags = O_RDONLY;
	r.capabilities = U64_MAX;
	AUTH_EXPECT(gate_authority_call(gate, false, 10, r, private_name,
		sizeof(private_name), &reply, NULL, 0), -EACCES);
	AUTH_EXPECT(gate_authority_call(gate, false, 11, r, private_name,
		sizeof(private_name), &reply, NULL, 0), -EACCES);
	r.client = 10;
	r.credential_generation = 2;
	AUTH_EXPECT(gate_authority_call(gate, false, 10, r, private_name,
		sizeof(private_name), &reply, NULL, 0), -ESTALE);
	r.credential_generation = 1;
	r.generation = 2;
	AUTH_EXPECT(gate_authority_call(gate, false, 10, r, private_name,
		sizeof(private_name), &reply, NULL, 0), -ESTALE);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE,
		.client = 11, .credential_generation = 1, .handle = source};
	AUTH_EXPECT(gate_authority_call(gate, false, 11, r, NULL, 0,
		&reply, NULL, 0), -EBADF);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_OPENAT2,
		.client = 11, .credential_generation = 1, .flags = O_RDONLY,
		.path = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(public_name)}};
	AUTH_EXPECT(gate_authority_call(gate, false, 11, r, public_name,
		sizeof(public_name), &reply, NULL, 0), 0);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_TRANSFER_DUP,
		.client = 10, .second_handle = 11, .handle = source};
	AUTH_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0,
		&reply, NULL, 0), 0);
	recipient = reply.handle;
	if (recipient == source)
		return -EINVAL;
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_READLINK_HANDLE,
		.client = 11, .credential_generation = 1,
		.handle = recipient, .length = sizeof(path)};
	AUTH_EXPECT(gate_authority_call(gate, false, 11, r, NULL, 0,
		&reply, path, sizeof(path)), sizeof(private_name));
	if (memcmp(path, "/suite/abi-secret", sizeof(private_name)))
		return -EINVAL;
	/* This exceeds the retired inline capacity and stays a single Linux
	 * operation. Delegation preserves the real open file, not a reopen.
	 */
	large = kvmalloc(65536, GFP_KERNEL);
	if (!large)
		return -ENOMEM;
	memset(large, 'Q', 65536);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_WRITE,
		.client = 11, .credential_generation = 1,
		.handle = recipient, .length = 65536,
		.data = {KB2_FILESYSTEM_REQUEST_SIZE, 65536}};
	if (gate_authority_call(gate, false, 11, r, large, 65536,
		&reply, NULL, 0) != 65536)
		error = -EINVAL;
	kvfree(large);
	if (error)
		return error;
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_LSEEK,
		.client = 10, .credential_generation = 1,
		.handle = source, .flags = SEEK_CUR};
	AUTH_EXPECT(gate_authority_call(gate, false, 10, r, NULL, 0,
		&reply, NULL, 0), 65536);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_UNLINKAT,
		.client = 10, .credential_generation = 1,
		.path = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(private_name)}};
	AUTH_EXPECT(gate_authority_call(gate, false, 10, r, private_name,
		sizeof(private_name), &reply, NULL, 0), 0);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_READLINK_HANDLE,
		.client = 11, .credential_generation = 1,
		.handle = recipient, .length = sizeof(path)};
	AUTH_EXPECT(gate_authority_call(gate, false, 11, r, NULL, 0,
		&reply, path, sizeof(path)), sizeof("/suite/abi-secret (deleted)") - 1);
	if (memcmp(path, "/suite/abi-secret (deleted)",
		   sizeof("/suite/abi-secret (deleted)") - 1))
		return -EINVAL;
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLIENT_UNREGISTER,
		.client = 10};
	AUTH_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0,
		&reply, NULL, 0), 0);
	r.opcode = KB2_FILESYSTEM_OP_CLIENT_REGISTER;
	r.credential_generation = 1;
	r.capabilities = KB2_FILESYSTEM_RIGHTS_VALID_MASK;
	r.data = (kb2_fs_span_t){KB2_FILESYSTEM_REQUEST_SIZE, sizeof(wire)};
	AUTH_EXPECT(gate_authority_call(gate, true, 0, r, wire, sizeof(wire),
		&reply, NULL, 0), -EEXIST);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PREAD,
		.client = 11, .credential_generation = 1,
		.handle = recipient, .length = sizeof(path)};
	AUTH_EXPECT(gate_authority_call(gate, false, 11, r, NULL, 0,
		&reply, path, sizeof(path)), sizeof(path));
	if (path[0] != 'Q' || path[sizeof(path) - 1] != 'Q')
		return -EINVAL;
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLIENT_UNREGISTER,
		.client = 11};
	AUTH_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0,
		&reply, NULL, 0), 0);
#undef AUTH_EXPECT
	return 0;
}

static int gate_capability_policy(struct fs_gate *gate)
{
	static const char existing[] = "suite/user";
	static const char forbidden[] = "suite/capability-denied";
	kb2_fs_response_t reply;
	kb2_fs_request_t r;
	u8 stat[KB2_FILESYSTEM_STATX_SIZE], data[8];
	u64 handle;
	u64 rights = KB2_FILESYSTEM_RIGHT_LOOKUP | KB2_FILESYSTEM_RIGHT_READ |
		KB2_FILESYSTEM_RIGHT_STAT;

/* Root Linux credentials deliberately make DAC permissive here. A denial
 * must therefore come from the registered filesystem capability policy,
 * not coincidentally from inode permissions or a read-only descriptor.
 */
#define POLICY_EXPECT(call, expected) do { \
	s64 actual = (call); \
	if (actual != (expected)) { \
		pr_err("fs capability policy line=%u actual=%lld expected=%lld\n", \
			__LINE__, actual, (s64)(expected)); \
		return -EINVAL; \
	} \
} while (0)
	POLICY_EXPECT(kobox_linux_fs_service_register(gate->service, 12, 1,
		gate->creds[FS_ROOT], BIT_ULL(40)), -EINVAL);
	POLICY_EXPECT(kobox_linux_fs_service_register(gate->service, 12, 1,
		gate->creds[FS_ROOT], rights), 0);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_OPENAT2,
		.flags = O_CREAT | O_EXCL | O_RDWR, .mode = 0600,
		.capabilities = U64_MAX,
		.path = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(forbidden)}};
	/* Zero claims must select client 12, never the privileged owner. */
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, forbidden,
		sizeof(forbidden), &reply, NULL, 0), -EACCES);
	r.opcode = KB2_FILESYSTEM_OP_STATX;
	r.flags = 0;
	r.mask = STATX_BASIC_STATS;
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, forbidden,
		sizeof(forbidden), &reply, stat, sizeof(stat)), -ENOENT);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_OPENAT2,
		.flags = O_RDONLY,
		.path = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(existing)}};
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, existing,
		sizeof(existing), &reply, NULL, 0), 0);
	handle = reply.handle;
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PWRITE,
		.handle = handle, .length = sizeof(data), .capabilities = U64_MAX,
		.data = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(data)}};
	memset(data, 'X', sizeof(data));
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, data,
		sizeof(data), &reply, NULL, 0), -EACCES);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_SESSION_ATTACH};
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, NULL, 0,
		&reply, NULL, 0), -EPERM);
	r.opcode = KB2_FILESYSTEM_OP_EXEC_OPEN;
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, NULL, 0,
		&reply, NULL, 0), -EPERM);
	POLICY_EXPECT(kobox_linux_fs_service_credentials(gate->service, 12, 2,
		gate->creds[FS_ROOT], BIT_ULL(40)), -EINVAL);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_STATX,
		.client = 12, .credential_generation = 1, .handle = handle,
		.flags = AT_EMPTY_PATH, .mask = STATX_BASIC_STATS};
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, NULL, 0,
		&reply, stat, sizeof(stat)), 0);
	POLICY_EXPECT(kobox_linux_fs_service_credentials(gate->service, 12, 2,
		gate->creds[FS_ROOT], KB2_FILESYSTEM_RIGHT_STAT), 0);
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, NULL, 0,
		&reply, stat, sizeof(stat)), -ESTALE);
	r.client = 0;
	r.credential_generation = 0;
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, NULL, 0,
		&reply, stat, sizeof(stat)), 0);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PREAD,
		.handle = handle, .length = sizeof(data), .capabilities = U64_MAX};
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, NULL, 0,
		&reply, data, sizeof(data)), -EACCES);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, NULL, 0,
		&reply, NULL, 0), 0);
	POLICY_EXPECT(kobox_linux_fs_service_unregister(gate->service, 12), 0);
	POLICY_EXPECT(gate_authority_call(gate, false, 12, r, NULL, 0,
		&reply, NULL, 0), -EACCES);
#undef POLICY_EXPECT
	pr_info("fs capability policy: root DAC/registered rights/forged claims/owner rejection/update/retirement PASS\n");
	return 0;
}

static int gate_device_route(struct fs_gate *gate)
{
	static const char device[] = "suite/route-device";
	static const char regular[] = "suite/route-regular";
	static const char contents[] = "routing must never truncate this";
	struct cred *different_fsuid;
	kb2_fs_response_t reply;
	kb2_fs_request_t request;
	u8 stat[KB2_FILESYSTEM_STATX_SIZE];
	char readback[sizeof(contents)];
	u64 handle;
	int error;

#define ROUTE_EXPECT(call, expected) do { \
	s64 actual = (call); \
	if (actual != (expected)) { \
		pr_err("fs device route line=%u actual=%lld expected=%lld\n", \
			__LINE__, actual, (s64)(expected)); \
		return -EINVAL; \
	} \
} while (0)
	ROUTE_EXPECT(wire_metadata(gate, FS_ROOT, FS_TEST_MKNOD, 0, device,
		S_IFCHR | 0600, new_encode_dev(MKDEV(250, 0)), 0), 0);
	ROUTE_EXPECT(kobox_linux_fs_service_register(gate->service, 13, 1,
		gate->creds[FS_ROOT], KB2_FILESYSTEM_RIGHTS_VALID_MASK), 0);
	request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_DEVICE_ROUTE,
		.client = 13, .credential_generation = 1, .flags = O_RDONLY,
		.path = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(device)}};
	ROUTE_EXPECT(gate_authority_call(gate, false, 13, request, device,
		sizeof(device), &reply, stat, sizeof(stat)), -EPERM);
	/* An unregistered device major has no driver to open. Classification
	 * must still succeed without invoking that driver's open method. */
	ROUTE_EXPECT(gate_authority_call(gate, true, 0, request, device,
		sizeof(device), &reply, stat, sizeof(stat)), 0);
	handle = reply.handle;
	if (!handle || get_unaligned_le32(stat + KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET) != 250)
		return -EINVAL;
	ROUTE_EXPECT(gate_authority_call(gate, false, 13,
		(kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle},
		NULL, 0, &reply, NULL, 0), 0);
	ROUTE_EXPECT(kobox_linux_fs_service_credentials(gate->service, 13, 2,
		gate->creds[FS_ROOT], KB2_FILESYSTEM_RIGHT_LOOKUP | KB2_FILESYSTEM_RIGHT_READ), 0);
	request.credential_generation = 2;
	request.flags = O_CREAT | O_TRUNC | O_RDWR;
	request.mode = 0600;
	ROUTE_EXPECT(gate_authority_call(gate, true, 0, request, device,
		sizeof(device), &reply, stat, sizeof(stat)), -EACCES);
	/* Deliberately make effective IDs differ from filesystem IDs. A
	 * faccessat(AT_EACCESS) shortcut would wrongly authorize this request. */
	different_fsuid = prepare_creds();
	if (!different_fsuid)
		return -ENOMEM;
	different_fsuid->euid = GLOBAL_ROOT_UID;
	different_fsuid->egid = GLOBAL_ROOT_GID;
	different_fsuid->fsuid = gate->creds[FS_USER]->fsuid;
	different_fsuid->fsgid = gate->creds[FS_USER]->fsgid;
	different_fsuid->cap_effective = CAP_EMPTY_SET;
	error = kobox_linux_fs_service_credentials(gate->service, 13, 3,
		different_fsuid, KB2_FILESYSTEM_RIGHTS_VALID_MASK);
	put_cred(different_fsuid);
	ROUTE_EXPECT(error, 0);
	request.credential_generation = 3;
	request.flags = O_RDONLY;
	request.mode = 0;
	ROUTE_EXPECT(gate_authority_call(gate, true, 0, request, device,
		sizeof(device), &reply, stat, sizeof(stat)), -EACCES);
	ROUTE_EXPECT(wire_metadata(gate, FS_ROOT, FS_TEST_CHMOD, 0, device, 0666, 0, 0), 0);
	ROUTE_EXPECT(gate_authority_call(gate, true, 0, request, device,
		sizeof(device), &reply, stat, sizeof(stat)), 0);
	handle = reply.handle;
	ROUTE_EXPECT(gate_authority_call(gate, false, 13,
		(kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle},
		NULL, 0, &reply, NULL, 0), 0);
	ROUTE_EXPECT(wire_open(gate, FS_ROOT, regular, O_CREAT | O_EXCL | O_RDWR,
		0644, 0, &handle), 0);
	ROUTE_EXPECT(gate_wire_call(gate, FS_ROOT, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_WRITE, .handle = handle, .length = sizeof(contents)},
		NULL, NULL, contents, sizeof(contents), NULL, 0, NULL), sizeof(contents));
	request.flags = O_CREAT | O_TRUNC | O_RDWR;
	request.mode = 0600;
	request.path.length = sizeof(regular);
	ROUTE_EXPECT(gate_authority_call(gate, true, 0, request, regular,
		sizeof(regular), &reply, stat, sizeof(stat)), -ENODEV);
	ROUTE_EXPECT(gate_wire_call(gate, FS_ROOT, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_PREAD, .handle = handle, .length = sizeof(contents)},
		NULL, NULL, NULL, 0, readback, sizeof(readback), NULL), sizeof(contents));
	if (memcmp(readback, contents, sizeof(contents)))
		return -EINVAL;
	ROUTE_EXPECT(wire_close(gate, FS_ROOT, handle), 0);
	ROUTE_EXPECT(kobox_linux_fs_service_unregister(gate->service, 13), 0);
#undef ROUTE_EXPECT
	pr_info("fs device route: owner-only/original capabilities/fsuid DAC/no driver open/no ordinary-file truncate PASS\n");
	return 0;
}

static int gate_global_sync(struct fs_gate *gate)
{
	const struct cred *before = current_cred();
	static const char name[] = "suite/global-sync";
	static const char contents[] = "dirty data before global sync";
	kb2_fs_response_t reply;
	char readback[sizeof(contents)];
	u64 writer = 0, reader = 0, path = 0;
	int error = 0;

#define SYNC_EXPECT(call, expected) do { \
	s64 actual = (call); \
	if (actual != (expected) || current_cred() != before) { \
		pr_err("fs sync line=%u actual=%lld expected=%lld credentials_restored=%u\n", \
			__LINE__, actual, (s64)(expected), current_cred() == before); \
		error = -EINVAL; goto out; \
	} \
} while (0)
	SYNC_EXPECT(wire_open(gate, FS_ROOT, name, O_CREAT | O_EXCL | O_RDWR,
			      0644, 0, &writer), 0);
	SYNC_EXPECT(gate_wire_call(gate, FS_ROOT, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_PWRITE, .handle = writer,
		.length = sizeof(contents)}, NULL, NULL, contents,
		sizeof(contents), NULL, 0, NULL), sizeof(contents));
	/* This invokes actual upstream global writeback after dirtying a real
	 * file, with an unprivileged actor and no invented file descriptor. */
	SYNC_EXPECT(gate_wire_call(gate, FS_USER, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_SYNC}, NULL, NULL, NULL, 0,
		NULL, 0, NULL), 0);
	SYNC_EXPECT(gate_authority_call(gate, false, 1, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_SYNC, .client = 1,
		.credential_generation = gate->credential_generation + 1},
		NULL, 0, &reply, NULL, 0), -ESTALE);
	SYNC_EXPECT(gate_authority_call(gate, false, 1, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_SYNC, .client = 2,
		.credential_generation = gate->credential_generation},
		NULL, 0, &reply, NULL, 0), -EACCES);
	SYNC_EXPECT(wire_open(gate, FS_USER, name, O_RDONLY, 0, 0, &reader), 0);
	SYNC_EXPECT(gate_wire_call(gate, FS_USER, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_SYNCFS, .handle = reader}, NULL,
		NULL, NULL, 0, NULL, 0, NULL), 0);
	SYNC_EXPECT(gate_wire_call(gate, FS_USER, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_PREAD, .handle = reader,
		.length = sizeof(readback)}, NULL, NULL, NULL, 0,
		readback, sizeof(readback), NULL), sizeof(readback));
	if (memcmp(readback, contents, sizeof(contents))) {
		error = -EINVAL;
		goto out;
	}
	SYNC_EXPECT(gate_wire_call(gate, FS_USER, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_SYNCFS}, NULL, NULL, NULL, 0,
		NULL, 0, NULL), -EBADF);
	SYNC_EXPECT(wire_open(gate, FS_USER, name, O_PATH, 0, 0, &path), 0);
	SYNC_EXPECT(gate_wire_call(gate, FS_USER, (kb2_fs_request_t){
		.opcode = KB2_FILESYSTEM_OP_SYNCFS, .handle = path}, NULL,
		NULL, NULL, 0, NULL, 0, NULL), -EBADF);
	SYNC_EXPECT(kobox_linux_fs_sync(NULL), -EINVAL);
	pr_info("fs sync: actual global sync/read-only syncfs/invalid handle/credentials restore PASS\n");
out:
	if (path)
		wire_close(gate, FS_ROOT, path);
	if (reader)
		wire_close(gate, FS_ROOT, reader);
	if (writer)
		wire_close(gate, FS_ROOT, writer);
	wire_change(gate, FS_ROOT, FS_TEST_UNLINK, name, NULL, 0);
#undef SYNC_EXPECT
	return error;
}

static void gate_drain(void)
{
	task_work_run();
	flush_delayed_fput();
	rcu_barrier();
}

static int gate_close_return(struct fs_gate *gate)
{
	static const char name[] = "suite/close-return";
	kb2_fs_response_t reply;
	kb2_fs_request_t request;
	u64 writer = 0, executable = 0;
	unsigned int iteration;
	s64 actual;
	int error = 0;

	/* Earlier direct-port tests deliberately drain at teardown. This case
	 * must use only production request boundaries: PID 1 never returns to
	 * Linux userspace, so a queued final fput would otherwise retain writer
	 * authority and unlinked tmpfs inodes for the entire service lifetime.
	 */
	gate_drain();
	for (iteration = 0; iteration < 1024; iteration++) {
		error = wire_open(gate, FS_ROOT, name,
			O_CREAT | O_EXCL | O_WRONLY, 0755, 0, &writer);
		if (error)
			goto out;
		error = wire_close(gate, FS_ROOT, writer);
		writer = 0;
		if (error)
			goto out;
		request = (kb2_fs_request_t) {
			.opcode = KB2_FILESYSTEM_OP_EXEC_OPEN, .client = 1,
			.credential_generation = gate->credential_generation,
			.path = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(name)},
		};
		actual = gate_authority_call(gate, true, 0, request,
			name, sizeof(name), &reply, NULL, 0);
		if (actual) {
			pr_err("fs close-return: iteration=%u exec=%lld task_work_pending=%u\n",
				iteration, actual, task_work_pending(current));
			error = -EINVAL;
			goto out;
		}
		executable = reply.handle;
		request.opcode = KB2_FILESYSTEM_OP_CLOSE;
		request.handle = executable;
		request.path = (kb2_fs_span_t){};
		error = gate_authority_call(gate, true, 0, request,
			NULL, 0, &reply, NULL, 0);
		executable = 0;
		if (error)
			goto out;
		error = wire_open(gate, FS_ROOT, name, O_WRONLY, 0, 0,
			&writer);
		if (error)
			goto out;
		error = wire_change(gate, FS_ROOT, FS_TEST_UNLINK, name,
			NULL, 0);
		if (error)
			goto out;
		error = wire_close(gate, FS_ROOT, writer);
		writer = 0;
		if (error || task_work_pending(current)) {
			error = error ?: -EINVAL;
			goto out;
		}
	}
	pr_info("fs close-return: 1024 write-close/exec-close/reopen/unlink cycles without external drain PASS\n");
out:
	if (writer)
		wire_close(gate, FS_ROOT, writer);
	if (executable) {
		request = (kb2_fs_request_t) {
			.opcode = KB2_FILESYSTEM_OP_CLOSE, .client = 1,
			.credential_generation = gate->credential_generation,
			.handle = executable,
		};
		gate_authority_call(gate, true, 0, request, NULL, 0,
			&reply, NULL, 0);
	}
	wire_change(gate, FS_ROOT, FS_TEST_UNLINK, name, NULL, 0);
	return error;
}

struct fs_exec_reader {
	struct kobox_linux_exec_table *table;
	const struct cred *cred;
	struct completion acquired, read, release, done;
	u64 handle;
	int error;
};

static int gate_exec_reader(void *argument)
{
	struct fs_exec_reader *reader = argument;
	struct kobox_linux_exec_file *file;
	char bytes[8];

	file = kobox_linux_exec_get(reader->table, 20, reader->handle);
	reader->error = PTR_ERR_OR_ZERO(file);
	complete(&reader->acquired);
	wait_for_completion(&reader->read);
	if (!IS_ERR(file)) {
		if (kobox_linux_exec_pread(file, reader->cred, bytes,
					  sizeof(bytes), 0) != sizeof(bytes) ||
		    memcmp(bytes, "EXECHERE", sizeof(bytes)))
			reader->error = -EINVAL;
	}
	complete(&reader->done);
	wait_for_completion(&reader->release);
	if (!IS_ERR(file))
		kobox_linux_exec_put(file);
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int gate_exec(struct fs_gate *gate, struct vfsmount *mnt)
{
	static const char name[] = "suite/exec-only";
	struct kobox_linux_exec_table *table = NULL;
	struct kobox_linux_exec_file *pin = NULL;
	struct fs_exec_reader reader = {};
	struct task_struct *task = NULL;
	kb2_fs_response_t reply;
	kb2_fs_request_t r;
	u8 stat[KB2_FILESYSTEM_STATX_SIZE];
	char bytes[8];
	u64 ordinary = 0, directory = 0, handle = 0, private_handle = 0;
	int error = 0;
	unsigned int mount_flags = mnt->mnt_flags;
	bool flags_changed = false;
	const char *stage = "fixture/owner namespace";

#define EXEC_EXPECT(call, expected) do { \
	s64 actual = (call); \
	if (actual != (expected)) { \
		pr_err("fs exec line=%u actual=%lld expected=%lld\n", \
			__LINE__, actual, (s64)(expected)); \
		error = -EINVAL; goto out; \
	} \
} while (0)
#define EXEC_POINTER(pointer) do { \
	if (IS_ERR(pointer)) { \
		error = PTR_ERR(pointer); pointer = NULL; \
		pr_err("fs exec pointer=" #pointer " line=%u stage=%s error=%d\n", \
			__LINE__, stage, error); goto out; \
	} \
} while (0)
	EXEC_EXPECT(gate_open(gate, FS_ROOT, name, O_CREAT | O_EXCL | O_WRONLY,
			     0111, 0, &ordinary), 0);
	EXEC_EXPECT(gate_io(gate, FS_ROOT, ordinary, "EXECHERE", 8, 0, 1, 1), 8);
	EXEC_EXPECT(gate_close(gate, FS_ROOT, ordinary), 0); ordinary = 0;
	gate_drain();
	EXEC_EXPECT(gate_open(gate, FS_USER, name, O_RDONLY, 0, 0, &ordinary), -EACCES);
	EXEC_EXPECT(kobox_linux_fs_service_register(gate->service, 20, 1,
						   gate->creds[FS_USER],
						   KB2_FILESYSTEM_RIGHTS_VALID_MASK), 0);
	EXEC_EXPECT(kobox_linux_fs_service_register(gate->service, 21, 1,
						   gate->creds[FS_USER],
						   KB2_FILESYSTEM_RIGHTS_VALID_MASK), 0);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_EXEC_OPEN,
		.client = 20, .credential_generation = 1,
		.path = {KB2_FILESYSTEM_REQUEST_SIZE, sizeof(name)}};
	EXEC_EXPECT(gate_authority_call(gate, false, 20, r, name, sizeof(name),
				       &reply, NULL, 0), -EPERM);
	r.credential_generation = 2;
	EXEC_EXPECT(gate_authority_call(gate, true, 0, r, name, sizeof(name),
				       &reply, NULL, 0), -ESTALE);
	r.credential_generation = 1;
	EXEC_EXPECT(gate_authority_call(gate, true, 0, r, name, sizeof(name),
				       &reply, NULL, 0), 0);
	private_handle = reply.handle;
	EXEC_EXPECT(gate_open(gate, FS_ROOT, name, O_WRONLY, 0, 0, &ordinary), -ETXTBSY);
	r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PREAD,
		.client = 20, .credential_generation = 1, .handle = private_handle, .length = 8};
	EXEC_EXPECT(gate_authority_call(gate, false, 20, r, NULL, 0, &reply, bytes, 8), -EBADF);
	EXEC_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0, &reply, bytes, 8), 8);
	if (memcmp(bytes, "EXECHERE", 8)) { error = -EINVAL; goto out; }
	r.client = 21;
	EXEC_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0, &reply, bytes, 8), -EBADF);
	r.client = 20; r.opcode = KB2_FILESYSTEM_OP_DUP;
	EXEC_EXPECT(gate_authority_call(gate, false, 20, r, NULL, 0, &reply, NULL, 0), -EBADF);
	EXEC_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0, &reply, NULL, 0), -EOPNOTSUPP);
	r.opcode = KB2_FILESYSTEM_OP_TRANSFER_DUP; r.second_handle = 21;
	EXEC_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0, &reply, NULL, 0), -EBADF);
	r.opcode = KB2_FILESYSTEM_OP_STATX; r.flags = AT_EMPTY_PATH; r.mask = STATX_BASIC_STATS;
	EXEC_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0, &reply, stat, sizeof(stat)), 0);
	if (get_unaligned_le64(stat + KB2_FILESYSTEM_STATX_SIZE_OFFSET) != 8) {
		error = -EINVAL; goto out;
	}
	r.opcode = KB2_FILESYSTEM_OP_CLOSE;
	EXEC_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0, &reply, NULL, 0), 0);
	EXEC_EXPECT(gate_authority_call(gate, true, 0, r, NULL, 0, &reply, NULL, 0), -EBADF);
	private_handle = 0;
	gate_drain();
	EXEC_EXPECT(gate_open(gate, FS_ROOT, name, O_WRONLY, 0, 0, &ordinary), 0);
	stage = "direct guard allocation";
	table = kobox_linux_exec_create();
	EXEC_POINTER(table);
	EXEC_EXPECT(kobox_linux_exec_open(table, gate->port, gate->creds[FS_USER],
					20, 0, name, &handle), -ETXTBSY);
	EXEC_EXPECT(gate_close(gate, FS_ROOT, ordinary), 0); ordinary = 0;
	gate_drain();
	kobox_linux_exec_gate_fail_next(table, KOBOX_EXEC_GATE_ALLOC);
	EXEC_EXPECT(kobox_linux_exec_open(table, gate->port, gate->creds[FS_USER],
					20, 0, name, &handle), -ENOMEM);
	kobox_linux_exec_gate_fail_next(table, KOBOX_EXEC_GATE_INSTALL);
	EXEC_EXPECT(kobox_linux_exec_open(table, gate->port, gate->creds[FS_USER],
					20, 0, name, &handle), -ENOMEM);
	gate_drain();
	EXEC_EXPECT(gate_open(gate, FS_ROOT, name, O_WRONLY, 0, 0, &ordinary), 0);
	EXEC_EXPECT(gate_close(gate, FS_ROOT, ordinary), 0); ordinary = 0; gate_drain();
	EXEC_EXPECT(gate_open(gate, FS_USER, "suite", O_PATH | O_DIRECTORY, 0, 0, &directory), 0);
	EXEC_EXPECT(kobox_linux_exec_open(table, gate->port, gate->creds[FS_USER],
					20, directory, "exec-only", &handle), 0);
	reader.table = table; reader.cred = gate->creds[FS_USER]; reader.handle = handle;
	stage = "SMP close/read";
	init_completion(&reader.acquired); init_completion(&reader.read);
	init_completion(&reader.release); init_completion(&reader.done);
	task = kthread_run(gate_exec_reader, &reader, "fs-exec/close");
	EXEC_POINTER(task);
	wait_for_completion(&reader.acquired);
	EXEC_EXPECT(reader.error, 0);
	EXEC_EXPECT(kobox_linux_exec_close(table, 20, handle), 0);
	EXEC_EXPECT(PTR_ERR(kobox_linux_exec_get(table, 20, handle)), -EBADF);
	EXEC_EXPECT(gate_open(gate, FS_ROOT, name, O_WRONLY, 0, 0, &ordinary), -ETXTBSY);
	complete(&reader.read); wait_for_completion(&reader.done);
	EXEC_EXPECT(reader.error, 0);
	EXEC_EXPECT(gate_open(gate, FS_ROOT, name, O_WRONLY, 0, 0, &ordinary), -ETXTBSY);
	complete(&reader.release); kthread_stop(task); task = NULL;
	gate_drain();
	EXEC_EXPECT(gate_open(gate, FS_ROOT, name, O_WRONLY, 0, 0, &ordinary), 0);
	EXEC_EXPECT(gate_close(gate, FS_ROOT, ordinary), 0); ordinary = 0; gate_drain();
	EXEC_EXPECT(kobox_linux_exec_open(table, gate->port, gate->creds[FS_USER],
					20, 0, name, &handle), 0);
	stage = "pinned retirement";
	pin = kobox_linux_exec_get(table, 20, handle);
	EXEC_POINTER(pin);
	kobox_linux_exec_release_client(table, 20);
	EXEC_EXPECT(kobox_linux_exec_close(table, 20, handle), -EBADF);
	EXEC_EXPECT(gate_open(gate, FS_ROOT, name, O_WRONLY, 0, 0, &ordinary), -ETXTBSY);
	kobox_linux_exec_destroy(table); table = NULL;
	EXEC_EXPECT(kobox_linux_exec_pread(pin, gate->creds[FS_USER], bytes, 8, 0), 8);
	kobox_linux_exec_put(pin); pin = NULL; gate_drain();
	EXEC_EXPECT(gate_open(gate, FS_ROOT, name, O_WRONLY, 0, 0, &ordinary), 0);
	EXEC_EXPECT(gate_close(gate, FS_ROOT, ordinary), 0); ordinary = 0; gate_drain();
	stage = "noexec mount";
	table = kobox_linux_exec_create();
	EXEC_POINTER(table);
	/* kern_mount produces a namespace-less internal fixture which Linux
	 * deliberately refuses to clone_private_mount. All fixture requests
	 * are quiesced and the read kthread has joined: temporarily change this
	 * test-owned mount's real noexec flag and restore it on every exit. */
	mnt->mnt_flags = mount_flags | MNT_NOEXEC;
	flags_changed = true;
	EXEC_EXPECT(kobox_linux_exec_open(table, gate->port, gate->creds[FS_ROOT],
					20, 0, name, &handle), -EACCES);
	mnt->mnt_flags = mount_flags;
	flags_changed = false;
	EXEC_EXPECT(kobox_linux_fs_chmodat(gate->port, gate->creds[FS_ROOT], 0, name, 0644, 0), 0);
	EXEC_EXPECT(kobox_linux_exec_open(table, gate->port, gate->creds[FS_ROOT],
					20, 0, name, &handle), -EACCES);
	pr_info("fs exec guard: execute-only/noexec/ETXTBSY/private namespace/SMP close/pin retirement/controlled ENOMEM unwind PASS\n");
out:
	if (error)
		pr_err("fs exec failed stage=%s error=%d\n", stage, error);
	if (flags_changed)
		mnt->mnt_flags = mount_flags;
	if (task) {
		complete_all(&reader.read); complete_all(&reader.release); kthread_stop(task);
	}
	if (pin) kobox_linux_exec_put(pin);
	if (table) kobox_linux_exec_destroy(table);
	if (ordinary) gate_close(gate, FS_ROOT, ordinary);
	if (directory) gate_close(gate, FS_ROOT, directory);
	if (private_handle) {
		r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE,
			.client = 20, .credential_generation = 1, .handle = private_handle};
		gate_authority_call(gate, true, 0, r, NULL, 0, &reply, NULL, 0);
	}
	kobox_linux_fs_service_unregister(gate->service, 20);
	kobox_linux_fs_service_unregister(gate->service, 21);
	gate_change(gate, FS_ROOT, FS_TEST_UNLINK, name, NULL, 0);
	gate_drain();
#undef EXEC_EXPECT
#undef EXEC_POINTER
	return error;
}

static int gate_handles(struct fs_gate *gate, struct kobox_fs_port_report *report)
{
	uint64_t *handles, reopened;
	unsigned int i;
	int error;

	handles = kcalloc(FS_HANDLES, sizeof(*handles), GFP_KERNEL);
	if (!handles)
		return -ENOMEM;
	for (i = 0; i < FS_HANDLES; i++) {
		error = gate_open(gate, FS_ROOT, "suite/b", O_RDONLY, 0, 0,
				  &handles[i]);
		if (error)
			goto out;
	}
	for (i = 0; i < FS_HANDLES; i++) {
		error = gate_close(gate, FS_ROOT, handles[i]);
		if (error)
			goto out;
	}
	error = gate_open(gate, FS_ROOT, "suite/b", O_RDONLY, 0, 0, &reopened);
	if (error)
		goto out;
	if (reopened <= handles[FS_HANDLES - 1] ||
	    gate_close(gate, FS_ROOT, handles[0]) != -EBADF)
		error = -EINVAL;
	gate_close(gate, FS_ROOT, reopened);
	if (!error)
		report->handles += FS_HANDLES;
out:
	kfree(handles);
	return error;
}

static int gate_reader(void *argument)
{
	struct fs_reader *reader = argument;
	char buffer[64];
	unsigned int i;
	ssize_t result;

	wait_for_completion(reader->start);
	reader->cpu = raw_smp_processor_id();
	for (i = 0; i < 128; i++) {
		result = gate_io(reader->gate, FS_ROOT, reader->handle, buffer,
				 sizeof(buffer), 0, 0, 0);
		if (result != sizeof(buffer) || memchr_inv(buffer, 'Q', sizeof(buffer))) {
			reader->error = result < 0 ? result : -EINVAL;
			break;
		}
		reader->calls++;
		cond_resched();
	}
	complete(&reader->done);
	/* kthread_stop owns the final task reference; do not exit before it. */
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int gate_close_reader(void *argument)
{
	struct fs_reader *reader = argument;
	const struct cred *before = current_cred();
	char byte;
	unsigned int i;
	ssize_t result;

	for (i = 0; i < 512; i++) {
		wait_for_completion(reader->start);
		result = gate_io(reader->gate, FS_ROOT, reader->handle, &byte,
				 1, 0, 0, 1);
		if ((result != 1 || byte != 'B') && result != -EBADF)
			reader->error = -EINVAL;
		if (current_cred() != before)
			reader->error = -EINVAL;
		complete(&reader->done);
	}
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int gate_close_race(struct fs_gate *gate,
			   struct kobox_fs_port_report *report)
{
	struct completion start;
	struct fs_reader reader = {.gate = gate, .start = &start};
	struct task_struct *task;
	unsigned int i;
	int error = 0, result;

	init_completion(&start);
	init_completion(&reader.done);
	task = kthread_create(gate_close_reader, &reader, "fs-port/close");
	if (IS_ERR(task))
		return PTR_ERR(task);
	kthread_bind(task, 0);
	wake_up_process(task);
	for (i = 0; i < 512; i++) {
		result = gate_open(gate, FS_ROOT, "suite/b", O_RDONLY, 0, 0,
				   &reader.handle);
		if (result) {
			error = result;
			reader.handle = 0;
		}
		complete(&start);
		/* Either the lookup already pinned the file or it sees EBADF.
		 * No table lock may cover I/O, even when final fput is deferred.
		 */
		if (!result && gate_close(gate, FS_ROOT, reader.handle))
			error = -EINVAL;
		wait_for_completion(&reader.done);
		report->close_races++;
	}
	kthread_stop(task);
	return error ?: reader.error;
}

static int gate_shared_offset(struct fs_gate *gate,
			      struct kobox_fs_port_report *report)
{
	struct completion start;
	struct fs_reader readers[2] = {};
	struct task_struct *tasks[2] = {};
	u64 handles[2] = {};
	char *buffer;
	unsigned int i;
	int error;

	buffer = kmalloc(16384, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	memset(buffer, 'Q', 16384);
	error = gate_open(gate, FS_ROOT, "suite/parallel", O_CREAT | O_RDWR,
			  0600, 0, &handles[0]);
	if (error)
		goto out;
	if (gate_io(gate, FS_ROOT, handles[0], buffer, 16384, 0, 1, 1) != 16384) {
		error = -EINVAL;
		goto out;
	}
	error = gate_dup(gate, FS_ROOT, handles[0], &handles[1]);
	if (error)
		goto out;
	init_completion(&start);
	for (i = 0; i < 2; i++) {
		readers[i].gate = gate;
		readers[i].start = &start;
		readers[i].handle = handles[i];
		init_completion(&readers[i].done);
		tasks[i] = kthread_create(gate_reader, &readers[i], "fs-port/%u", i);
		if (IS_ERR(tasks[i])) {
			error = PTR_ERR(tasks[i]);
			tasks[i] = NULL;
			break;
		}
		kthread_bind(tasks[i], i);
		wake_up_process(tasks[i]);
	}
	complete_all(&start);
	for (i = 0; i < 2; i++) {
		if (!tasks[i])
			continue;
		wait_for_completion(&readers[i].done);
		kthread_stop(tasks[i]);
		if (readers[i].error)
			error = readers[i].error;
		report->shared_reads += readers[i].calls;
		report->cpu_mask |= 1U << readers[i].cpu;
	}
	if (!error && gate_seek(gate, handles[0], 0, SEEK_CUR) != 16384)
		error = -EINVAL;
out:
	for (i = 0; i < 2; i++)
		if (handles[i])
			gate_close(gate, FS_ROOT, handles[i]);
	gate_change(gate, FS_ROOT, FS_TEST_UNLINK, "suite/parallel", NULL, 0);
	kfree(buffer);
	return error;
}

static int gate_filesystem(struct fs_gate *gate, struct vfsmount *mnt,
			   struct fs_test_report *results,
			   struct kobox_fs_port_report *report)
{
	const struct cred *before = current_cred();
	int error;

	gate->port = kobox_linux_fs_create(mnt);
	if (IS_ERR(gate->port))
		return PTR_ERR(gate->port);
	gate->service = kobox_linux_fs_service_create(mnt, 1);
	if (IS_ERR(gate->service)) {
		error = PTR_ERR(gate->service);
		goto out_port;
	}
	gate->credential_generation = 1;
	gate->role = FS_ROOT;
	error = kobox_linux_fs_service_register(gate->service, 1, 1,
						gate->creds[FS_ROOT],
						KB2_FILESYSTEM_RIGHTS_VALID_MASK);
	/* Exercise retained root/pwd reconciliation through the same complete
	 * namespace/dirfd/umask/credential workload as native Linux. This scope
	 * belongs only to this serial task and is gone before threaded gates.
	 */
	if (!error) {
		gate->scope = copy_fs_struct(current->fs ?: init_task.fs);
		if (!gate->scope)
			error = -ENOMEM;
	}
	if (!error)
		error = kobox_fs_workload(&wire_ops, gate, results);
	if (gate->scope) {
		free_fs_struct(gate->scope);
		gate->scope = NULL;
	}
	if (!error)
		pr_info("FS_SCOPE_REUSE checks=%u namespace_dirfd_umask_creds=PASS\n",
			results->checks);
	if (!error)
		error = gate_close_return(gate);
	if (!error)
		error = gate_authority(gate);
	if (!error)
		error = gate_capability_policy(gate);
	if (!error)
		error = gate_device_route(gate);
	if (!error)
		error = gate_global_sync(gate);
	if (!error)
		error = gate_exec(gate, mnt);
	if (!error)
		error = kobox_linux_fs_workers_verify(mnt, gate->creds[FS_ROOT]);
	kobox_linux_fs_service_destroy(gate->service);
	gate->service = NULL;
	if (!error)
		error = gate_handles(gate, report);
	if (!error)
		error = gate_shared_offset(gate, report);
	if (!error)
		error = gate_close_race(gate, report);
	if (current_cred() != before)
		error = -EINVAL;
out_port:
	kobox_linux_fs_destroy(gate->port);
	gate->port = NULL;
	gate_drain();
	return error;
}

static struct vfsmount *gate_mount_ext4(void)
{
	struct file_system_type *type = get_fs_type("ext4");
	struct fs_context *fc;
	struct vfsmount *mnt;
	int error;

	if (!type)
		return ERR_PTR(-ENODEV);
	fc = fs_context_for_mount(type, SB_KERNMOUNT);
	put_filesystem(type);
	if (IS_ERR(fc))
		return ERR_CAST(fc);
	error = vfs_parse_fs_string(fc, "source", "/fs-port-ram");
	if (!error)
		error = vfs_get_tree(fc);
	if (error) {
		mnt = ERR_PTR(error);
	} else {
		mnt = vfs_create_mount(fc);
		up_write(&fc->root->d_sb->s_umount);
	}
	put_fs_context(fc);
	return mnt;
}

static int gate_image_io(struct file *file, const struct kobox_fs_image *image,
			 bool write)
{
	loff_t position = 0;
	ssize_t count;

	while (position < image->length) {
		count = min_t(size_t, image->length - position, 1UL << 20);
		count = write ? kernel_write(file, image->data + position,
					     count, &position) :
				kernel_read(file, image->data + position,
					    count, &position);
		if (count <= 0)
			return count ?: -EIO;
	}
	return write ? vfs_fsync(file, 0) : 0;
}

static int gate_ram_node(void)
{
	struct path parent;
	struct dentry *entry;
	int error;

	/* init_mknod is __init text, already reclaimed at this test entry. */
	entry = start_creating_path(AT_FDCWD, "/fs-port-ram", &parent, 0);
	if (IS_ERR(entry))
		return PTR_ERR(entry);
	error = vfs_mknod(mnt_idmap(parent.mnt), d_inode(parent.dentry),
			  entry, S_IFBLK | 0600, MKDEV(1, 0));
	end_creating_path(&parent, entry);
	return error;
}

__attribute__((visibility("default")))
int kobox_linux_fs_port_verify(const struct kobox_fs_image *image,
			       struct kobox_fs_port_report *report)
{
	struct fs_gate gate = {};
	struct file_system_type *type;
	struct vfsmount *mnt;
	struct file *disk;
	unsigned int role, old_umask;
	int error;

	if (!image || !image->data || image->length != FS_RAM_BYTES || !report ||
	    system_state != SYSTEM_RUNNING || task_pid_nr(current) != 1 ||
	    current->flags & PF_KTHREAD || num_online_cpus() != 2)
		return -EINVAL;
	/* Test modes must not change kthreadd's shared boot-time umask. */
	error = unshare_fs_struct();
	if (error)
		return error;
	old_umask = current->fs->umask;
	current->fs->umask = 0;
	for (role = 0; role < FS_ROLES; role++) {
		gate.creds[role] = gate_cred(role);
		if (IS_ERR(gate.creds[role])) {
			error = PTR_ERR(gate.creds[role]);
			gate.creds[role] = NULL;
			goto out_creds;
		}
	}
	report->stage = 1;
	type = get_fs_type("tmpfs");
	if (!type) {
		error = -ENODEV;
		goto out_creds;
	}
	mnt = kern_mount(type);
	put_filesystem(type);
	if (IS_ERR(mnt)) {
		error = PTR_ERR(mnt);
		goto out_creds;
	}
	error = gate_filesystem(&gate, mnt, &report->tmpfs, report);
	kern_unmount(mnt);
	gate_drain();
	if (error)
		goto out_creds;
	report->stage = 2;
	error = gate_ram_node();
	if (error)
		goto out_creds;
	disk = bdev_file_open_by_dev(MKDEV(1, 0), BLK_OPEN_READ | BLK_OPEN_WRITE,
				     NULL, NULL);
	if (IS_ERR(disk)) {
		error = PTR_ERR(disk);
		goto out_creds;
	}
	error = gate_image_io(disk, image, true);
	if (error)
		goto out_disk;
	invalidate_bdev(file_bdev(disk));
	report->stage = 3;
	mnt = gate_mount_ext4();
	if (IS_ERR(mnt)) {
		error = PTR_ERR(mnt);
		goto out_disk;
	}
	error = gate_filesystem(&gate, mnt, &report->ext4, report);
	kern_unmount(mnt);
	gate_drain();
	if (error)
		goto out_disk;
	report->stage = 4;
	invalidate_bdev(file_bdev(disk));
	error = gate_image_io(disk, image, false);
out_disk:
	fput(disk);
	gate_drain();
out_creds:
	for (role = 0; role < FS_ROLES; role++)
		if (gate.creds[role])
			put_cred(gate.creds[role]);
	current->fs->umask = old_umask;
	if (!error)
		error = kobox_linux_lifecycle_async_verify();
	report->warnings = kobox_linux_exception_warnings();
	report->result = error;
	if (!error && report->warnings)
		error = -EINVAL;
	return error;
}
