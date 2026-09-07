// SPDX-License-Identifier: GPL-2.0-only

#include "host.h"
#include "shmem_gate.h"

#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/falloc.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>

#define FILE_BYTES (4 * PAGE_SIZE + 73)
#define BUFFER_BYTES (6 * PAGE_SIZE)
#define REUSE_BATCH 1024
#define RACE_ROUNDS 64
#define GATE_WAIT (5 * HZ)

enum worker_action {
	INCREMENT,
	READ_PAGE,
	WRITE_PAGE,
	TRUNCATE,
	PUNCH,
};

struct shmem_case;

struct shmem_worker {
	struct shmem_case *test;
	struct file *file;
	struct task_struct *task;
	struct completion go;
	struct completion entered;
	struct completion done;
	enum worker_action action;
	bool finish;
	int result;
	unsigned long pfn;
	char *buffer;
};

struct shmem_case {
	struct kobox_linux_shmem_report *report;
	struct vfsmount *mnt;
	struct file *file;
	struct file *peer;
	struct inode *inode;
	struct shmem_sb_info *sbinfo;
	struct shmem_worker workers[2];
	unsigned long committed;
	unsigned long free_ispace;
	unsigned long inode_charge;
	gfp_t folio_gfp;
	char *expected;
	char *read;
};

static int fail(struct shmem_case *test, unsigned int line, int result)
{
	test->report->line = line;
	test->report->result = result;
	test->report->warnings = kobox_linux_exception_warnings();
	return -EINVAL;
}

/* These exact counters are sampled only after every mutator has joined its
 * operation. The mount is private; no other fixture uses its accounting.
 */
static int account(struct shmem_case *test, loff_t size, unsigned long pages)
{
	struct shmem_inode_info *info = SHMEM_I(test->inode);
	unsigned long charged = test->report->noreserve ? pages :
		DIV_ROUND_UP(size, PAGE_SIZE);

	if (i_size_read(test->inode) != size ||
	    test->inode->i_mapping->nrpages != pages ||
	    info->alloced != pages || info->swapped ||
	    test->inode->i_blocks != pages * (PAGE_SIZE / 512) ||
	    percpu_counter_sum(&test->sbinfo->used_blocks) != pages ||
	    vm_memory_committed() != test->committed + charged ||
	    test->sbinfo->free_ispace != test->free_ispace - test->inode_charge)
		return fail(test, __LINE__, pages);
	test->report->accounting_checks++;
	return 0;
}

static int read_expected(struct shmem_case *test, loff_t size)
{
	struct file *files[] = {test->file, test->peer};
	unsigned int index;
	loff_t position;
	ssize_t result;

	for (index = 0; index < ARRAY_SIZE(files); index++) {
		position = 0;
		memset(test->read, 0xa7, BUFFER_BYTES);
		result = kernel_read(files[index], test->read, BUFFER_BYTES, &position);
		if (result != size || position != size ||
		    memcmp(test->read, test->expected, size) ||
		    memchr_inv(test->read + size, 0xa7, BUFFER_BYTES - size))
			return fail(test, __LINE__, result);
		result = kernel_read(files[index], test->read, 1, &position);
		if (result || position != size)
			return fail(test, __LINE__, result);
		test->report->io_checks++;
	}
	return 0;
}

static int resize(struct shmem_case *test, loff_t size)
{
	int result = vfs_truncate(&test->file->f_path, size);

	return result ? fail(test, __LINE__, result) : 0;
}

static int fill(struct shmem_case *test, size_t size, unsigned char value)
{
	loff_t position = 0;
	ssize_t result;

	/* Pre-reserved shmem grows through setattr's reservation accounting. */
	if (resize(test, size))
		return -EINVAL;
	memset(test->expected, value, size);
	result = kernel_write(test->file, test->expected, size, &position);
	if (result != size || position != size)
		return fail(test, __LINE__, result);
	return 0;
}

