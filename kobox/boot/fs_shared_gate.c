// SPDX-License-Identifier: GPL-2.0-only
#include "fs_shared.h"
#include "fs_shared_gate.h"
#include <linux/file.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/shmem_fs.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/sched.h>

/* This gate tests Linux pin accounting, not the native PTE/TLB callbacks;
 * those are exercised by the guest's shared-memory workload. */
static int gate_create(void *context, u64 client, u64 sequence,
	const u64 *pages, size_t count, u64 flags, u64 *id)
{
	*id = ++*(u64 *)context;
	return 0;
}

static int gate_refresh(void *context, u64 id, const u64 *pages, size_t count)
{
	return 0;
}

static int gate_retire(void *context, u64 id, int force)
{
	return force ? 1 : 0;
}

struct publication_gate {
	struct kobox_linux_fs_shared *shared;
	struct completion started, done;
	int error;
};

static int gate_publish(void *argument)
{
	struct publication_gate *gate = argument;
	struct kobox_linux_fs_shared_guard guard;
	complete(&gate->started);
	gate->error = kobox_linux_fs_shared_enter(gate->shared, false, true, &guard);
	if (!gate->error) {
		if (guard.mode != KOBOX_FS_SHARED_WRITE)
			gate->error = -EINVAL;
		kobox_linux_fs_shared_leave(gate->shared, false, &guard);
	}
	complete(&gate->done);
	return 0;
}

static int gate_empty_admission(struct kobox_linux_fs_shared *shared)
{
	struct publication_gate gate = {.shared = shared};
	struct kobox_linux_fs_shared_guard first, second, nowait;
	struct task_struct *task;
	int error = kobox_linux_fs_shared_enter(shared, false, false, &first);
	if (error)
		return error;
	/* Two ordinary mutations may both reach VFS while the list stays empty. */
	/* Cross the idle maintenance period: it must not queue an exclusive
	 * waiter and indirectly block readers through rwsem writer preference. */
	schedule_timeout_uninterruptible(msecs_to_jiffies(120));
	error = kobox_linux_fs_shared_enter(shared, true, false, &second);
	if (error)
		goto out_first;
	if (first.mode != KOBOX_FS_SHARED_READ || second.mode != KOBOX_FS_SHARED_READ)
		error = -EINVAL;
	kobox_linux_fs_shared_leave(shared, false, &second);
	if (error)
		goto out_first;
	if (kobox_linux_fs_shared_enter(shared, true, true, &nowait) != -EAGAIN ||
	    nowait.mode != KOBOX_FS_SHARED_NONE) {
		error = -EINVAL;
		goto out_first;
	}
	init_completion(&gate.started);
	init_completion(&gate.done);
	task = kthread_run(gate_publish, &gate, "fs-publish-gate");
	if (IS_ERR(task)) {
		error = PTR_ERR(task);
		goto out_first;
	}
	wait_for_completion(&gate.started);
	schedule_timeout_uninterruptible(2);
	if (completion_done(&gate.done))
		error = -EINVAL;
	kobox_linux_fs_shared_leave(shared, false, &first);
	/* The stack-owned gate must outlive the publisher on every result. */
	wait_for_completion(&gate.done);
	return error ?: gate.error;
out_first:
	kobox_linux_fs_shared_leave(shared, false, &first);
	return error;
}

static int gate_exported_read(struct kobox_linux_fs_shared *shared,
	struct file *file)
{
	struct kobox_linux_fs_shared_guard reader, direct;
	unsigned int flags = READ_ONCE(file->f_flags);
	int error;

	/* The exports above are still live. A buffered read must coexist with
	 * them, while a direct read must punt before touching their folios. */
	error = kobox_linux_fs_shared_enter_read(shared, true, file, &reader);
	if (error)
		return error;
	if (reader.mode != KOBOX_FS_SHARED_READ)
		error = -EINVAL;
	WRITE_ONCE(file->f_flags, flags | O_DIRECT);
	if (kobox_linux_fs_shared_enter_read(shared, true, file, &direct) != -EAGAIN ||
	    direct.mode != KOBOX_FS_SHARED_NONE)
		error = -EINVAL;
	WRITE_ONCE(file->f_flags, flags);
	kobox_linux_fs_shared_leave(shared, false, &reader);
	return error;
}

static int gate_scoped_mutation(struct kobox_linux_fs_shared *shared,
	struct file *exported)
{
	struct file *other;
	struct publication_gate gate = {.shared = shared};
	struct kobox_linux_fs_shared_guard first, second, target;
	struct task_struct *task;
	long result;
	int error;

	other = shmem_file_setup("unrelated-native-export-gate", PAGE_SIZE, VM_NORESERVE);
	if (IS_ERR(other))
		return PTR_ERR(other);
	error = kobox_linux_fs_shared_enter_file(shared, false, false, other, &first);
	if (error)
		goto out_file;
	/* Exports of another inode must not force this mutation onto a worker
	 * or traverse their folios. Read admission still excludes publication. */
	if (first.mode != KOBOX_FS_SHARED_READ || first.mapping != other->f_mapping) {
		error = -EINVAL;
		goto out_first;
	}
	error = kobox_linux_fs_shared_enter_file(shared, true, false, other, &second);
	if (error)
		goto out_first;
	if (second.mode != KOBOX_FS_SHARED_READ)
		error = -EINVAL;
	kobox_linux_fs_shared_leave(shared, true, &second);
	if (error)
		goto out_first;
	if (kobox_linux_fs_shared_enter_file(shared, true, false, exported, &target) != -EAGAIN ||
	    target.mode != KOBOX_FS_SHARED_NONE ||
	    kobox_linux_fs_shared_add_seals(shared, other, F_SEAL_WRITE, &result)) {
		error = -EINVAL;
		goto out_first;
	}
	init_completion(&gate.started);
	init_completion(&gate.done);
	task = kthread_run(gate_publish, &gate, "fs-scoped-publish-gate");
	if (IS_ERR(task)) {
		error = PTR_ERR(task);
		goto out_first;
	}
	wait_for_completion(&gate.started);
	schedule_timeout_uninterruptible(2);
	if (completion_done(&gate.done))
		error = -EINVAL;
	kobox_linux_fs_shared_leave(shared, true, &first);
	wait_for_completion(&gate.done);
	error = error ?: gate.error;
	goto out_file;
out_first:
	kobox_linux_fs_shared_leave(shared, false, &first);
out_file:
	fput(other);
	return error;
}

