// SPDX-License-Identifier: GPL-2.0-only
#include "fs_worker_gate.h"
#include "fs_service.h"
#include "fs_worker.h"

#include <kobox2/filesystem.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/stringhash.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/task_work.h>

struct fs_worker_gate {
	struct kobox_linux_fs_workers *workers;
	unsigned long seen;
	unsigned int outstanding;
};

static int worker_gate_encode(u8 *input, size_t size, kb2_fs_request_t *request,
	const char *path)
{
	request->generation = 1;
	request->mask = STATX_BASIC_STATS;
	if (path) {
		request->path = (kb2_fs_span_t) {
			KB2_FILESYSTEM_REQUEST_SIZE, strlen(path) + 1,
		};
		memcpy(input + request->path.offset, path, request->path.length);
	}
	return kb2_fs_request_encode(input, size, size, request);
}

static int worker_gate_submit_capacity(struct fs_worker_gate *gate,
	unsigned long cookie, u64 client, u32 opcode, u64 handle, size_t capacity)
{
	u8 input[KB2_FILESYSTEM_REQUEST_SIZE + 2];
	kb2_fs_request_t request = {
		.opcode = opcode, .client = client, .sequence = cookie,
		.handle = handle, .length = 0,
	};
	int error;

	if (opcode == KB2_FILESYSTEM_OP_PREAD) {
		request.length = 2;
		request.offset = 2;
	}
	error = worker_gate_encode(input, sizeof(input), &request,
		opcode == KB2_FILESYSTEM_OP_STATX ? "." : NULL);
	if (!error)
		error = kobox_linux_fs_workers_submit(gate->workers, client, input,
			sizeof(input), capacity,
			(void *)cookie);
	if (!error) {
		gate->outstanding++;
		/* No worker may read this storage after admission. In particular,
		 * changing the opcode must not turn an authorized stat into a close.
		 */
		memset(input, 0xff, sizeof(input));
	}
	return error;
}

static int worker_gate_submit(struct fs_worker_gate *gate, unsigned long cookie,
	u64 client, u32 opcode, u64 handle)
{
	return worker_gate_submit_capacity(gate, cookie, client, opcode, handle,
		KB2_FILESYSTEM_RESPONSE_SIZE + KB2_FILESYSTEM_STATX_SIZE);
}

static int worker_gate_collect(struct fs_worker_gate *gate, unsigned long allowed)
{
	struct kobox_linux_fs_work *work;
	kb2_fs_response_t reply;
	const void *response;
	void *cookie;
	size_t used;
	unsigned long id;
	int error, result;

	result = kobox_linux_fs_workers_collect(gate->workers, &work, &cookie,
					       &response, &used, &error);
	if (result)
		return result;
	id = (unsigned long)cookie;
	if (id >= BITS_PER_LONG || !(allowed & BIT(id)) ||
	    (gate->seen & BIT(id)) || error ||
	    kb2_fs_response_decode(response, used, &reply) ||
	    reply.sequence != id || reply.result != (id == 7 ? -EACCES : 0) ||
	    (id == 17 && (reply.data_length || used != KB2_FILESYSTEM_RESPONSE_SIZE ||
		memchr_inv(response + KB2_FILESYSTEM_RESPONSE_SIZE, 0, 2))))
		result = -EINVAL;
	else
		gate->seen |= BIT(id);
	kobox_linux_fs_workers_release(gate->workers, work);
	gate->outstanding--;
	return result;
}

static int worker_gate_wait(struct fs_worker_gate *gate, unsigned long wanted,
	unsigned long allowed)
{
	unsigned long deadline = jiffies + HZ;
	int error;

	while ((gate->seen & wanted) != wanted) {
		error = worker_gate_collect(gate, allowed);
		if (error && error != -EAGAIN)
			return error;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		if (error)
			schedule_timeout_uninterruptible(1);
	}
	return 0;
}

static unsigned int worker_gate_waiters(struct inode *inode)
{
	struct list_head *waiter;
	unsigned long flags;
	unsigned int count = 0;

	/* Count actual VFS lock waiters under the upstream rwsem's wait lock.
	 * A queued transport request is not evidence of a sleeping Linux task.
	 */
	raw_spin_lock_irqsave(&inode->i_rwsem.wait_lock, flags);
	list_for_each(waiter, &inode->i_rwsem.wait_list)
		count++;
	raw_spin_unlock_irqrestore(&inode->i_rwsem.wait_lock, flags);
	return count;
}

static int worker_gate_large_io(struct fs_worker_gate *gate, u64 handle)
{
	const size_t length = 65536;
	u8 *input = kvmalloc(KB2_FILESYSTEM_REQUEST_SIZE + length, GFP_KERNEL);
	unsigned int phase;
	int result = -ENOMEM;

	if (!input)
		return result;
	/* Equal total capacity, different input/output partition: reuse must not
	 * retain either the old snapshot layout or bytes supplied by the peer.
	 */
	for (phase = 0; phase < 2; phase++) {
		kb2_fs_request_t request = {
			.opcode = phase ? KB2_FILESYSTEM_OP_PREAD : KB2_FILESYSTEM_OP_PWRITE,
			.client = 2, .handle = handle, .length = length, .sequence = 19,
			.data = phase ? (kb2_fs_span_t) {} :
				(kb2_fs_span_t) { KB2_FILESYSTEM_REQUEST_SIZE, length },
		};
		struct kobox_linux_fs_work *work;
		kb2_fs_response_t reply;
		const void *response;
		void *cookie;
		size_t used, size = KB2_FILESYSTEM_REQUEST_SIZE + (phase ? 0 : length);
		unsigned long deadline = jiffies + HZ;
		int execution;

		result = worker_gate_encode(input, size, &request, NULL);
		if (result)
			break;
		if (!phase)
			memset(input + KB2_FILESYSTEM_REQUEST_SIZE, 0x5a, length);
		result = kobox_linux_fs_workers_submit(gate->workers, 2, input, size,
			KB2_FILESYSTEM_RESPONSE_SIZE + (phase ? length : 0), (void *)19);
		if (result)
			break;
		gate->outstanding++;
		memset(input, 0xff, size);
		for (;;) {
			result = kobox_linux_fs_workers_collect(gate->workers, &work,
				&cookie, &response, &used, &execution);
			if (result != -EAGAIN)
				break;
			if (time_after(jiffies, deadline)) {
				result = -ETIMEDOUT;
				break;
			}
			schedule_timeout_uninterruptible(1);
		}
		if (result)
			break;
		result = execution || (unsigned long)cookie != 19 ||
			kb2_fs_response_decode(response, used, &reply) ||
			reply.sequence != 19 || reply.result != length ||
			reply.data_length != (phase ? length : 0) ||
			(phase && memchr_inv(response + KB2_FILESYSTEM_RESPONSE_SIZE,
				0x5a, length)) ? -EINVAL : 0;
		kobox_linux_fs_workers_release(gate->workers, work);
		gate->outstanding--;
		if (result)
			break;
	}
	kvfree(input);
	return result;
}