static int create_file(struct shmem_case *test)
{
	struct file_system_type *type;
	struct fs_context *fc;
	int result;

	type = get_fs_type("tmpfs");
	if (!type)
		return fail(test, __LINE__, -ENOENT);
	/* A regular, unattached mount exercises block AND inode limits. Internal
	 * SB_KERNMOUNT tmpfs deliberately bypasses both; don't alter its counters.
	 */
	fc = fs_context_for_mount(type, 0);
	put_filesystem(type);
	if (IS_ERR(fc))
		return fail(test, __LINE__, PTR_ERR(fc));
	result = vfs_parse_fs_string(fc, "size", "65536");
	if (!result)
		result = vfs_parse_fs_string(fc, "nr_inodes", "16");
	if (result)
		return fail(test, __LINE__, result);
	test->mnt = fc_mount(fc);
	put_fs_context(fc);
	if (IS_ERR(test->mnt))
		return fail(test, __LINE__, PTR_ERR(test->mnt));
	test->sbinfo = test->mnt->mnt_sb->s_fs_info;
	if (test->sbinfo->max_blocks != 16 || !test->sbinfo->max_inodes ||
	    percpu_counter_sum(&test->sbinfo->used_blocks))
		return fail(test, __LINE__, -EINVAL);
	test->free_ispace = test->sbinfo->free_ispace;
	test->committed = vm_memory_committed();
	test->file = shmem_file_setup_with_mnt(test->mnt, "shmem-gate", FILE_BYTES,
					test->report->noreserve ? VM_NORESERVE : 0);
	if (IS_ERR(test->file))
		return fail(test, __LINE__, PTR_ERR(test->file));
	test->inode = file_inode(test->file);
	test->inode_charge = test->free_ispace - test->sbinfo->free_ispace;
	test->folio_gfp = mapping_gfp_mask(test->inode->i_mapping);
	test->peer = dentry_open(&test->file->f_path, O_RDWR, current_cred());
	if (IS_ERR(test->peer))
		return fail(test, __LINE__, PTR_ERR(test->peer));
	if (!test->inode_charge || test->file == test->peer ||
	    file_inode(test->peer) != test->inode ||
	    test->peer->f_mapping != test->file->f_mapping ||
	    !shmem_mapping(test->file->f_mapping))
		return fail(test, __LINE__, -EINVAL);
	test->report->sharing_checks++;
	return account(test, FILE_BYTES, 0);
}

