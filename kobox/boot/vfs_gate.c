// SPDX-License-Identifier: GPL-2.0-only

#include "host.h"
#include "vfs_gate.h"

#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/task_work.h>

#define IO_BYTES (2 * PAGE_SIZE + 37)
#define REUSE_BATCH 1024
#define GATE_WAIT (5 * HZ)
#define MAX_COMPANIONS 64

struct vfs_case {
	struct kobox_linux_vfs_report *report;
	struct vfsmount *mnt;
	struct file *file;
	struct inode *inode;
	struct folio *folios[3];
	struct file *companions[MAX_COMPANIONS];
	unsigned int companions_count;
	gfp_t folio_gfp;
	unsigned long file_address;
	unsigned long inode_address;
	struct task_struct *reader;
	struct task_struct *closer;
	struct completion entered;
	struct completion inspected;
	struct completion closed;
	bool inspect;
	bool release;
	bool expired;
	bool inode_retired;
	struct rcu_head marker;
	atomic_t callbacks;
	char *written;
	char *read;
	int fd;
};

static int fail(struct vfs_case *test, unsigned int line, int result)
{
	test->report->line = line;
	test->report->result = result;
	test->report->warnings = kobox_linux_exception_warnings();
	return -EINVAL;
}

static int wait_for_stop(void)
{
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int hold_inode_rcu(void *argument)
{
	struct vfs_case *test = argument;
	ktime_t deadline = ktime_add_ms(ktime_get(), 5000);

	rcu_read_lock();
	complete(&test->entered);
	/* Acquire the controller's inspection/release phases before using data. */
	while (!smp_load_acquire(&test->release)) {
		/* Eviction precedes inspect; completion publishes our observation. */
		if (smp_load_acquire(&test->inspect)) {
			test->inode_retired =
				(READ_ONCE(test->inode->i_state) & (I_FREEING | I_CLEAR)) ==
				(I_FREEING | I_CLEAR) && !test->inode->i_mapping->nrpages;
			WRITE_ONCE(test->inspect, false);
			complete(&test->inspected);
		}
		if (ktime_get() > deadline) {
			WRITE_ONCE(test->expired, true);
			break;
		}
		cpu_relax();
	}
	rcu_read_unlock();
	return wait_for_stop();
}

static void reclaimed_marker(struct rcu_head *head)
{
	struct vfs_case *test = container_of(head, struct vfs_case, marker);

	atomic_inc(&test->callbacks);
}

static int close_in_kthread(void *argument)
{
	struct vfs_case *test = argument;

	fput(test->file);
	complete(&test->closed);
	return wait_for_stop();
}

static int unlink_name(struct vfsmount *mnt, const char *name)
{
	struct inode *parent = d_inode(mnt->mnt_root);
	struct dentry *dentry;
	struct qstr component = QSTR_INIT(name, strlen(name));
	int result;

	result = mnt_want_write(mnt);
	if (result)
		return result;
	inode_lock(parent);
	dentry = lookup_one(mnt_idmap(mnt), &component, mnt->mnt_root);
	if (IS_ERR(dentry)) {
		result = PTR_ERR(dentry);
	} else {
		result = d_is_negative(dentry) ? -ENOENT :
			vfs_unlink(mnt_idmap(mnt), parent, dentry, NULL);
		dput(dentry);
	}
	inode_unlock(parent);
	mnt_drop_write(mnt);
	return result;
}

static int check_io(struct vfs_case *test, struct file *file)
{
	loff_t position = 0;
	ssize_t result;

	memset(test->read, 0x5a, IO_BYTES + 13);
	result = kernel_read(file, test->read, IO_BYTES + 13, &position);
	if (result != IO_BYTES + 13 || position != IO_BYTES + 13 ||
	    memchr_inv(test->read, 0, 13) ||
	    memcmp(test->read + 13, test->written, IO_BYTES))
		return fail(test, __LINE__, result);
	result = kernel_read(file, test->read, 1, &position);
	if (result || position != IO_BYTES + 13)
		return fail(test, __LINE__, result);
	test->report->io_checks++;
	return 0;
}

static int linked_close_reopen(struct vfs_case *test)
{
	struct file *file;
	loff_t position = 13;
	ssize_t result;

	file = file_open_root_mnt(test->mnt, "linked", O_CREAT | O_EXCL | O_RDWR, 0600);
	if (IS_ERR(file))
		return fail(test, __LINE__, PTR_ERR(file));
	result = kernel_write(file, test->written, IO_BYTES, &position);
	if (result != IO_BYTES || filp_close(file, current->files))
		return fail(test, __LINE__, result);
	/* No file, inode or folio reference is retained across this final close. */
	task_work_run();
	file = file_open_root_mnt(test->mnt, "linked", O_RDONLY, 0);
	if (IS_ERR(file))
		return fail(test, __LINE__, PTR_ERR(file));
	if (check_io(test, file) || filp_close(file, current->files))
		return fail(test, __LINE__, -EINVAL);
	task_work_run();
	result = unlink_name(test->mnt, "linked");
	if (result)
		return fail(test, __LINE__, result);
	rcu_barrier();
	test->report->linked_reopens++;
	return 0;
}

static int create_observable_file(struct vfs_case *test)
{
	struct file *companion;
	unsigned int index;

	/* Retain independent live objects in each target's slab. This prevents
	 * empty-slab destruction from turning a freed object into another cache's
	 * storage before our allocation-reuse oracle runs. No allocator policy or
	 * target reference is changed; companions use the separate shmem mount.
	 */
	for (index = 0; index < ARRAY_SIZE(test->companions); index++) {
		companion = shmem_file_setup("vfs-companion", 0, VM_NORESERVE);
		if (IS_ERR(companion))
			return fail(test, __LINE__, PTR_ERR(companion));
		test->companions[test->companions_count++] = companion;
		test->file = file_open_root_mnt(test->mnt, "object", O_CREAT | O_EXCL | O_RDWR, 0600);
		if (IS_ERR(test->file))
			return fail(test, __LINE__, PTR_ERR(test->file));
		if (virt_to_head_page(companion) == virt_to_head_page(test->file) &&
		    virt_to_head_page(SHMEM_I(file_inode(companion))) ==
		    virt_to_head_page(SHMEM_I(file_inode(test->file))))
			return 0;
		if (unlink_name(test->mnt, "object"))
			return fail(test, __LINE__, -EINVAL);
		__fput_sync(test->file);
		test->file = NULL;
		rcu_barrier();
	}
	return fail(test, __LINE__, -ENOSPC);
}

static int create_file(struct vfs_case *test)
{
	struct file_system_type *type;
	struct file *other;
	loff_t position = 13;
	unsigned int index;
	ssize_t result;

	type = get_fs_type("tmpfs");
	if (!type)
		return fail(test, __LINE__, -ENODEV);
	test->mnt = kern_mount(type);
	put_filesystem(type);
	if (IS_ERR(test->mnt))
		return fail(test, __LINE__, PTR_ERR(test->mnt));
	if (test->mnt->mnt_sb->s_magic != TMPFS_MAGIC)
		return fail(test, __LINE__, -EINVAL);
	test->report->mounts++;
	for (index = 0; index < IO_BYTES; index++)
		test->written[index] = (index * 17 + test->report->cpu + 1) % 251;
	if (linked_close_reopen(test) || create_observable_file(test))
		return -EINVAL;
	test->inode = igrab(file_inode(test->file));
	if (!test->inode || !shmem_mapping(test->file->f_mapping) ||
	    test->file->f_mapping != &test->inode->i_data)
		return fail(test, __LINE__, -EINVAL);
	test->file_address = (unsigned long)test->file;
	test->inode_address = (unsigned long)test->inode;
	test->folio_gfp = mapping_gfp_mask(test->inode->i_mapping);
	result = kernel_write(test->file, test->written, IO_BYTES, &position);
	if (result != IO_BYTES || position != IO_BYTES + 13 ||
	    i_size_read(test->inode) != IO_BYTES + 13)
		return fail(test, __LINE__, result);
	if (check_io(test, test->file))
		return -EINVAL;
	for (index = 0; index < ARRAY_SIZE(test->folios); index++) {
		test->folios[index] = filemap_get_folio(test->inode->i_mapping, index);
		if (IS_ERR(test->folios[index]) || folio_order(test->folios[index]))
			return fail(test, __LINE__, -EINVAL);
	}
	other = file_open_root_mnt(test->mnt, "object", O_RDONLY, 0);
	if (IS_ERR(other))
		return fail(test, __LINE__, PTR_ERR(other));
	if (file_inode(other) != test->inode || check_io(test, other))
		return fail(test, __LINE__, -EINVAL);
	position = 0;
	/* vfs_write rejects mode before touching its user buffer. kernel_write
	 * instead requires a writable kernel caller and warns on misuse.
	 */
	result = vfs_write(other, NULL, 1, &position);
	if (result != -EBADF || filp_close(other, current->files))
		return fail(test, __LINE__, result);
	task_work_run();
	other = file_open_root_mnt(test->mnt, "object", O_CREAT | O_EXCL | O_RDWR, 0600);
	if (other != ERR_PTR(-EEXIST))
		return fail(test, __LINE__, IS_ERR(other) ? PTR_ERR(other) : 0);
	test->report->negative_checks += 2;
	/* Unmounting the owner's reference cannot destroy a referenced mount. */
	mntget(test->mnt);
	kern_unmount(test->mnt);
	if (check_io(test, test->file))
		return -EINVAL;
	test->fd = get_unused_fd_flags(O_CLOEXEC);
	if (test->fd < 0)
		return fail(test, __LINE__, test->fd);
	fd_install(test->fd, get_file(test->file));
	other = fget(test->fd);
	if (other != test->file || file_count(other) != 3)
		return fail(test, __LINE__, -EINVAL);
	fput(other);
	return 0;
}

static int close_and_unlink(struct vfs_case *test)
{
	struct file *other;
	int result;

	if (test->report->unlink_first) {
		result = unlink_name(test->mnt, "object");
		if (result)
			return fail(test, __LINE__, result);
	}
	if (close_fd(test->fd) || fget(test->fd) || close_fd(test->fd) != -EBADF ||
	    file_count(test->file) != 1 || check_io(test, test->file))
		return fail(test, __LINE__, -EINVAL);
	test->report->negative_checks++;
	if (!test->report->unlink_first) {
		result = unlink_name(test->mnt, "object");
		if (result)
			return fail(test, __LINE__, result);
	}
	other = file_open_root_mnt(test->mnt, "object", O_RDONLY, 0);
	if (other != ERR_PTR(-ENOENT) || unlink_name(test->mnt, "object") != -ENOENT ||
	    test->inode->i_nlink || check_io(test, test->file))
		return fail(test, __LINE__, -EINVAL);
	test->report->negative_checks += 2;
	/* A new object at the same pathname must not replace the open old one. */
	other = file_open_root_mnt(test->mnt, "object", O_CREAT | O_EXCL | O_RDWR, 0600);
	if (IS_ERR(other) || file_inode(other) == test->inode ||
	    i_size_read(file_inode(other)) || check_io(test, test->file))
		return fail(test, __LINE__, -EINVAL);
	result = unlink_name(test->mnt, "object");
	if (result || filp_close(other, current->files))
		return fail(test, __LINE__, result);
	task_work_run();
	return 0;
}

static int release_file(struct vfs_case *test)
{
	ktime_t deadline = ktime_add_ms(ktime_get(), 5000);
	struct folio *folio;

	if (test->report->deferred) {
		test->closer = kthread_create(close_in_kthread, test, "vfs-close");
		if (IS_ERR(test->closer))
			return fail(test, __LINE__, PTR_ERR(test->closer));
		kthread_bind(test->closer, test->report->cpu ^ 1);
		wake_up_process(test->closer);
		if (!wait_for_completion_timeout(&test->closed, GATE_WAIT) ||
		    kthread_stop(test->closer))
			return fail(test, __LINE__, -ETIMEDOUT);
		/* Observe real delayed-work progress before using its drain API. */
		while (atomic_read(&test->inode->i_count) != 1) {
			if (ktime_get() > deadline)
				return fail(test, __LINE__, -ETIMEDOUT);
			usleep_range(500, 1000);
		}
		/* No locks held, and these private files have no umount dependency. */
		flush_delayed_fput();
		test->report->delayed_fput_cases++;
	} else {
		if (task_work_pending(current))
			return fail(test, __LINE__, -EBUSY);
		fput(test->file);
		if (!task_work_pending(current) ||
		    atomic_read(&test->inode->i_count) != 2)
			return fail(test, __LINE__, -EINVAL);
		/* The hosted PID 1 harness explicitly reaches its task-work boundary. */
		task_work_run();
		test->report->task_work_cases++;
	}
	test->file = NULL;
	if (atomic_read(&test->inode->i_count) != 1 ||
	    test->inode->i_mapping->nrpages != ARRAY_SIZE(test->folios) ||
	    test->inode->i_state & (I_FREEING | I_CLEAR))
		return fail(test, __LINE__, -EINVAL);
	folio = shmem_read_folio_gfp(test->inode->i_mapping, 0, GFP_KERNEL);
	if (IS_ERR(folio) || folio != test->folios[0] ||
	    memcmp((char *)folio_address(folio) + 13, test->written, PAGE_SIZE - 13))
		return fail(test, __LINE__, -EINVAL);
	folio_put(folio);
	return 0;
}

static int release_inode(struct vfs_case *test)
{
	unsigned int index;

	test->reader = kthread_create(hold_inode_rcu, test, "vfs-reader");
	if (IS_ERR(test->reader))
		return fail(test, __LINE__, PTR_ERR(test->reader));
	kthread_bind(test->reader, test->report->cpu ^ 1);
	wake_up_process(test->reader);
	if (!wait_for_completion_timeout(&test->entered, GATE_WAIT))
		return fail(test, __LINE__, -ETIMEDOUT);
	iput(test->inode);
	call_rcu(&test->marker, reclaimed_marker);
	/* Publish completion of eviction to the already-held RCU reader. */
	smp_store_release(&test->inspect, true);
	if (!wait_for_completion_timeout(&test->inspected, GATE_WAIT) ||
	    !test->inode_retired || READ_ONCE(test->expired) ||
	    atomic_read(&test->callbacks))
		return fail(test, __LINE__, -EINVAL);
	for (index = 0; index < ARRAY_SIZE(test->folios); index++) {
		if (folio_mapping(test->folios[index]) ||
		    folio_ref_count(test->folios[index]) != 1)
			return fail(test, __LINE__, -EINVAL);
	}
	/* Release the reader only after all held-state observations complete. */
	smp_store_release(&test->release, true);
	if (kthread_stop(test->reader) || READ_ONCE(test->expired))
		return fail(test, __LINE__, -EINVAL);
	rcu_barrier();
	if (atomic_read(&test->callbacks) != 1)
		return fail(test, __LINE__, -EINVAL);
	test->inode = NULL;
	test->report->rcu_holds++;
	return 0;
}

static int reuse_files_and_inodes(struct vfs_case *test)
{
	struct file **files;
	unsigned int cpu, index, count = 0;
	bool file_seen = false, inode_seen = false;

	files = kcalloc(2 * REUSE_BATCH, sizeof(*files), GFP_KERNEL);
	if (!files)
		return fail(test, __LINE__, -ENOMEM);
	/* No retired object is dereferenced. Successful fresh allocations prove
	 * actual return to the real filp and shmem-inode allocators after draining.
	 * Hold probes simultaneously and visit both freeing CPUs' caches.
	 */
	for_each_online_cpu(cpu) {
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
			return fail(test, __LINE__, -EINVAL);
		for (index = 0; index < REUSE_BATCH && !(file_seen && inode_seen); index++) {
			struct file *file = shmem_file_setup("vfs-reuse", 0, VM_NORESERVE);

			if (IS_ERR(file))
				return fail(test, __LINE__, PTR_ERR(file));
			files[count++] = file;
			file_seen |= (unsigned long)file == test->file_address;
			inode_seen |= (unsigned long)file_inode(file) == test->inode_address;
		}
	}
	if (!file_seen || !inode_seen)
		return fail(test, __LINE__, file_seen | (inode_seen << 1));
	for (index = 0; index < count; index++)
		__fput_sync(files[index]);
	for (index = 0; index < test->companions_count; index++)
		__fput_sync(test->companions[index]);
	kfree(files);
	rcu_barrier();
	test->report->file_reclaims++;
	test->report->inode_reclaims++;
	return set_cpus_allowed_ptr(current, cpumask_of(test->report->cpu));
}

static int reuse_folios(struct vfs_case *test)
{
	struct page **pages;
	unsigned long pfns[ARRAY_SIZE(test->folios)];
	unsigned int cpu, index, which, count = 0, seen = 0;

	pages = kcalloc(2 * REUSE_BATCH, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return fail(test, __LINE__, -ENOMEM);
	for (index = 0; index < ARRAY_SIZE(test->folios); index++) {
		size_t offset = index * PAGE_SIZE;
		size_t length = min_t(size_t, PAGE_SIZE, IO_BYTES + 13 - offset);

		if (folio_mapping(test->folios[index]) ||
		    folio_ref_count(test->folios[index]) != 1 ||
		    memcmp(folio_address(test->folios[index]), test->read + offset, length))
			return fail(test, __LINE__, -EINVAL);
		pfns[index] = folio_pfn(test->folios[index]);
		folio_put(test->folios[index]);
		test->folios[index] = NULL;
	}
	for_each_online_cpu(cpu) {
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
			return fail(test, __LINE__, -EINVAL);
		for (index = 0; index < REUSE_BATCH && seen != GENMASK(2, 0); index++) {
			/* Match shmem's zone and mobility class, not an unrelated PCP list. */
			struct page *page = alloc_page(test->folio_gfp);

			if (!page)
				return fail(test, __LINE__, -ENOMEM);
			pages[count++] = page;
			for (which = 0; which < ARRAY_SIZE(pfns); which++)
				if (page_to_pfn(page) == pfns[which])
					seen |= BIT(which);
		}
	}
	if (seen != GENMASK(2, 0))
		return fail(test, __LINE__, seen);
	for (index = 0; index < count; index++)
		__free_page(pages[index]);
	kfree(pages);
	test->report->folio_reclaims += ARRAY_SIZE(pfns);
	return set_cpus_allowed_ptr(current, cpumask_of(test->report->cpu));
}

static int release_mount(struct vfs_case *test)
{
	unsigned long address = (unsigned long)test->mnt->mnt_sb;
	void **objects;
	unsigned int cpu, index, round, count;
	bool seen = false;

	objects = kcalloc(2 * REUSE_BATCH, sizeof(*objects), GFP_KERNEL);
	if (!objects)
		return fail(test, __LINE__, -ENOMEM);
	mntput(test->mnt);
	test->mnt = NULL;
	task_work_run();
	rcu_barrier();
	/* The superblock uses RCU -> system work -> kfree. A GP alone cannot
	 * certify that work completed. Observe allocator reuse, never flush work
	 * through a possibly already freed superblock pointer.
	 */
	for (round = 0; round < 8 && !seen; round++) {
		count = 0;
		for_each_online_cpu(cpu) {
			if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
				return fail(test, __LINE__, -EINVAL);
			for (index = 0; index < REUSE_BATCH && !seen; index++) {
				objects[count] = kzalloc(sizeof(struct super_block), GFP_KERNEL);
				if (!objects[count])
					return fail(test, __LINE__, -ENOMEM);
				seen = (unsigned long)objects[count++] == address;
			}
		}
		for (index = 0; index < count; index++)
			kfree(objects[index]);
		if (!seen)
			usleep_range(4000, 5000);
	}
	if (!seen)
		return fail(test, __LINE__, -ETIMEDOUT);
	kfree(objects);
	test->report->super_reclaims++;
	return set_cpus_allowed_ptr(current, cpumask_of(test->report->cpu));
}

static int run_case(struct kobox_linux_vfs_report *report)
{
	struct vfs_case *test;
	int result;

	/* Failures retain storage until the launcher's immediate process exit. */
	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->report = report;
	test->written = kmalloc(IO_BYTES, GFP_KERNEL);
	test->read = kmalloc(IO_BYTES + 13, GFP_KERNEL);
	if (!test->written || !test->read)
		return fail(test, __LINE__, -ENOMEM);
	init_completion(&test->entered);
	init_completion(&test->inspected);
	init_completion(&test->closed);
	atomic_set(&test->callbacks, 0);
	report->phase = 1;
	result = create_file(test);
	if (result)
		return result;
	report->phase = 2;
	result = close_and_unlink(test);
	if (result)
		return result;
	report->phase = 3;
	result = release_file(test);
	if (result)
		return result;
	report->phase = 4;
	result = release_inode(test);
	if (result)
		return result;
	report->phase = 5;
	result = reuse_files_and_inodes(test);
	if (result)
		return result;
	report->phase = 6;
	result = reuse_folios(test);
	if (result)
		return result;
	report->phase = 7;
	result = release_mount(test);
	if (result)
		return result;
	kfree(test->written);
	kfree(test->read);
	kfree(test);
	report->cases++;
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_vfs_verify(struct kobox_linux_vfs_report *report)
{
	cpumask_t saved;
	unsigned int cpu, deferred, unlink_first;
	int result;

	if (!report || report->size != sizeof(*report) ||
	    system_state != SYSTEM_RUNNING || task_pid_nr(current) != 1 ||
	    current->flags & PF_KTHREAD || num_online_cpus() != 2)
		return -EINVAL;
	cpumask_copy(&saved, current->cpus_ptr);
	task_work_run();
	for_each_online_cpu(cpu) {
		report->cpu = cpu;
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
			return -EINVAL;
		for (deferred = 0; deferred < 2; deferred++) {
			report->deferred = deferred;
			for (unlink_first = 0; unlink_first < 2; unlink_first++) {
				report->unlink_first = unlink_first;
				result = run_case(report);
				if (result)
					return result;
			}
		}
	}
	report->warnings = kobox_linux_exception_warnings();
	if (report->warnings)
		return -EINVAL;
	report->phase = 8;
	return set_cpus_allowed_ptr(current, &saved);
}
