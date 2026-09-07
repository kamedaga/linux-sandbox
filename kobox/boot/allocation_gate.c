// SPDX-License-Identifier: GPL-2.0-only

#include "host.h"
#include "allocation_gate.h"
#include "pressure_gate.h"

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>

#if defined(CONFIG_FAILSLAB) && defined(CONFIG_FAIL_PAGE_ALLOC) && \
	defined(CONFIG_FAULT_INJECTION_DEBUG_FS)

#define TEST_PAGES 9
#define MAX_FAIL_NTH 128

enum allocation_case {
	MM_CREATE,
	FILE_SETUP,
	SHMEM_PAGE,
	VMAP_ALIAS,
	VMALLOC_PAGES,
	ALLOCATION_CASES,
};

struct injection_control {
	const char *name;
	const char *value;
	char saved[32];
	struct file *file;
};

static int configure_injection(struct injection_control *controls, size_t count,
			       struct vfsmount *mnt, bool restore)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		struct injection_control *control = &controls[i];
		loff_t position = 0;
		ssize_t result;
		const char *value;

		if (!restore) {
			control->file = file_open_root_mnt(mnt, control->name,
							 O_RDWR, 0);
			if (IS_ERR(control->file))
				return PTR_ERR(control->file);
			/* These are process-local syscall input buffers in the
			 * hosted address space. Use native uaccess and debugfs file
			 * operations, not writes to allocator-private globals.
			 */
			result = vfs_read(control->file, (char __user *)control->saved,
					  sizeof(control->saved) - 1, &position);
			if (result <= 0)
				return result ?: -EINVAL;
			control->saved[result] = '\0';
		}
		position = 0;
		value = restore ? control->saved : control->value;
		result = vfs_write(control->file, (const char __user *)value,
				   strlen(value), &position);
		if (result != strlen(value))
			return result < 0 ? result : -EINVAL;
		if (restore)
			__fput_sync(control->file);
	}
	return 0;
}

struct vfsmount *kobox_linux_allocation_mount(void)
{
	struct file_system_type *type = get_fs_type("tmpfs");
	struct fs_context *fc;
	struct vfsmount *mnt;
	int result;

	if (!type)
		return ERR_PTR(-ENOENT);
	fc = fs_context_for_mount(type, 0);
	put_filesystem(type);
	if (IS_ERR(fc))
		return ERR_CAST(fc);
	result = vfs_parse_fs_string(fc, "size", "16777216");
	if (!result)
		result = vfs_parse_fs_string(fc, "nr_inodes", "256");
	mnt = result ? ERR_PTR(result) : fc_mount(fc);
	put_fs_context(fc);
	return mnt;
}

static int allocation_trial(struct kobox_linux_pressure_report *report,
			    struct vfsmount *mnt, struct page **pins,
			    unsigned int action, unsigned int nth)
{
	struct shmem_sb_info *sb = mnt->mnt_sb->s_fs_info;
	unsigned long committed = vm_memory_committed();
	unsigned long ispace = sb->free_ispace;
	unsigned long mapped = vmalloc_nr_pages();
	struct file *file = NULL;
	struct mm_struct *mm = NULL;
	struct page *page = NULL;
	void *alias = NULL;
	unsigned int i, remaining;
	int result = 0;

	if (action == SHMEM_PAGE) {
		file = shmem_file_setup_with_mnt(mnt, "allocation-rollback",
						TEST_PAGES * PAGE_SIZE, 0);
		if (IS_ERR(file))
			return PTR_ERR(file);
	}
	/* The upstream per-task counter affects only this operation. Cleanup,
	 * observers, recovery and all other Linux tasks run without injection.
	 */
	WRITE_ONCE(current->fail_nth, nth);
	switch (action) {
	case MM_CREATE:
		mm = mm_alloc();
		if (!mm)
			result = -ENOMEM;
		break;
	case FILE_SETUP:
		file = shmem_file_setup_with_mnt(mnt, "allocation-rollback",
						TEST_PAGES * PAGE_SIZE, 0);
		if (IS_ERR(file))
			result = PTR_ERR(file);
		break;
	case SHMEM_PAGE:
		/* An index outside the root XArray slot forces its node path. */
		page = shmem_read_mapping_page(file->f_mapping, TEST_PAGES - 1);
		if (IS_ERR(page))
			result = PTR_ERR(page);
		break;
	case VMAP_ALIAS:
		alias = vmap(pins, TEST_PAGES, VM_MAP, PAGE_KERNEL);
		if (!alias)
			result = -ENOMEM;
		break;
	case VMALLOC_PAGES:
		alias = __vmalloc(TEST_PAGES * PAGE_SIZE,
				  GFP_KERNEL | __GFP_NOWARN);
		if (!alias)
			result = -ENOMEM;
		break;
	}
	remaining = READ_ONCE(current->fail_nth);
	WRITE_ONCE(current->fail_nth, 0);
	if (nth && !remaining)
		report->injected++;
	/* Upstream shmem maps new_inode() failure to ENOSPC, even with ample
	 * inode quota. Verify the rollback, not an invented errno contract.
	 */
	if (result && result != -ENOMEM &&
	    !(action == FILE_SETUP && result == -ENOSPC))
		return result;
	if (result && (!nth || remaining))
		return -EINVAL;
	if (mm) {
		if (!mm->pgd || mm->map_count || atomic_read(&mm->mm_users) != 1 ||
		    atomic_read(&mm->mm_count) != 1)
			return -EINVAL;
		mmput(mm);
	}
	if (action == SHMEM_PAGE) {
		unsigned long expected = result ? 0 : 1;

		if (file_count(file) != 1 || file->f_mapping->nrpages != expected ||
		    SHMEM_I(file_inode(file))->alloced != expected ||
		    SHMEM_I(file_inode(file))->swapped)
			return -EINVAL;
		if (!IS_ERR(page)) {
			if (memchr_inv(page_address(page), 0, PAGE_SIZE))
				return -EINVAL;
			put_page(page);
		}
	}
	if (action == VMAP_ALIAS) {
		if (alias) {
			for (i = 0; i < TEST_PAGES; i++) {
				if (vmalloc_to_page(alias + i * PAGE_SIZE) != pins[i] ||
				    memchr_inv(alias + i * PAGE_SIZE, i + 1, PAGE_SIZE))
					return -EINVAL;
			}
			vunmap(alias);
			if (find_vm_area(alias))
				return -EINVAL;
		}
		for (i = 0; i < TEST_PAGES; i++)
			if (page_count(pins[i]) != 1)
				return -EINVAL;
	} else if (action == VMALLOC_PAGES && alias) {
		memset(alias, 0x6d, TEST_PAGES * PAGE_SIZE);
		vfree(alias);
		if (find_vm_area(alias))
			return -EINVAL;
	}
	if (file && !IS_ERR(file))
		__fput_sync(file);
	lru_add_drain_all();
	rcu_barrier();
	if (sb->free_ispace != ispace || percpu_counter_sum(&sb->used_blocks) ||
	    vm_memory_committed() != committed || vmalloc_nr_pages() != mapped)
		return -EINVAL;
	if (result)
		report->rollbacks++;
	else
		report->recovered++;
	/* A successful operation that did not reach nth ends this sweep. */
	return nth && remaining ? 1 : 0;
}