static int reuse_page(struct shmem_case *test, struct folio *folio)
{
	struct page **pages;
	unsigned long pfn;
	unsigned int cpu, index, count = 0;
	bool seen = false;

	pages = kcalloc(2 * REUSE_BATCH, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return fail(test, __LINE__, -ENOMEM);
	lru_add_drain_all();
	if (folio_mapping(folio) || folio_ref_count(folio) != 1)
		return fail(test, __LINE__, -EINVAL);
	pfn = folio_pfn(folio);
	folio_put(folio);
	/* Never inspect a freed folio. Reacquire its numeric PFN from buddy using
	 * the original zone/mobility class, on both CPU-local allocation lists.
	 */
	for_each_online_cpu(cpu) {
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
			return fail(test, __LINE__, -EINVAL);
		for (index = 0; index < REUSE_BATCH && !seen; index++) {
			struct page *page = alloc_page(test->folio_gfp);

			if (!page)
				return fail(test, __LINE__, -ENOMEM);
			pages[count++] = page;
			seen = page_to_pfn(page) == pfn;
		}
	}
	for (index = 0; index < count; index++)
		__free_page(pages[index]);
	kfree(pages);
	if (!seen)
		return fail(test, __LINE__, -ETIMEDOUT);
	test->report->page_reclaims++;
	return set_cpus_allowed_ptr(current, cpumask_of(test->report->cpu));
}

static int boundaries(struct shmem_case *test)
{
	struct folio *folio = NULL;
	loff_t position = 0;
	unsigned int index;
	int result;

	memset(test->expected, 0, BUFFER_BYTES);
	if (read_expected(test, FILE_BYTES) || account(test, FILE_BYTES, 0))
		return -EINVAL;
	result = shmem_get_folio(test->inode, 0, 0, &folio, SGP_READ);
	if (result || folio)
		return fail(test, __LINE__, result);
	result = shmem_get_folio(test->inode, 0, 0, &folio, SGP_NOALLOC);
	if (result != -ENOENT || folio)
		return fail(test, __LINE__, result);
	result = shmem_get_folio(test->inode, 5, 0, &folio, SGP_CACHE);
	if (result != -EINVAL || folio ||
	    vfs_truncate(&test->file->f_path, -1) != -EINVAL)
		return fail(test, __LINE__, result);
	if (vfs_fallocate(test->file, FALLOC_FL_PUNCH_HOLE, 0, PAGE_SIZE) != -EOPNOTSUPP ||
	    vfs_fallocate(test->file, FALLOC_FL_KEEP_SIZE, -1, 1) != -EINVAL ||
	    vfs_fallocate(test->file, 0, 0, 0) != -EINVAL ||
	    vfs_fallocate(test->file, FALLOC_FL_ZERO_RANGE, 0, PAGE_SIZE) != -EOPNOTSUPP ||
	    vfs_fallocate(test->file, 0, LLONG_MAX, 1) != -EFBIG)
		return fail(test, __LINE__, -EINVAL);
	test->report->negative_checks += 8;
	if (account(test, FILE_BYTES, 0))
		return -EINVAL;
	result = shmem_get_folio(test->inode, 4, 0, &folio, SGP_CACHE);
	if (result)
		return fail(test, __LINE__, result);
	if (!folio_test_uptodate(folio) || folio_order(folio) ||
	    memchr_inv(folio_address(folio), 0, PAGE_SIZE))
		return fail(test, __LINE__, -EINVAL);
	folio_mark_dirty(folio);
	folio_unlock(folio);
	folio_put(folio);
	if (account(test, FILE_BYTES, 1) || resize(test, 0) ||
	    account(test, 0, 0) || resize(test, FILE_BYTES))
		return -EINVAL;
	for (index = 0; index < FILE_BYTES; index++)
		test->expected[index] = index % 251 + 1;
	result = kernel_write(test->file, test->expected, FILE_BYTES, &position);
	if (result != FILE_BYTES || read_expected(test, FILE_BYTES) ||
	    account(test, FILE_BYTES, 5))
		return fail(test, __LINE__, result);
	folio = shmem_read_folio(test->inode->i_mapping, 1);
	if (IS_ERR(folio))
		return fail(test, __LINE__, PTR_ERR(folio));
	result = vfs_fallocate(test->peer, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			       PAGE_SIZE - 13, 2 * PAGE_SIZE + 26);
	if (result || account(test, FILE_BYTES, 3) || folio_mapping(folio) ||
	    memcmp(folio_address(folio), test->expected + PAGE_SIZE, PAGE_SIZE))
		return fail(test, __LINE__, result);
	memset(test->expected + PAGE_SIZE - 13, 0, 2 * PAGE_SIZE + 26);
	if (read_expected(test, FILE_BYTES) || reuse_page(test, folio))
		return -EINVAL;
	if (resize(test, PAGE_SIZE + 17) || account(test, PAGE_SIZE + 17, 1))
		return -EINVAL;
	memset(test->expected + PAGE_SIZE + 17, 0, BUFFER_BYTES - PAGE_SIZE - 17);
	if (resize(test, FILE_BYTES) || read_expected(test, FILE_BYTES) ||
	    account(test, FILE_BYTES, 1))
		return -EINVAL;
	/* A punch beyond EOF neither grows the file nor allocates hole pages. */
	result = vfs_fallocate(test->file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			       5 * PAGE_SIZE, PAGE_SIZE);
	if (result || account(test, FILE_BYTES, 1))
		return fail(test, __LINE__, result);
	/* Shrink inside an allocated, nonzero page, then at an exact page
	 * boundary. Regrowth must never reveal the old discarded tail bytes.
	 */
	if (fill(test, FILE_BYTES, 0x39) || resize(test, PAGE_SIZE + 17) ||
	    account(test, PAGE_SIZE + 17, 2))
		return -EINVAL;
	memset(test->expected + PAGE_SIZE + 17, 0, BUFFER_BYTES - PAGE_SIZE - 17);
	if (resize(test, FILE_BYTES) || read_expected(test, FILE_BYTES) ||
	    account(test, FILE_BYTES, 2) || resize(test, PAGE_SIZE) ||
	    account(test, PAGE_SIZE, 1))
		return -EINVAL;
	memset(test->expected + PAGE_SIZE, 0, BUFFER_BYTES - PAGE_SIZE);
	if (resize(test, FILE_BYTES) || read_expected(test, FILE_BYTES) ||
	    account(test, FILE_BYTES, 1))
		return -EINVAL;
	return 0;
}

static int worker_operation(struct shmem_worker *worker)
{
	struct inode *inode = file_inode(worker->file);
	struct folio *folio;
	unsigned long *value;
	loff_t position = PAGE_SIZE;
	ssize_t result;
	unsigned int index;

	switch (worker->action) {
	case INCREMENT:
		result = shmem_get_folio(inode, 0, 0, &folio, SGP_CACHE);
		if (result)
			return result;
		worker->pfn = folio_pfn(folio);
		value = folio_address(folio);
		(*value)++;
		folio_mark_dirty(folio);
		folio_unlock(folio);
		folio_put(folio);
		return 0;
	case READ_PAGE:
		memset(worker->buffer, 0xa7, PAGE_SIZE);
		result = kernel_read(worker->file, worker->buffer, PAGE_SIZE, &position);
		if (result < 0 || result > PAGE_SIZE || position != PAGE_SIZE + result)
			return -EIO;
		/* A concurrent shrink may shorten the read. Linux does not promise
		 * an atomic snapshot across a racing write or hole punch.
		 */
		for (index = 0; index < result; index++)
			if (worker->buffer[index] && worker->buffer[index] != 0x5a)
				return -EIO;
		return memchr_inv(worker->buffer + result, 0xa7, PAGE_SIZE - result) ?
			-EIO : 0;
	case WRITE_PAGE:
		memset(worker->buffer, 0x5a, PAGE_SIZE);
		result = kernel_write(worker->file, worker->buffer, PAGE_SIZE, &position);
		return result == PAGE_SIZE && position == 2 * PAGE_SIZE ? 0 : -EIO;
	case TRUNCATE:
		return vfs_truncate(&worker->file->f_path, 0);
	case PUNCH:
		return vfs_fallocate(worker->file,
			FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, PAGE_SIZE, PAGE_SIZE);
	}
	return -EINVAL;
}

static int shmem_worker(void *argument)
{
	struct shmem_worker *worker = argument;

	for (;;) {
		wait_for_completion(&worker->go);
		if (READ_ONCE(worker->finish))
			break;
		complete(&worker->entered);
		worker->result = worker_operation(worker);
		complete(&worker->done);
	}
	/* Keep the task alive until kthread_stop owns its join reference. */
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int start_workers(struct shmem_case *test)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		struct shmem_worker *worker = &test->workers[cpu];

		worker->test = test;
		worker->file = cpu ? test->peer : test->file;
		worker->buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!worker->buffer)
			return fail(test, __LINE__, -ENOMEM);
		init_completion(&worker->go);
		init_completion(&worker->entered);
		init_completion(&worker->done);
		worker->task = kthread_create(shmem_worker, worker, "shmem-gate/%u", cpu);
		if (IS_ERR(worker->task))
			return fail(test, __LINE__, PTR_ERR(worker->task));
		kthread_bind(worker->task, cpu);
		wake_up_process(worker->task);
	}
	return 0;
}