/* Submit one request to the pool and copy its whole reply. Immediate reports
 * that the first collect already found it, i.e. no worker was involved. */
static int inline_gate_call(struct kobox_linux_fs_workers *workers,
	kb2_fs_request_t *request, const char *path, u8 *reply, size_t capacity,
	size_t *used, bool *immediate)
{
	u8 input[KB2_FILESYSTEM_REQUEST_SIZE + 32];
	struct kobox_linux_fs_work *work;
	unsigned long deadline = jiffies + HZ;
	const void *response;
	void *cookie;
	int error, execution;

	request->generation = 1;
	request->path = (kb2_fs_span_t) {};
	if (path) {
		request->path = (kb2_fs_span_t) {
			KB2_FILESYSTEM_REQUEST_SIZE, strlen(path) + 1,
		};
		memcpy(input + request->path.offset, path, request->path.length);
	}
	error = kb2_fs_request_encode(input, sizeof(input), sizeof(input), request);
	if (!error)
		error = kobox_linux_fs_workers_submit(workers, request->client, input,
			sizeof(input), capacity, (void *)(unsigned long)request->sequence);
	if (error)
		return error;
	*immediate = true;
	while ((error = kobox_linux_fs_workers_collect(workers, &work, &cookie,
			&response, used, &execution)) == -EAGAIN) {
		*immediate = false;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		schedule_timeout_uninterruptible(1);
	}
	if (error)
		return error;
	if ((unsigned long)cookie != request->sequence || *used > capacity)
		error = -EINVAL;
	else
		memcpy(reply, response, *used);
	kobox_linux_fs_workers_release(workers, work);
	return error ?: execution;
}

/* Inline owner execution: a cached stat completes on the owner, an uncached
 * name or page and a busy file table (a possible final fput) are punted to a
 * worker with the same result, and inline and worker replies are identical.
 */