int kobox_linux_with_allocation_failures(int (*run)(void *), void *argument)
{
	struct injection_control controls[] = {
		{ .name = "failslab/verbose", .value = "0" },
		{ .name = "fail_page_alloc/verbose", .value = "0" },
		{ .name = "failslab/ignore-gfp-wait", .value = "N" },
		{ .name = "fail_page_alloc/ignore-gfp-wait", .value = "N" },
		{ .name = "fail_page_alloc/ignore-gfp-highmem", .value = "N" },
		{ .name = "fail_page_alloc/min-order", .value = "0" },
	};
	struct file_system_type *type = get_fs_type("debugfs");
	struct vfsmount *debugfs;
	int result, restored;

	if (!type)
		return -ENOENT;
	debugfs = kern_mount(type);
	put_filesystem(type);
	if (IS_ERR(debugfs))
		return PTR_ERR(debugfs);
	result = configure_injection(controls, ARRAY_SIZE(controls), debugfs, false);
	if (result)
		return result;
	result = run(argument);
	restored = configure_injection(controls, ARRAY_SIZE(controls), debugfs, true);
	kern_unmount(debugfs);
	return result ?: restored;
}

static int run_allocations(void *argument)
{
	struct kobox_linux_pressure_report *report = argument;
	struct vfsmount *mnt = kobox_linux_allocation_mount();
	struct page *pins[TEST_PAGES];
	unsigned int action, nth, i, previous;
	int result;

	if (IS_ERR(mnt))
		return PTR_ERR(mnt);
	for (i = 0; i < TEST_PAGES; i++) {
		pins[i] = alloc_page(GFP_KERNEL);
		if (!pins[i])
			return -ENOMEM;
		memset(page_address(pins[i]), i + 1, PAGE_SIZE);
	}
	report->phase = 2;
	for (action = 0; action < ALLOCATION_CASES; action++) {
		report->allocation_case = action;
		previous = report->rollbacks;
		for (nth = 1; nth <= MAX_FAIL_NTH; nth++) {
			report->fail_nth = nth;
			result = allocation_trial(report, mnt, pins, action, nth);
			if (result < 0)
				return result;
			if (result > 0)
				break;
			/* Recovery must work immediately after every failure. */
			result = allocation_trial(report, mnt, pins, action, 0);
			if (result)
				return result;
		}
		if (nth > MAX_FAIL_NTH || report->rollbacks == previous)
			return -EINVAL;
		report->sweeps++;
	}
	report->phase = 3;
	for (i = 0; i < TEST_PAGES; i++)
		__free_page(pins[i]);
	mntput(mnt);
	rcu_barrier();
	return 0;
}
#else
int kobox_linux_with_allocation_failures(int (*run)(void *), void *argument)
{
	return -EOPNOTSUPP;
}

struct vfsmount *kobox_linux_allocation_mount(void)
{
	return ERR_PTR(-EOPNOTSUPP);
}
#endif

__attribute__((visibility("default")))
int kobox_linux_allocation_verify(struct kobox_linux_pressure_report *report)
{
	if (!report || report->size != sizeof(*report) ||
	    system_state != SYSTEM_RUNNING || num_online_cpus() != 2)
		return -EINVAL;
	report->phase = 1;
#if defined(CONFIG_FAILSLAB) && defined(CONFIG_FAIL_PAGE_ALLOC) && \
	defined(CONFIG_FAULT_INJECTION_DEBUG_FS)
	report->result = kobox_linux_with_allocation_failures(run_allocations, report);
#else
	report->result = -EOPNOTSUPP;
#endif
	report->warnings = kobox_linux_exception_warnings();
	return report->result ?: (report->warnings ? -EINVAL : 0);
}