static void launch(struct shmem_worker *worker, enum worker_action action)
{
	worker->action = action;
	reinit_completion(&worker->entered);
	reinit_completion(&worker->done);
	complete(&worker->go);
}

static int finish_operation(struct shmem_worker *worker)
{
	if (!wait_for_completion_timeout(&worker->done, GATE_WAIT))
		return fail(worker->test, __LINE__, -ETIMEDOUT);
	if (worker->result)
		return fail(worker->test, __LINE__, worker->result);
	return 0;
}

static int shared_offset(struct shmem_case *test)
{
	struct folio *folio;
	struct page *page;
	unsigned long *first, *second;
	unsigned long value;
	unsigned int round;
	loff_t position = 0;
	int result;

	if (resize(test, 0) || resize(test, PAGE_SIZE) || account(test, PAGE_SIZE, 0))
		return -EINVAL;
	for (round = 0; round < 128; round++) {
		launch(&test->workers[0], INCREMENT);
		launch(&test->workers[1], INCREMENT);
		if (finish_operation(&test->workers[0]) || finish_operation(&test->workers[1]))
			return -EINVAL;
		if (test->workers[0].pfn != test->workers[1].pfn)
			return fail(test, __LINE__, -EINVAL);
		test->report->increments += 2;
	}
	result = kernel_read(test->peer, &value, sizeof(value), &position);
	if (result != sizeof(value) || value != 256 || account(test, PAGE_SIZE, 1))
		return fail(test, __LINE__, result);
	folio = shmem_read_folio(test->inode->i_mapping, 0);
	if (IS_ERR(folio))
		return fail(test, __LINE__, PTR_ERR(folio));
	page = folio_page(folio, 0);
	first = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
	second = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
	if (!first || !second || first == second || *first != value || *second != value)
		return fail(test, __LINE__, -EINVAL);
	folio_lock(folio);
	*first = 0x713579ab;
	folio_mark_dirty(folio);
	folio_unlock(folio);
	position = 0;
	result = kernel_read(test->peer, &value, sizeof(value), &position);
	if (result != sizeof(value) || value != *second || value != 0x713579ab)
		return fail(test, __LINE__, result);
	/* These are pinned kernel aliases, not user VMAs: truncate detaches the
	 * cache entry but must not free its still-mapped physical backing.
	 */
	if (resize(test, 0) || account(test, 0, 0) || folio_mapping(folio) ||
	    *first != value || *second != value)
		return fail(test, __LINE__, -EINVAL);
	vunmap(first);
	vunmap(second);
	vm_unmap_aliases();
	test->report->sharing_checks += 3;
	return reuse_page(test, folio);
}