static int worker_gate_inline(struct kobox_linux_fs_service *service,
	struct vfsmount *root, const struct cred *cred)
{
	const struct kobox_linux_fs_worker_config config = {
		.workers = 3, .client_running = 2,
		.client_requests = 5, .total_requests = 10,
		.client_bytes = 131072, .total_bytes = 262144, .control_bytes = 4096,
	};
	struct kobox_linux_fs_workers *workers = NULL;
	struct kobox_linux_fs_request *prepared = NULL;
	struct dentry *entry = NULL;
	struct qstr name = QSTR_INIT("worker-inline", 13);
	struct qstr absent_name = QSTR_INIT("inline-absent", 13);
	struct dentry *absent;
	u8 input[KB2_FILESYSTEM_REQUEST_SIZE + 16];
	u8 first[KB2_FILESYSTEM_RESPONSE_SIZE + KB2_FILESYSTEM_STATX_SIZE];
	u8 second[sizeof(first)];
	u8 response[KB2_FILESYSTEM_RESPONSE_SIZE];
	kb2_fs_request_t request;
	kb2_fs_response_t reply;
	u64 handle = 0, closing = 0, hits, punts, base_hits, base_punts;
	size_t used, used_second;
	bool immediate, cached_read = false, negative_cached = false;
	int error, line = 0;

#define INLINE_EXPECT(call, expected) do { \
	error = (call); \
	if (error != (expected)) { \
		line = __LINE__; \
		error = error ?: -EINVAL; \
		goto out; \
	} \
} while (0)
#define INLINE_COUNTS(hit, punt) do { \
	kobox_linux_fs_workers_gate_inline(workers, true, &hits, &punts); \
	INLINE_EXPECT(hits == base_hits + (hit) && punts == base_punts + (punt), 1); \
} while (0)
	INLINE_EXPECT(kobox_linux_fs_workers_create(service, &config, &workers), 0);
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_OPENAT2, .client = 2, .sequence = 200,
		.flags = O_CREAT | O_RDWR | O_EXCL, .mode = 0600, .generation = 1,
		.path = { KB2_FILESYSTEM_REQUEST_SIZE, name.len + 1 },
	};
	memcpy(input + KB2_FILESYSTEM_REQUEST_SIZE, name.name, name.len + 1);
	INLINE_EXPECT(kb2_fs_request_encode(input, sizeof(input), sizeof(input), &request), 0);
	INLINE_EXPECT(kobox_linux_fs_service_dispatch(service, 2, input, sizeof(input),
		response, sizeof(response), &used), 0);
	INLINE_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	INLINE_EXPECT(reply.result, 0);
	handle = reply.handle;
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_WRITE, .client = 2, .sequence = 201,
		.handle = handle, .length = 6, .generation = 1,
		.data = { KB2_FILESYSTEM_REQUEST_SIZE, 6 },
	};
	memcpy(input + KB2_FILESYSTEM_REQUEST_SIZE, "INLINE", 6);
	INLINE_EXPECT(kb2_fs_request_encode(input, sizeof(input), sizeof(input), &request), 0);
	INLINE_EXPECT(kobox_linux_fs_service_dispatch(service, 2, input, sizeof(input),
		response, sizeof(response), &used), 0);
	INLINE_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	INLINE_EXPECT(reply.result, 6);
	kobox_linux_fs_workers_gate_inline(workers, true, &base_hits, &base_punts);

	/* 1. A cached path completes on the owner: no worker, no wake. */
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_STATX, .client = 2, .sequence = 202,
		.mask = STATX_BASIC_STATS,
	};
	INLINE_EXPECT(inline_gate_call(workers, &request, ".", first, sizeof(first),
		&used, &immediate), 0);
	INLINE_EXPECT(kb2_fs_response_decode(first, used, &reply), 0);
	INLINE_EXPECT(reply.result == 0 && immediate, 1);
	INLINE_COUNTS(1, 0);
	/* 2. A name never looked up needs ->lookup: punted, same -ENOENT. */
	request.sequence = 203;
	INLINE_EXPECT(inline_gate_call(workers, &request, "inline-absent", first,
		sizeof(first), &used, &immediate), 0);
	INLINE_EXPECT(kb2_fs_response_decode(first, used, &reply), 0);
	INLINE_EXPECT(reply.result, -ENOENT);
	INLINE_COUNTS(1, 1);
	/* 3. Where the worker left a negative dentry (ext4; tmpfs deletes them
	 * on dput), the repeated miss is inline too; otherwise it punts again.
	 */
	/* d_lookup matches on the hashed name; QSTR_INIT leaves it unhashed. */
	absent_name.hash = full_name_hash(root->mnt_root, absent_name.name, absent_name.len);
	absent = d_lookup(root->mnt_root, &absent_name);
	negative_cached = absent != NULL;
	dput(absent);
	request.sequence = 204;
	INLINE_EXPECT(inline_gate_call(workers, &request, "inline-absent", first,
		sizeof(first), &used, &immediate), 0);
	INLINE_EXPECT(kb2_fs_response_decode(first, used, &reply), 0);
	INLINE_EXPECT(reply.result, -ENOENT);
	if (negative_cached) {
		INLINE_EXPECT(immediate, 1);
		INLINE_COUNTS(2, 1);
	} else {
		INLINE_COUNTS(1, 2);
	}
	base_hits = hits;
	base_punts = punts;

	/* 4. Dropped page cache: IOCB_NOIO punts the read; data is unchanged.
	 * The repeat is inline only where read_iter honours IOCB_NOWAIT.
	 */
	entry = lookup_one_positive_unlocked(mnt_idmap(root), &name, root->mnt_root);
	if (IS_ERR(entry)) {
		error = PTR_ERR(entry);
		entry = NULL;
		line = __LINE__;
		goto out;
	}
	INLINE_EXPECT(filemap_write_and_wait(d_inode(entry)->i_mapping), 0);
	invalidate_mapping_pages(d_inode(entry)->i_mapping, 0, -1);
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_PREAD, .client = 2, .sequence = 205,
		.handle = handle, .length = 6, .offset = 0,
	};
	INLINE_EXPECT(inline_gate_call(workers, &request, NULL, first,
		KB2_FILESYSTEM_RESPONSE_SIZE + 6, &used, &immediate), 0);
	INLINE_EXPECT(kb2_fs_response_decode(first, used, &reply), 0);
	INLINE_EXPECT(reply.result, 6);
	INLINE_EXPECT(memcmp(first + KB2_FILESYSTEM_RESPONSE_SIZE, "INLINE", 6), 0);
	INLINE_COUNTS(0, 1);
	request.sequence = 206;
	INLINE_EXPECT(inline_gate_call(workers, &request, NULL, first,
		KB2_FILESYSTEM_RESPONSE_SIZE + 6, &used, &immediate), 0);
	INLINE_EXPECT(kb2_fs_response_decode(first, used, &reply), 0);
	INLINE_EXPECT(reply.result, 6);
	INLINE_EXPECT(memcmp(first + KB2_FILESYSTEM_RESPONSE_SIZE, "INLINE", 6), 0);
	kobox_linux_fs_workers_gate_inline(workers, true, &hits, &punts);
	cached_read = hits == base_hits + 1;
	INLINE_EXPECT(cached_read ? punts == base_punts + 1 : punts == base_punts + 2, 1);
	base_hits = hits;
	base_punts = punts;

	/* 5. A concurrent close leaves the request's pin as the last file
	 * reference. The inline result is discarded (its fput would be final on
	 * the owner) and the unexecuted job still runs on the pinned file later.
	 */
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_OPENAT2, .client = 2, .sequence = 207,
		.flags = O_RDONLY, .generation = 1,
		.path = { KB2_FILESYSTEM_REQUEST_SIZE, name.len + 1 },
	};
	memcpy(input + KB2_FILESYSTEM_REQUEST_SIZE, name.name, name.len + 1);
	INLINE_EXPECT(kb2_fs_request_encode(input, sizeof(input), sizeof(input), &request), 0);
	INLINE_EXPECT(kobox_linux_fs_service_dispatch(service, 2, input, sizeof(input),
		response, sizeof(response), &used), 0);
	INLINE_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	INLINE_EXPECT(reply.result, 0);
	closing = reply.handle;
	/* fstat runs inline on every filesystem (no lookup, no read_iter), so
	 * the release check itself is what must refuse the result. */
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_STATX, .client = 2, .sequence = 208,
		.handle = closing, .flags = AT_EMPTY_PATH, .mask = STATX_BASIC_STATS,
		.generation = 1,
	};
	INLINE_EXPECT(kb2_fs_request_encode(input, sizeof(input), sizeof(input), &request), 0);
	INLINE_EXPECT(kobox_linux_fs_request_prepare(service, 2, input, sizeof(input),
		first, sizeof(first), &prepared), 0);
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_CLOSE, .client = 2, .sequence = 209,
		.handle = closing, .generation = 1,
	};
	INLINE_EXPECT(kb2_fs_request_encode(input, sizeof(input), sizeof(input), &request), 0);
	INLINE_EXPECT(kobox_linux_fs_service_dispatch(service, 2, input, sizeof(input),
		response, sizeof(response), &used), 0);
	INLINE_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	INLINE_EXPECT(reply.result, 0);
	INLINE_EXPECT(kobox_linux_fs_request_try_nowait(prepared, NULL, &used), -EAGAIN);
	INLINE_EXPECT(kobox_linux_fs_request_executed(prepared), 0);
	INLINE_EXPECT(kobox_linux_fs_request_execute(prepared, &used), 0);
	INLINE_EXPECT(kb2_fs_response_decode(first, used, &reply), 0);
	INLINE_EXPECT(reply.result == 0 && reply.data_length == KB2_FILESYSTEM_STATX_SIZE, 1);
	kobox_linux_fs_request_destroy(prepared);
	prepared = NULL;

	/* 6. Inline and worker execution give byte-identical replies. */
	for (unsigned int pass = 0; pass < 2; pass++) {
		request = pass ? (kb2_fs_request_t) {
			.opcode = KB2_FILESYSTEM_OP_PREAD, .client = 2, .sequence = 210,
			.handle = handle, .length = 6,
		} : (kb2_fs_request_t) {
			.opcode = KB2_FILESYSTEM_OP_STATX, .client = 2, .sequence = 210,
			.mask = STATX_BASIC_STATS,
		};
		INLINE_EXPECT(inline_gate_call(workers, &request, pass ? NULL : ".", first,
			sizeof(first), &used, &immediate), 0);
		kobox_linux_fs_workers_gate_inline(workers, false, &hits, &punts);
		INLINE_EXPECT(inline_gate_call(workers, &request, pass ? NULL : ".", second,
			sizeof(second), &used_second, &immediate), 0);
		kobox_linux_fs_workers_gate_inline(workers, true, &hits, &punts);
		INLINE_EXPECT(used == used_second && !memcmp(first, second, used), 1);
	}
	pr_info("FS_WORKER_INLINE status=0 cached_stat=1 uncached_punt=1 negative_cached=%d page_punt=1 cached_read=%d closed_release_punt=1 identical=2\n",
		negative_cached, cached_read);
	error = 0;	/* the boolean checks above leave their value in error */