static int gate_collect_dirty(void *context, u64 id, u64 *bitmap, size_t words)
{
	(void)context;
	(void)id;
	memset(bitmap, 0, words * sizeof(*bitmap));
	return 0;
}

int kobox_linux_fs_shared_verify(void)
{
	struct kobox_linux_fs_shared *shared;
	struct kobox_linux_fs_shared_guard guard;
	struct file *file;
	struct folio *extra = NULL;
	u64 next_id = 0, id, size;
	struct kobox_linux_fs_shared_ops ops = {
		.context = &next_id, .budget_bytes = 1 << 20,
		.create = gate_create, .refresh = gate_refresh,
		.collect_dirty = gate_collect_dirty, .retire = gate_retire,
	};
	long result;
	int error;

	file = shmem_file_setup("native-ro-pin-gate", PAGE_SIZE, VM_NORESERVE);
	if (IS_ERR(file))
		return PTR_ERR(file);
	/* Test-created shmem_file_setup starts sealed, unlike MFD_ALLOW_SEALING. */
	inode_lock(file_inode(file));
	SHMEM_I(file_inode(file))->seals = 0;
	inode_unlock(file_inode(file));
	shared = kobox_linux_fs_shared_create();
	if (IS_ERR(shared)) {
		error = PTR_ERR(shared);
		goto out_file;
	}
	error = kobox_linux_fs_shared_bind(shared, &ops);
	if (error)
		goto out_shared;
	error = gate_empty_admission(shared);
	if (error)
		goto out_shared;
	error = kobox_linux_fs_shared_enter(shared, false, true, &guard);
	if (error)
		goto out_shared;
	error = kobox_linux_fs_shared_export(shared, file, 1, 1, 0,
		2 * PAGE_SIZE, 0, 1, &id, &size);
	if (!error)
		error = kobox_linux_fs_shared_export(shared, file, 1, 2, 0,
			PAGE_SIZE, 0, 1, &id, &size);
	if (error)
		goto out_locked;
	{
		struct kobox_linux_fs_shared_guard nowait;
		if (kobox_linux_fs_shared_enter(shared, true, false, &nowait) != -EAGAIN ||
		    nowait.mode != KOBOX_FS_SHARED_NONE) {
			error = -EINVAL;
			goto out_locked;
		}
	}
	/* A real extra folio reference must remain visible even though two owned
	 * readonly slots alias the same folio and another slot is an EOF hole. */
	extra = shmem_read_folio(file->f_mapping, 0);
	if (IS_ERR(extra)) {
		error = PTR_ERR(extra);
		extra = NULL;
		goto out_locked;
	}
	if (!kobox_linux_fs_shared_add_seals(shared, file, F_SEAL_WRITE, &result) ||
	    result != -EBUSY || SHMEM_I(file_inode(file))->seals) {
		error = -EINVAL;
		goto out_locked;
	}
	/* Failure must undo mapping_deny_writable, not poison future writers. */
	error = mapping_map_writable(file->f_mapping);
	if (error)
		goto out_locked;
	mapping_unmap_writable(file->f_mapping);
	folio_put(extra);
	extra = NULL;
	if (!kobox_linux_fs_shared_add_seals(shared, file, F_SEAL_WRITE, &result) ||
	    result || !(SHMEM_I(file_inode(file))->seals & F_SEAL_WRITE)) {
		error = -EINVAL;
		goto out_locked;
	}
	error = mapping_map_writable(file->f_mapping);
	if (!error) {
		mapping_unmap_writable(file->f_mapping);
		error = -EINVAL;
	} else if (error == -EPERM) {
		error = 0;
	}
out_locked:
	if (extra)
		folio_put(extra);
	kobox_linux_fs_shared_leave(shared, false, &guard);
	if (!error)
		error = gate_exported_read(shared, file);
	if (!error)
		error = gate_scoped_mutation(shared, file);
out_shared:
	kobox_linux_fs_shared_destroy(shared);
out_file:
	fput(file);
	if (!error)
		pr_info("FS_SHARED_SEALS own_ro_duplicates_hole=PASS external_pin=EBUSY rollback_writer=PASS unpin_then_seal=PASS\n");
	if (!error)
		pr_info("FS_SHARED_ADMISSION concurrent_empty=PASS publication_waits=PASS nowait_no_side_effect=PASS\n");
	if (!error)
		pr_info("FS_SHARED_READ exported_buffered=PASS direct_punt=PASS\n");
	if (!error)
		pr_info("FS_SHARED_SCOPED unrelated_mutations=PASS target_punt=PASS publication_waits=PASS\n");
	return error;
}