static int locked_mutation(struct shmem_case *test, enum worker_action action)
{
	struct shmem_worker *worker = &test->workers[!test->report->cpu];
	struct folio *folio;
	unsigned long deadline = jiffies + GATE_WAIT;
	int result;

	if (resize(test, 0) || fill(test, 3 * PAGE_SIZE, 0x5a))
		return -EINVAL;
	lru_add_drain_all();
	result = shmem_get_folio(test->inode, 1, 0, &folio, SGP_CACHE);
	if (result)
		return fail(test, __LINE__, result);
	launch(worker, action);
	if (!wait_for_completion_timeout(&worker->entered, GATE_WAIT))
		return fail(test, __LINE__, -ETIMEDOUT);
	while (!(READ_ONCE(worker->task->__state) & TASK_UNINTERRUPTIBLE)) {
		if (completion_done(&worker->done) || time_after(jiffies, deadline))
			return fail(test, __LINE__, -ETIMEDOUT);
		usleep_range(1000, 2000);
	}
	if (completion_done(&worker->done) || folio_mapping(folio) != test->file->f_mapping)
		return fail(test, __LINE__, -EINVAL);
	folio_unlock(folio);
	if (finish_operation(worker) ||
	    account(test, action == TRUNCATE ? 0 : 3 * PAGE_SIZE, action == TRUNCATE ? 0 : 2) ||
	    folio_mapping(folio) || memchr_inv(folio_address(folio), 0x5a, PAGE_SIZE))
		return fail(test, __LINE__, -EINVAL);
	test->report->locked_waits++;
	return reuse_page(test, folio);
}