out:
	kobox_linux_fs_request_destroy(prepared);
	if (workers) {
		kobox_linux_fs_workers_close(workers);
		kobox_linux_fs_workers_destroy(workers);
	}
	if (handle) {
		request = (kb2_fs_request_t) {
			.opcode = KB2_FILESYSTEM_OP_CLOSE, .client = 2, .sequence = 211,
			.handle = handle, .generation = 1,
		};
		if (!kb2_fs_request_encode(input, sizeof(input), sizeof(input), &request))
			kobox_linux_fs_service_dispatch(service, 2, input, sizeof(input),
				response, sizeof(response), &used);
	}
	if (!entry && handle) {
		entry = lookup_one_positive_unlocked(mnt_idmap(root), &name, root->mnt_root);
		if (IS_ERR(entry))
			entry = NULL;
	}
	if (entry) {
		int result;

		inode_lock(d_inode(root->mnt_root));
		result = vfs_unlink(mnt_idmap(root), d_inode(root->mnt_root), entry, NULL);
		inode_unlock(d_inode(root->mnt_root));
		if (!error)
			error = result;
		dput(entry);
	}
	task_work_run();
	if (error)
		pr_info("FS_WORKER_INLINE status=%d line=%d\n", error, line);
	return error;
#undef INLINE_COUNTS
#undef INLINE_EXPECT
}