static int concurrent_io(struct shmem_case *test)
{
	unsigned int round;
	int result;

	if (resize(test, 0) || resize(test, 3 * PAGE_SIZE))
		return -EINVAL;
	for (round = 0; round < RACE_ROUNDS; round++) {
		launch(&test->workers[0], READ_PAGE);
		launch(&test->workers[1], WRITE_PAGE);
		if (round & 1) {
			result = vfs_fallocate(test->file,
				FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, PAGE_SIZE, PAGE_SIZE);
		} else {
			/* Reserved objects never write beyond their reservation. */
			result = resize(test, test->report->noreserve ? 0 : 2 * PAGE_SIZE);
			if (!result)
				result = resize(test, 3 * PAGE_SIZE);
		}
		if (result || finish_operation(&test->workers[0]) ||
		    finish_operation(&test->workers[1]))
			return fail(test, __LINE__, result);
		test->report->race_rounds++;
	}
	if (resize(test, 0) || account(test, 0, 0) || resize(test, 3 * PAGE_SIZE))
		return -EINVAL;
	memset(test->expected, 0, BUFFER_BYTES);
	return read_expected(test, 3 * PAGE_SIZE) ?: account(test, 3 * PAGE_SIZE, 0);
}

static int allocation_rollback(struct shmem_case *test)
{
	struct file *quota;
	int result;

	/* Fallocate's growing/preallocation path is for incremental tmpfs
	 * accounting, not fixed-size, pre-reserved anonymous shmem objects.
	 */
	if (!test->report->noreserve)
		return 0;
	if (resize(test, 0))
		return -EINVAL;
	result = vfs_fallocate(test->file, FALLOC_FL_KEEP_SIZE, 2 * PAGE_SIZE, 2 * PAGE_SIZE);
	if (result || account(test, 0, 2) || resize(test, 6 * PAGE_SIZE))
		return fail(test, __LINE__, result);
	memset(test->expected, 0, BUFFER_BYTES);
	if (read_expected(test, BUFFER_BYTES) || account(test, BUFFER_BYTES, 2) ||
	    resize(test, 0) || fill(test, PAGE_SIZE, 0x5a))
		return -EINVAL;
	quota = shmem_file_setup_with_mnt(test->mnt, "quota-holder", 14 * PAGE_SIZE, VM_NORESERVE);
	if (IS_ERR(quota))
		return fail(test, __LINE__, PTR_ERR(quota));
	result = vfs_fallocate(quota, 0, 0, 14 * PAGE_SIZE);
	if (result || percpu_counter_sum(&test->sbinfo->used_blocks) != 15)
		return fail(test, __LINE__, result);
	/* One slot is free: this must allocate part of the request, then undo
	 * those new !uptodate folios on ENOSPC, preserving existing dirty data.
	 */
	result = vfs_fallocate(test->file, 0, PAGE_SIZE, 3 * PAGE_SIZE);
	if (result != -ENOSPC || test->inode->i_mapping->nrpages != 1 ||
	    SHMEM_I(test->inode)->alloced != 1 ||
	    percpu_counter_sum(&test->sbinfo->used_blocks) != 15 ||
	    vm_memory_committed() != test->committed + 15 ||
	    i_size_read(test->inode) != PAGE_SIZE || read_expected(test, PAGE_SIZE))
		return fail(test, __LINE__, result);
	__fput_sync(quota);
	if (account(test, PAGE_SIZE, 1))
		return -EINVAL;
	test->report->rollbacks++;
	return 0;
}