int kobox_linux_fs_workers_verify(struct vfsmount *root, const struct cred *cred)
{
	const struct kobox_linux_fs_worker_config config = {
		.workers = 3, .client_running = 2,
		.client_requests = 5, .total_requests = 10,
		.client_bytes = 131072, .total_bytes = 262144, .control_bytes = 4096,
	};
	struct kobox_linux_fs_service *service;
	struct kobox_linux_fs_request *prepared = NULL;
	struct kobox_linux_fs_request *transfer_prepared = NULL;
	struct fs_worker_gate gate = {};
	struct kobox_linux_fs_work *held = NULL;
	const void *private_response;
	void *cookie;
	struct dentry *entry = NULL;
	struct qstr name = QSTR_INIT("worker-blocked", 14);
	u8 input[KB2_FILESYSTEM_REQUEST_SIZE + 15];
	u8 response[KB2_FILESYSTEM_RESPONSE_SIZE];
	u8 control_response[KB2_FILESYSTEM_RESPONSE_SIZE + KB2_FILESYSTEM_STATX_SIZE];
	u8 held_read[KB2_FILESYSTEM_RESPONSE_SIZE + 2];
	kb2_fs_request_t request = {
		.opcode = KB2_FILESYSTEM_OP_OPENAT2, .client = 1, .sequence = 100,
		.flags = O_CREAT | O_RDWR | O_EXCL, .mode = 0600,
	};
	kb2_fs_response_t reply;
	unsigned long deadline;
	unsigned int cached_works, cached_clients;
	size_t used;
	u64 handle = 0, other_handle = 0, shared_handle = 0;
	u64 cached_bytes, reused_buffers, evicted_buffers, inline_hits, inline_punts;
	bool locked = false;
	int error, result, execution, line = 0;

#define WORKER_EXPECT(call, expected) do { \
	error = (call); \
	if (error != (expected)) { \
		line = __LINE__; \
		error = error ?: -EINVAL; \
		goto out; \
	} \
} while (0)
	service = kobox_linux_fs_service_create(root, 1);
	if (IS_ERR(service))
		return PTR_ERR(service);
	WORKER_EXPECT(kobox_linux_fs_service_register(service, 1, 1, cred,
		KB2_FILESYSTEM_RIGHTS_VALID_MASK), 0);
	WORKER_EXPECT(kobox_linux_fs_service_register(service, 2, 1, cred,
		KB2_FILESYSTEM_RIGHTS_VALID_MASK), 0);
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request,
		"worker-blocked"), 0);
	WORKER_EXPECT(kobox_linux_fs_service_dispatch(service, 1, input,
		sizeof(input), response, sizeof(response), &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	WORKER_EXPECT(reply.result, 0);
	handle = reply.handle;
	request.client = 2;
	request.flags = O_RDWR;
	request.mode = 0;
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request,
		"worker-blocked"), 0);
	WORKER_EXPECT(kobox_linux_fs_service_dispatch(service, 2, input,
		sizeof(input), response, sizeof(response), &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	WORKER_EXPECT(reply.result, 0);
	other_handle = reply.handle;
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_WRITE, .client = 1, .sequence = 110,
		.handle = handle, .length = 2,
		.data = { KB2_FILESYSTEM_REQUEST_SIZE, 2 },
	};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, NULL), 0);
	memcpy(input + KB2_FILESYSTEM_REQUEST_SIZE, "AB", 2);
	WORKER_EXPECT(kobox_linux_fs_service_dispatch(service, 1, input,
		sizeof(input), response, sizeof(response), &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	WORKER_EXPECT(reply.result, 2);
	request.opcode = KB2_FILESYSTEM_OP_LSEEK;
	request.length = 0;
	request.data = (kb2_fs_span_t) {};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, NULL), 0);
	WORKER_EXPECT(kobox_linux_fs_service_dispatch(service, 1, input,
		sizeof(input), response, sizeof(response), &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	WORKER_EXPECT(reply.result, 0);
	WORKER_EXPECT(kobox_linux_fs_service_transfer(service, 1, 2, handle,
		&shared_handle), 0);
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_READ, .client = 1, .sequence = 111,
		.handle = handle, .length = 1,
	};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, NULL), 0);
	WORKER_EXPECT(kobox_linux_fs_request_prepare(service, 1, input,
		sizeof(input), held_read, sizeof(held_read), &prepared), 0);
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_TRANSFER_DUP, .client = 2,
		.credential_generation = 1, .sequence = 112,
		.handle = shared_handle, .second_handle = 1,
	};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, NULL), 0);
	WORKER_EXPECT(kobox_linux_fs_request_prepare_control(service, input,
		sizeof(input), response, sizeof(response), &transfer_prepared), 0);
	/* Both source handles disappear before either prepared request executes.
	 * The read and owner transfer must retain the same actual OFD, not reopen
	 * the pathname or look up a closed table entry when a worker starts.
	 */
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_CLOSE, .client = 1, .sequence = 113,
		.handle = handle,
	};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, NULL), 0);
	WORKER_EXPECT(kobox_linux_fs_service_dispatch(service, 1, input,
		sizeof(input), control_response, sizeof(control_response), &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(control_response, used, &reply), 0);
	WORKER_EXPECT(reply.result, 0);
	request.client = 2;
	request.handle = shared_handle;
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, NULL), 0);
	WORKER_EXPECT(kobox_linux_fs_service_dispatch(service, 2, input,
		sizeof(input), control_response, sizeof(control_response), &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(control_response, used, &reply), 0);
	WORKER_EXPECT(reply.result, 0);
	WORKER_EXPECT(kobox_linux_fs_request_execute(prepared, &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(held_read, used, &reply), 0);
	WORKER_EXPECT(reply.result, 1);
	WORKER_EXPECT(held_read[KB2_FILESYSTEM_RESPONSE_SIZE], 'A');
	kobox_linux_fs_request_destroy(prepared);
	prepared = NULL;
	WORKER_EXPECT(kobox_linux_fs_request_execute(transfer_prepared, &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	WORKER_EXPECT(reply.result, 0);
	handle = reply.handle;
	kobox_linux_fs_request_destroy(transfer_prepared);
	transfer_prepared = NULL;
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_READ, .client = 1, .sequence = 114,
		.handle = handle, .length = 1,
	};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, NULL), 0);
	WORKER_EXPECT(kobox_linux_fs_service_dispatch(service, 1, input,
		sizeof(input), held_read, sizeof(held_read), &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(held_read, used, &reply), 0);
	WORKER_EXPECT(reply.result, 1);
	WORKER_EXPECT(held_read[KB2_FILESYSTEM_RESPONSE_SIZE], 'B');
	pr_info("FS_REQUEST_LIFETIME pinned_read=1 pinned_transfer=1 shared_offset=1\n");
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_CLOSE, .client = 1,
		.sequence = 101, .handle = handle,
	};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, NULL), 0);
	WORKER_EXPECT(kobox_linux_fs_request_prepare(service, 1, input,
		sizeof(input), response, sizeof(response), &prepared), 0);
	/* Cancelling an admitted but unexecuted CLOSE must leave the handle open. */
	kobox_linux_fs_request_destroy(prepared);
	prepared = NULL;
	/* Owner VFS admission has the same immutable authority boundary as data.
	 * Updating rights and overwriting shared bytes must not change a prepared
	 * device probe into either a newly denied operation or an unregister.
	 */
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_DEVICE_ROUTE, .client = 1,
		.credential_generation = 1, .sequence = 103, .flags = O_RDONLY,
	};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, "."), 0);
	WORKER_EXPECT(kobox_linux_fs_request_prepare_control(service, input,
		sizeof(input), control_response, sizeof(control_response), &prepared), 0);
	WORKER_EXPECT(kobox_linux_fs_service_credentials(service, 1, 2, cred, 0), 0);
	memset(input, 0xff, sizeof(input));
	WORKER_EXPECT(kobox_linux_fs_request_execute(prepared, &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(control_response, used, &reply), 0);
	WORKER_EXPECT(reply.result, -ENODEV);
	WORKER_EXPECT(reply.sequence, 103);
	kobox_linux_fs_request_destroy(prepared);
	prepared = NULL;
	request.credential_generation = 2;
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, "."), 0);
	WORKER_EXPECT(kobox_linux_fs_request_prepare_control(service, input,
		sizeof(input), control_response, sizeof(control_response), &prepared), 0);
	WORKER_EXPECT(kobox_linux_fs_service_credentials(service, 1, 3, cred,
		KB2_FILESYSTEM_RIGHTS_VALID_MASK), 0);
	WORKER_EXPECT(kobox_linux_fs_request_execute(prepared, &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(control_response, used, &reply), 0);
	WORKER_EXPECT(reply.result, -EACCES);
	kobox_linux_fs_request_destroy(prepared);
	prepared = NULL;
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_STATX, .client = 1, .sequence = 102,
	};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, "."), 0);
	WORKER_EXPECT(kobox_linux_fs_request_prepare(service, 1, input,
		sizeof(input), response, sizeof(response), &prepared), 0);
	WORKER_EXPECT(kobox_linux_fs_request_execute(prepared, &used), 0);
	WORKER_EXPECT(kb2_fs_response_decode(response, used, &reply), 0);
	WORKER_EXPECT(reply.result, -EMSGSIZE);
	WORKER_EXPECT(kobox_linux_fs_request_execute(prepared, &used), -EALREADY);
	WORKER_EXPECT(used, 0);
	kobox_linux_fs_request_destroy(prepared);
	prepared = NULL;
	entry = lookup_one_positive_unlocked(mnt_idmap(root), &name, root->mnt_root);
	if (IS_ERR(entry)) {
		error = PTR_ERR(entry);
		entry = NULL;
		goto out;
	}
	kobox_linux_fs_workers_gate_fail_scope();
	WORKER_EXPECT(kobox_linux_fs_workers_create(service, &config, &gate.workers),
		-ENOMEM);
	WORKER_EXPECT(gate.workers != NULL, 0);
	WORKER_EXPECT(kobox_linux_fs_workers_create(service, &config, &gate.workers), 0);
	/* These scenarios test worker admission, fairness and blocking, so keep
	 * every request on a worker; worker_gate_inline covers the owner path. */
	kobox_linux_fs_workers_gate_inline(gate.workers, false, &inline_hits, &inline_punts);
	kobox_linux_fs_request_gate_fail_allocation();
	WORKER_EXPECT(kobox_linux_fs_workers_submit(gate.workers, 1, input,
		sizeof(input), sizeof(control_response), NULL), -ENOMEM);
	WORKER_EXPECT(kobox_linux_fs_workers_client_busy(gate.workers, 1), 0);
	WORKER_EXPECT(kobox_linux_fs_workers_gate_cached(gate.workers,
		&cached_works, &cached_clients), 0);
	WORKER_EXPECT(cached_works, 1);
	WORKER_EXPECT(cached_clients, 1);
	WORKER_EXPECT(kobox_linux_fs_workers_submit(gate.workers, 1, input,
		SIZE_MAX, sizeof(response), NULL), -EOVERFLOW);
	WORKER_EXPECT(kobox_linux_fs_workers_submit(gate.workers, 1, input,
		sizeof(input), config.client_bytes + 1, NULL), -ENOMEM);
	/* Serial jobs must reuse the same cleared storage across client IDs,
	 * not keep a stale identity/revoked bit or a previous response lease.
	 */
	for (unsigned int client = 1; client <= 2; client++) {
		unsigned long cookie = 14 + client;

		WORKER_EXPECT(worker_gate_submit(&gate, cookie, client,
			KB2_FILESYSTEM_OP_STATX, 0), 0);
		WORKER_EXPECT(worker_gate_wait(&gate, BIT(cookie), BIT(cookie)), 0);
		WORKER_EXPECT(kobox_linux_fs_workers_gate_cached(gate.workers,
			&cached_works, &cached_clients), 0);
		WORKER_EXPECT(cached_works, 1);
		WORKER_EXPECT(cached_clients, 1);
	}
	WORKER_EXPECT(kobox_linux_fs_workers_gate_storage(gate.workers,
		&cached_bytes, &reused_buffers, &evicted_buffers), 0);
	WORKER_EXPECT(reused_buffers > 0, 1);
	/* An armed allocation fault must not affect recycled storage; the next
	 * different-size request must consume it, with no admission leaked.
	 */
	kobox_linux_fs_request_gate_fail_allocation();
	WORKER_EXPECT(worker_gate_submit(&gate, 17, 2,
		KB2_FILESYSTEM_OP_PREAD, other_handle), 0);
	WORKER_EXPECT(worker_gate_wait(&gate, BIT(17), BIT(17)), 0);
	WORKER_EXPECT(worker_gate_submit_capacity(&gate, 18, 2,
		KB2_FILESYSTEM_OP_STATX, 0,
		KB2_FILESYSTEM_RESPONSE_SIZE + KB2_FILESYSTEM_STATX_SIZE + 1), -ENOMEM);
	WORKER_EXPECT(kobox_linux_fs_workers_client_busy(gate.workers, 2), 0);
	WORKER_EXPECT(kobox_linux_fs_workers_gate_cached(gate.workers,
		&cached_works, &cached_clients), 0);
	WORKER_EXPECT(worker_gate_large_io(&gate, other_handle), 0);
	/* Three released buffers leave a cache larger than the space available
	 * to two larger simultaneous leases. Admission must evict the third
	 * idle buffer rather than reject a live request or exceed the budget.
	 */
	for (unsigned int pressure = 0; pressure < 2; pressure++) {
		gate.seen = 0;
		for (unsigned int slot = 0; slot < 3; slot++)
			WORKER_EXPECT(worker_gate_submit_capacity(&gate, 20 + slot,
				slot == 2 ? 2 : 1, KB2_FILESYSTEM_OP_STATX, 0,
				64000 - KB2_FILESYSTEM_REQUEST_SIZE - 2), 0);
		WORKER_EXPECT(worker_gate_wait(&gate, BIT(20) | BIT(21) | BIT(22),
			BIT(20) | BIT(21) | BIT(22)), 0);
		WORKER_EXPECT(kobox_linux_fs_workers_gate_cached(gate.workers,
			&cached_works, &cached_clients), 0);
		WORKER_EXPECT(kobox_linux_fs_workers_gate_storage(gate.workers,
			&cached_bytes, &reused_buffers, &evicted_buffers), 0);
		WORKER_EXPECT(cached_bytes, 192000);
		if (pressure)
			kobox_linux_fs_request_gate_fail_allocation();
		for (unsigned int slot = 0; slot < 2; slot++)
			WORKER_EXPECT(worker_gate_submit_capacity(&gate, 23 + slot,
				slot + 1, KB2_FILESYSTEM_OP_STATX, 0,
				128000 - KB2_FILESYSTEM_REQUEST_SIZE - 2), 0);
		WORKER_EXPECT(kobox_linux_fs_workers_gate_cached(gate.workers,
			&cached_works, &cached_clients), 0);
		WORKER_EXPECT(kobox_linux_fs_workers_gate_storage(gate.workers,
			&cached_bytes, &reused_buffers, &evicted_buffers), 0);
		WORKER_EXPECT(cached_bytes, 0);
		WORKER_EXPECT(evicted_buffers > 0, 1);
		WORKER_EXPECT(worker_gate_wait(&gate, BIT(23) | BIT(24),
			BIT(23) | BIT(24)), 0);
		WORKER_EXPECT(kobox_linux_fs_workers_gate_cached(gate.workers,
			&cached_works, &cached_clients), 0);
	}
	pr_info("FS_WORKER_STORAGE scrub=1 cross_client=1 reused=%llu eviction=1 allocation_failure=1 pressure_reclaim=1 large_snapshot=65536 eof_zero=1\n",
		reused_buffers);
	gate.seen = 0;
	/* Block actual do_ftruncate's inode exclusion, not a fabricated sleep in
	 * a transport mock. The owner continues admitting and collecting jobs.
	 */
	inode_lock(d_inode(entry));
	locked = true;
	WORKER_EXPECT(worker_gate_submit(&gate, 1, 1,
		KB2_FILESYSTEM_OP_FTRUNCATE, handle), 0);
	deadline = jiffies + HZ;
	while (!rwsem_is_contended(&d_inode(entry)->i_rwsem)) {
		if (time_after(jiffies, deadline)) {
			error = -ETIMEDOUT;
			line = __LINE__;
			goto out;
		}
		schedule_timeout_uninterruptible(1);
	}
	WORKER_EXPECT(worker_gate_submit(&gate, 2, 1, KB2_FILESYSTEM_OP_STATX, 0), 0);
	WORKER_EXPECT(worker_gate_submit(&gate, 3, 2, KB2_FILESYSTEM_OP_STATX, 0), 0);
	WORKER_EXPECT(worker_gate_wait(&gate, BIT(2) | BIT(3), BIT(2) | BIT(3)), 0);
	/* Two blockers consume this client's execution allowance. A third one
	 * remains queued, leaving a real worker available to another client.
	 */
	WORKER_EXPECT(worker_gate_submit(&gate, 4, 1,
		KB2_FILESYSTEM_OP_FTRUNCATE, handle), 0);
	WORKER_EXPECT(worker_gate_submit(&gate, 5, 1,
		KB2_FILESYSTEM_OP_FTRUNCATE, handle), 0);
	WORKER_EXPECT(worker_gate_submit(&gate, 6, 1, KB2_FILESYSTEM_OP_STATX, 0), 0);
	WORKER_EXPECT(kobox_linux_fs_service_credentials(service, 1, 4, cred, 0), 0);
	WORKER_EXPECT(worker_gate_submit(&gate, 7, 1, KB2_FILESYSTEM_OP_STATX, 0), 0);
	WORKER_EXPECT(worker_gate_submit(&gate, 9, 1, KB2_FILESYSTEM_OP_STATX, 0), -EAGAIN);
	WORKER_EXPECT(worker_gate_submit(&gate, 8, 2, KB2_FILESYSTEM_OP_STATX, 0), 0);
	WORKER_EXPECT(worker_gate_wait(&gate, BIT(8), BIT(8)), 0);
	/* Saturate every data task deliberately. Reception and the reserved
	 * management task must still progress; queued data resumes after unlock.
	 * This tests the finite-pool boundary rather than hiding it with threads.
	 */
	WORKER_EXPECT(worker_gate_submit(&gate, 12, 2,
		KB2_FILESYSTEM_OP_FTRUNCATE, other_handle), 0);
	deadline = jiffies + HZ;
	while (worker_gate_waiters(d_inode(entry)) != config.workers) {
		if (time_after(jiffies, deadline)) {
			error = -ETIMEDOUT;
			line = __LINE__;
			goto out;
		}
		schedule_timeout_uninterruptible(1);
	}
	WORKER_EXPECT(worker_gate_submit(&gate, 13, 2,
		KB2_FILESYSTEM_OP_STATX, 0), 0);
	/* Management has its own actual Linux task and reserved lease, even
	 * when data admission for the blocked client has exhausted its budget.
	 */
	request = (kb2_fs_request_t) {
		.opcode = KB2_FILESYSTEM_OP_HELLO, .sequence = 11,
	};
	WORKER_EXPECT(worker_gate_encode(input, sizeof(input), &request, NULL), 0);
	WORKER_EXPECT(kobox_linux_fs_workers_submit_control(gate.workers, input,
		SIZE_MAX, sizeof(response), NULL), -EOVERFLOW);
	WORKER_EXPECT(kobox_linux_fs_workers_submit_control(gate.workers, input,
		sizeof(input), config.control_bytes + 1, NULL), -ENOMEM);
	WORKER_EXPECT(kobox_linux_fs_workers_submit_control(gate.workers, input,
		sizeof(input), sizeof(response), (void *)11), 0);
	gate.outstanding++;
	memset(input, 0xff, sizeof(input));
	WORKER_EXPECT(kobox_linux_fs_workers_submit_control(gate.workers, input,
		sizeof(input), sizeof(response), NULL), -EAGAIN);
	WORKER_EXPECT(worker_gate_wait(&gate, BIT(11), BIT(11)), 0);
	WORKER_EXPECT(kobox_linux_fs_workers_client_busy(gate.workers, 1), 1);
	WORKER_EXPECT(kobox_linux_fs_workers_revoke(gate.workers, 1), 0);
	WORKER_EXPECT(worker_gate_submit(&gate, 9, 1, KB2_FILESYSTEM_OP_STATX, 0), -EACCES);
	WORKER_EXPECT(kobox_linux_fs_workers_client_busy(gate.workers, 1), 1);
	inode_unlock(d_inode(entry));
	locked = false;
	WORKER_EXPECT(worker_gate_wait(&gate,
		BIT(1) | BIT(4) | BIT(5) | BIT(6) | BIT(7) | BIT(12) | BIT(13),
		BIT(1) | BIT(4) | BIT(5) | BIT(6) | BIT(7) | BIT(12) | BIT(13)), 0);
	WORKER_EXPECT(kobox_linux_fs_workers_client_busy(gate.workers, 1), 0);
	/* Empty-group retirement must not resurrect admission for that identity. */
	WORKER_EXPECT(worker_gate_submit(&gate, 9, 1, KB2_FILESYSTEM_OP_STATX, 0), -EACCES);
	WORKER_EXPECT(worker_gate_submit(&gate, 10, 2, KB2_FILESYSTEM_OP_STATX, 0), 0);
	deadline = jiffies + HZ;
	do {
		result = kobox_linux_fs_workers_collect(gate.workers, &held, &cookie,
			&private_response, &used, &execution);
		if (result != -EAGAIN)
			break;
		if (time_after(jiffies, deadline)) {
			error = -ETIMEDOUT;
			line = __LINE__;
			goto out;
		}
		schedule_timeout_uninterruptible(1);
	} while (1);
	WORKER_EXPECT(result, 0);
	WORKER_EXPECT(execution, 0);
	WORKER_EXPECT((unsigned long)cookie, 10);
	WORKER_EXPECT(kb2_fs_response_decode(private_response, used, &reply), 0);
	WORKER_EXPECT(reply.result, 0);
	WORKER_EXPECT(reply.sequence, 10);
	WORKER_EXPECT(kobox_linux_fs_workers_client_busy(gate.workers, 2), 1);
	WORKER_EXPECT(kobox_linux_fs_workers_close(gate.workers), 0);
	WORKER_EXPECT(kobox_linux_fs_workers_gate_cached(gate.workers,
		&cached_works, &cached_clients), 0);
	WORKER_EXPECT(cached_works, 0);
	WORKER_EXPECT(cached_clients, 0);
	WORKER_EXPECT(kobox_linux_fs_workers_submit_control(gate.workers, input,
		sizeof(input), sizeof(response), NULL), -ESHUTDOWN);
	WORKER_EXPECT(worker_gate_submit(&gate, 11, 2, KB2_FILESYSTEM_OP_STATX, 0), -ESHUTDOWN);
	WORKER_EXPECT(kobox_linux_fs_workers_destroy(gate.workers), -EBUSY);
	WORKER_EXPECT(kobox_linux_fs_workers_release(gate.workers, held), 0);
	held = NULL;
	gate.outstanding--;
	gate.seen |= BIT(10);
	WORKER_EXPECT(kobox_linux_fs_workers_client_busy(gate.workers, 2), 0);
	WORKER_EXPECT(kobox_linux_fs_workers_destroy(gate.workers), 0);
	gate.workers = NULL;
	WORKER_EXPECT(worker_gate_inline(service, root, cred), 0);