static int release_case(struct shmem_case *test)
{
	struct folio *folio;
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		struct shmem_worker *worker = &test->workers[cpu];

		WRITE_ONCE(worker->finish, true);
		complete(&worker->go);
		if (kthread_stop(worker->task))
			return fail(test, __LINE__, -EINVAL);
		kfree(worker->buffer);
	}
	if (resize(test, 0) || fill(test, PAGE_SIZE, 0x5a) || account(test, PAGE_SIZE, 1))
		return -EINVAL;
	folio = shmem_read_folio(test->inode->i_mapping, 0);
	if (IS_ERR(folio))
		return fail(test, __LINE__, PTR_ERR(folio));
	__fput_sync(test->peer);
	__fput_sync(test->file);
	/* No inode dereference after final close. Inode/block/commit charges
	 * disappear during eviction; the explicit folio pin still owns RAM.
	 */
	test->peer = NULL;
	test->file = NULL;
	test->inode = NULL;
	if (percpu_counter_sum(&test->sbinfo->used_blocks) ||
	    test->sbinfo->free_ispace != test->free_ispace ||
	    vm_memory_committed() != test->committed || folio_mapping(folio) ||
	    memchr_inv(folio_address(folio), 0x5a, PAGE_SIZE))
		return fail(test, __LINE__, -EINVAL);
	test->report->accounting_checks++;
	if (reuse_page(test, folio))
		return -EINVAL;
	rcu_barrier();
	mntput(test->mnt);
	rcu_barrier();
	return 0;
}

static int run_case(struct kobox_linux_shmem_report *report)
{
	struct shmem_case *test;
	int result;

	/* On failure preserve possibly live storage for immediate process exit. */
	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->report = report;
	test->expected = kmalloc(BUFFER_BYTES, GFP_KERNEL);
	test->read = kmalloc(BUFFER_BYTES, GFP_KERNEL);
	if (!test->expected || !test->read)
		return fail(test, __LINE__, -ENOMEM);
	report->phase = 1;
	result = create_file(test);
	if (result)
		return result;
	report->phase = 2;
	result = boundaries(test);
	if (result)
		return result;
	report->phase = 3;
	result = start_workers(test) ?: shared_offset(test);
	if (result)
		return result;
	report->phase = 4;
	result = locked_mutation(test, TRUNCATE) ?: locked_mutation(test, PUNCH);
	if (result)
		return result;
	report->phase = 5;
	result = concurrent_io(test);
	if (result)
		return result;
	report->phase = 6;
	result = allocation_rollback(test);
	if (result)
		return result;
	report->phase = 7;
	result = release_case(test);
	if (result)
		return result;
	kfree(test->expected);
	kfree(test->read);
	kfree(test);
	report->cases++;
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_shmem_verify(struct kobox_linux_shmem_report *report)
{
	cpumask_t saved;
	unsigned int cpu, noreserve;
	int result;

	if (!report || report->size != sizeof(*report) ||
	    system_state != SYSTEM_RUNNING || task_pid_nr(current) != 1 ||
	    num_online_cpus() != 2 || IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) ||
	    IS_ENABLED(CONFIG_SWAP))
		return -EINVAL;
	cpumask_copy(&saved, current->cpus_ptr);
	for_each_online_cpu(cpu) {
		report->cpu = cpu;
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
			return -EINVAL;
		for (noreserve = 0; noreserve < 2; noreserve++) {
			report->noreserve = noreserve;
			result = run_case(report);
			if (result)
				return result;
		}
	}
	report->warnings = kobox_linux_exception_warnings();
	if (report->warnings)
		return -EINVAL;
	report->phase = 8;
	return set_cpus_allowed_ptr(current, &saved);
}