out:
	kobox_linux_fs_request_destroy(prepared);
	kobox_linux_fs_request_destroy(transfer_prepared);
	if (locked)
		inode_unlock(d_inode(entry));
	if (gate.workers) {
		kobox_linux_fs_workers_close(gate.workers);
		if (held) {
			kobox_linux_fs_workers_release(gate.workers, held);
			gate.outstanding--;
		}
		deadline = jiffies + 5 * HZ;
		while (gate.outstanding && time_before(jiffies, deadline)) {
			result = worker_gate_collect(&gate, ~0UL);
			if (result == -EAGAIN)
				schedule_timeout_uninterruptible(1);
		}
		/* Never free a service under an unfinished Linux VFS operation. */
		if (gate.outstanding)
			return -ETIMEDOUT;
		kobox_linux_fs_workers_destroy(gate.workers);
	}
	if (entry) {
		inode_lock(d_inode(root->mnt_root));
		result = vfs_unlink(mnt_idmap(root), d_inode(root->mnt_root), entry, NULL);
		inode_unlock(d_inode(root->mnt_root));
		if (!error)
			error = result;
		dput(entry);
	}
	kobox_linux_fs_service_destroy(service);
	task_work_run();
	pr_info("FS_WORKERS status=%d line=%d completed=%lx same_client=2 other_client=3 fair_client=8 control=11 saturated=3 resumed=13\n",
		error, line, gate.seen);
	return error;
#undef WORKER_EXPECT
}
