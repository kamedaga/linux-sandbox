// SPDX-License-Identifier: GPL-2.0-only

#include "lifetime_test.h"
#include "../mm/port.h"

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/iosys-map.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include "../../mm/slab.h"

#define BUFFER_PAGES 3
#define BUFFER_SIZE (BUFFER_PAGES * PAGE_SIZE)
#define REUSE_BATCH 4096
#define GATE_WAIT (5 * HZ)

enum gem_action {
	GEM_MAP,
	GEM_MAP_DENIED,
	GEM_MAP_PRIVATE,
	GEM_MAP_FAILURE,
	GEM_ACCESS,
	GEM_ACCESS_FAILURE,
	GEM_UNMAP,
};

struct gem_worker {
	const struct kobox_linux_vm_test *host;
	struct kobox_gem_lifetime_report *report;
	struct kobox_vm_space *space;
	struct task_struct *task;
	struct file *file;
	struct completion ready, go, done;
	struct completion *fault_received, *fault_release;
	enum gem_action action;
	u64 offset, sequence, value, observed;
	unsigned long address, length;
	unsigned int write, signal;
	unsigned int fail_nth, remaining;
	unsigned int fault_retries;
	bool finish;
	int result;
};

/* A driver-like test consumer owns its reference until native RCU work ends.
 * Queue policy is local to this test object, never a workqueue replacement.
 */
struct gem_deferred {
	struct work_struct producer;
	struct rcu_work retire;
	struct completion entered, release;
	spinlock_t lock;
	struct drm_gem_object *object;
	unsigned int rejected, callbacks;
	bool stopping;
	int result;
};

struct gem_case {
	struct kobox_gem_lifetime_report *report;
	const struct kobox_linux_vm_test *host;
	void (*drain)(void);
	enum kobox_gem_final_owner final_owner;
	struct device *parent;
	struct drm_device *dev;
	struct vfsmount *mnt;
	struct file *files[2];
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *lookup;
	struct iosys_map maps[2];
	struct gem_worker workers[2];
	u32 handles[3];
	unsigned long object_address, pfns[BUFFER_PAGES];
	struct kmem_cache *object_cache;
	gfp_t page_gfp;
	void **probes;
	bool cleanup;
	bool terminate_client, client_dead;
	bool pressure_alias;
	struct gem_deferred deferred;
};

DEFINE_DRM_GEM_FOPS(gem_fops);

/* Only the test device descriptor is ours; all GEM functions are upstream. */
static const struct drm_driver gem_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.fops = &gem_fops,
	.name = "kobox-gem-lifetime",
	.desc = "Upstream shmem lifetime test",
	.major = 1,
};

static int fail(struct gem_case *test, unsigned int line, int result)
{
	test->report->line = line;
	test->report->result = result ?: -EINVAL;
	return test->report->result;
}

static int probe_kernel_alias(const void *address, u64 *value)
{
	pagefault_disable();
	__get_kernel_nofault(value, address, u64, fault);
	pagefault_enable();
	return 0;
fault:
	pagefault_enable();
	return -EFAULT;
}

static int create_node(struct gem_case *test)
{
	struct inode *parent = d_inode(test->mnt->mnt_root);
	struct qstr name = QSTR_INIT("render", 6);
	struct dentry *dentry;
	int result;

	result = mnt_want_write(test->mnt);
	if (result)
		return result;
	inode_lock(parent);
	dentry = lookup_one(mnt_idmap(test->mnt), &name, test->mnt->mnt_root);
	if (IS_ERR(dentry)) {
		result = PTR_ERR(dentry);
	} else {
		result = vfs_mknod(mnt_idmap(test->mnt), parent, dentry,
			S_IFCHR | 0600, MKDEV(DRM_MAJOR, test->dev->render->index));
		dput(dentry);
	}
	inode_unlock(parent);
	mnt_drop_write(test->mnt);
	return result;
}

static int create_device(struct gem_case *test)
{
	struct file_system_type *type;
	unsigned int index;
	int result;

	test->parent = root_device_register("kobox-gem-lifetime");
	if (IS_ERR(test->parent))
		return fail(test, __LINE__, PTR_ERR(test->parent));
	test->dev = drm_dev_alloc(&gem_driver, test->parent);
	if (IS_ERR(test->dev))
		return fail(test, __LINE__, PTR_ERR(test->dev));
	result = drm_dev_register(test->dev, 0);
	if (result)
		return fail(test, __LINE__, result);
	type = get_fs_type("tmpfs");
	if (!type)
		return fail(test, __LINE__, -ENODEV);
	test->mnt = kern_mount(type);
	module_put(type->owner);
	if (IS_ERR(test->mnt))
		return fail(test, __LINE__, PTR_ERR(test->mnt));
	result = create_node(test);
	if (result)
		return fail(test, __LINE__, result);
	for (index = 0; index < 2; index++) {
		struct file *file;

		file = file_open_root_mnt(test->mnt, "render", O_RDWR, 0);
		if (IS_ERR(file))
			return fail(test, __LINE__, PTR_ERR(file));
		test->files[index] = file;
		if (file->f_op != &gem_fops || !file->private_data)
			return fail(test, __LINE__, -EINVAL);
		test->report->files++;
	}
	if (test->files[0]->private_data == test->files[1]->private_data ||
	    atomic_read(&test->dev->open_count) != 2)
		return fail(test, __LINE__, -EINVAL);
	return 0;
}

static int create_buffer(struct gem_case *test)
{
	struct drm_gem_object *object;
	unsigned int index;
	int result;

	test->shmem = drm_gem_shmem_create(test->dev, BUFFER_SIZE);
	if (IS_ERR(test->shmem))
		return fail(test, __LINE__, PTR_ERR(test->shmem));
	object = &test->shmem->base;
	test->object_address = (unsigned long)test->shmem;
	test->object_cache = virt_to_slab(test->shmem)->slab_cache;
	test->page_gfp = mapping_gfp_mask(object->filp->f_mapping);
	for (index = 0; index < 3; index++) {
		struct drm_file *file = test->files[index == 2]->private_data;

		result = drm_gem_handle_create(file, object, &test->handles[index]);
		if (result)
			return fail(test, __LINE__, result);
		test->report->handles++;
	}
	test->lookup = drm_gem_object_lookup(test->files[0]->private_data,
					    test->handles[0]);
	if (test->lookup != object || object->handle_count != 3 ||
	    kref_read(&object->refcount) != 3)
		return fail(test, __LINE__, -EINVAL);
	for (index = 0; index < 2; index++) {
		result = drm_gem_shmem_pin(test->shmem);
		if (result)
			return fail(test, __LINE__, result);
		test->report->pins++;
		result = drm_gem_vmap(object, &test->maps[index]);
		if (result)
			return fail(test, __LINE__, result);
		test->report->vmaps++;
	}
	if (!iosys_map_is_equal(&test->maps[0], &test->maps[1]) ||
	    test->maps[0].is_iomem || refcount_read(&test->shmem->pages_pin_count) != 3 ||
	    refcount_read(&test->shmem->vmap_use_count) != 2 ||
	    refcount_read(&test->shmem->pages_use_count) != 1)
		return fail(test, __LINE__, -EINVAL);
	for (index = 0; index < BUFFER_PAGES; index++) {
		struct page *page = test->shmem->pages[index];
		unsigned int previous;

		test->pfns[index] = page_to_pfn(page);
		for (previous = 0; previous < index; previous++)
			if (test->pfns[previous] == test->pfns[index])
				return fail(test, __LINE__, -EINVAL);
		if (vmalloc_to_page(test->maps[0].vaddr + index * PAGE_SIZE) != page)
			return fail(test, __LINE__, -EINVAL);
	}
	return 0;
}

static int access_client(struct gem_worker *worker)
{
	struct kobox_vm_space *space = worker->space;
	const struct kobox_linux_vm_host_operations *ops = space->operations;
	struct kobox_linux_vm_event event;
	long waited;
#ifdef CONFIG_FAULT_INJECTION
	bool first_fault = true;
#endif
	int result;

	worker->signal = 0;
	result = worker->host->probe(space->host_space, worker->address,
		worker->write, worker->value, worker->sequence);
	if (result)
		return result;
	for (;;) {
		waited = wait_event_timeout(space->events,
			(result = ops->event(space->host_space, &event)) != -EAGAIN,
			GATE_WAIT);
		if (!waited || result || event.error)
			return !waited ? -ETIMEDOUT : result ?: event.error;
		if (event.sequence != worker->sequence + 1 ||
		    current->mm != space->mm || current->active_mm != space->mm ||
		    current != raw_cpu_read(current_task))
			return -EINVAL;
		worker->sequence = event.sequence;
		if (event.kind == KOBOX_VM_EVENT_STOP) {
			worker->observed = event.value;
			worker->report->accesses++;
			return 0;
		}
		if (event.kind != KOBOX_VM_EVENT_FAULT ||
		    event.fault.address != worker->address)
			return -EFAULT;
		worker->report->faults++;
		if (worker->fault_received) {
			complete(worker->fault_received);
			if (!wait_for_completion_timeout(worker->fault_release,
						 GATE_WAIT))
				return -ETIMEDOUT;
			worker->fault_received = NULL;
		}
#ifdef CONFIG_FAULT_INJECTION
		if (worker->action == GEM_ACCESS_FAILURE && first_fault)
			WRITE_ONCE(current->fail_nth, worker->fail_nth);
#endif
		result = kobox_vm_resolve_fault(space, &event.fault);
#ifdef CONFIG_FAULT_INJECTION
		if (worker->action == GEM_ACCESS_FAILURE && first_fault) {
			worker->remaining = READ_ONCE(current->fail_nth);
			WRITE_ONCE(current->fail_nth, 0);
		}
		first_fault = false;
#endif
		if (result == -EAGAIN && worker->action == GEM_ACCESS_FAILURE)
			worker->fault_retries++;
		if (result && result != -EAGAIN)
			return result;
		if (event.fault.signal) {
			worker->signal = event.fault.signal;
			return 0;
		}
		result = ops->resume(space->host_space, worker->sequence);
		if (result)
			return result;
	}
}

static int worker_main(void *argument)
{
	struct gem_worker *worker = argument;
	unsigned long address, flags;
	struct vm_area_struct *vma;

	kthread_use_mm(worker->space->mm);
	complete(&worker->ready);
	for (;;) {
		wait_for_completion(&worker->go);
		if (worker->finish)
			break;
		switch (worker->action) {
		case GEM_MAP:
		case GEM_MAP_DENIED:
		case GEM_MAP_PRIVATE:
		case GEM_MAP_FAILURE:
			flags = MAP_FIXED | (worker->action == GEM_MAP_PRIVATE ?
					     MAP_PRIVATE : MAP_SHARED);
#ifdef CONFIG_FAULT_INJECTION
			if (worker->action == GEM_MAP_FAILURE)
				WRITE_ONCE(current->fail_nth, worker->fail_nth);
#endif
			address = vm_mmap(worker->file, worker->address,
				worker->length, PROT_READ | PROT_WRITE, flags,
				worker->offset);
#ifdef CONFIG_FAULT_INJECTION
			if (worker->action == GEM_MAP_FAILURE) {
				worker->remaining = READ_ONCE(current->fail_nth);
				WRITE_ONCE(current->fail_nth, 0);
			}
#endif
			worker->result = (long)address;
			if (worker->action != GEM_MAP && worker->action != GEM_MAP_FAILURE)
				break;
			if (address != worker->address)
				break;
			mmap_read_lock(current->mm);
			vma = find_vma(current->mm, address);
			worker->result = vma && vma->vm_start == address &&
				vma->vm_ops == &drm_gem_shmem_vm_ops &&
				vma->vm_flags & VM_PFNMAP ? 0 : -EINVAL;
			mmap_read_unlock(current->mm);
			break;
		case GEM_ACCESS:
		case GEM_ACCESS_FAILURE:
			worker->result = access_client(worker);
			break;
		case GEM_UNMAP:
			worker->result = vm_munmap(worker->address, worker->length);
			break;
		}
		complete(&worker->done);
	}
	kthread_unuse_mm(worker->space->mm);
	return 0;
}

static int run(struct gem_worker *worker, enum gem_action action)
{
	worker->action = action;
	reinit_completion(&worker->done);
	complete(&worker->go);
	if (!wait_for_completion_timeout(&worker->done, GATE_WAIT))
		return -ETIMEDOUT;
	return worker->result;
}

static int start_clients(struct gem_case *test)
{
	unsigned int index;
	int result;

	for (index = 0; index < 2; index++) {
		struct gem_worker *worker = &test->workers[index];

		worker->host = test->host;
		worker->report = test->report;
		worker->file = test->files[index];
		worker->address = test->host->start;
		worker->length = BUFFER_SIZE;
		worker->offset = drm_vma_node_offset_addr(&test->shmem->base.vma_node);
		worker->space = kobox_vm_space_create(test->host->spaces[index],
			test->host->operations, test->host->start, test->host->length);
		if (IS_ERR(worker->space))
			return fail(test, __LINE__, PTR_ERR(worker->space));
		init_completion(&worker->ready);
		init_completion(&worker->go);
		init_completion(&worker->done);
		worker->task = kthread_create(worker_main, worker, "gem/%u", index);
		if (IS_ERR(worker->task))
			return fail(test, __LINE__, PTR_ERR(worker->task));
		kthread_bind(worker->task, index);
		wake_up_process(worker->task);
		if (!wait_for_completion_timeout(&worker->ready, GATE_WAIT))
			return fail(test, __LINE__, -ETIMEDOUT);
		/* Actual native mmap rollback, including its temporary object ref. */
		result = run(worker, GEM_MAP_PRIVATE);
		if (result != -EINVAL || worker->space->mm->map_count ||
		    kref_read(&test->shmem->base.refcount) != 3 + index)
			return fail(test, __LINE__, result);
		test->report->denied++;
		result = run(worker, GEM_MAP);
		if (result)
			return fail(test, __LINE__, result);
		test->report->mappings++;
	}
	if (test->workers[0].space->mm == test->workers[1].space->mm ||
	    kref_read(&test->shmem->base.refcount) != 5 ||
	    refcount_read(&test->shmem->pages_use_count) != 3)
		return fail(test, __LINE__, -EINVAL);
	return 0;
}

static int access_value(struct gem_case *test, unsigned int client,
			unsigned int page, bool write, u64 value)
{
	struct gem_worker *worker = &test->workers[client];
	int result;

	worker->address = test->host->start + page * PAGE_SIZE;
	worker->write = write;
	worker->value = value;
	result = run(worker, GEM_ACCESS);
	if (result || worker->signal || worker->observed != value)
		return fail(test, __LINE__, result ?: -EINVAL);
	if (READ_ONCE(*(u64 *)(test->maps[0].vaddr + page * PAGE_SIZE)) != value)
		return fail(test, __LINE__, -EINVAL);
	return 0;
}

static int share_content(struct gem_case *test)
{
	u64 *mapping = test->maps[0].vaddr;
	unsigned int index;

	WRITE_ONCE(*mapping, 0x231547698badcfe0ULL);
	for (index = 0; index < 2; index++)
		if (access_value(test, index, 0, false, *mapping))
			return -EINVAL;
	if (access_value(test, 0, 0, true, 0x43abcde019876521ULL) ||
	    access_value(test, 1, 0, false, *mapping) ||
	    access_value(test, 1, 0, true, 0xfedcba0987654321ULL) ||
	    access_value(test, 0, 0, false, *mapping))
		return -EINVAL;
	/* A real populated middle PTE must disappear at partial munmap. */
	return access_value(test, 0, 1, true, 0xc35a);
}

static int revoke_handles(struct gem_case *test)
{
	struct drm_gem_object *object = &test->shmem->base;
	unsigned int index;
	int result;

	for (index = 0; index < 3; index++) {
		struct drm_file *file = test->files[index == 2]->private_data;
		struct drm_gem_object *found;

		result = drm_gem_handle_delete(file, test->handles[index]);
		if (result)
			return fail(test, __LINE__, result);
		found = drm_gem_object_lookup(file, test->handles[index]);
		if (found) {
			drm_gem_object_put(found);
			return fail(test, __LINE__, -EINVAL);
		}
		if (!index) {
			struct gem_worker *worker = &test->workers[0];

			/* The other handle in this same file still grants mmap access. */
			worker->address = test->host->start + 4 * PAGE_SIZE;
			result = run(worker, GEM_MAP);
			if (result)
				return fail(test, __LINE__, result);
			test->report->mappings++;
			worker->write = 0;
			result = run(worker, GEM_ACCESS);
			if (result || worker->signal ||
			    worker->observed != 0xfedcba0987654321ULL)
				return fail(test, __LINE__, result ?: -EINVAL);
			result = run(worker, GEM_UNMAP);
			if (result || kref_read(&object->refcount) != 5 + test->pressure_alias)
				return fail(test, __LINE__, result ?: -EINVAL);
		}
	}
	for (index = 0; index < 2; index++) {
		struct gem_worker *worker = &test->workers[index];

		worker->address = test->host->start + 4 * PAGE_SIZE;
		result = run(worker, GEM_MAP_DENIED);
		if (result != -EACCES ||
		    worker->space->mm->map_count != 1 + (!index && test->pressure_alias))
			return fail(test, __LINE__, result);
		test->report->denied++;
	}
	return 0;
}

static int release_kernel_owners(struct gem_case *test)
{
	struct drm_gem_object *object = &test->shmem->base;
	u64 value;
	unsigned int index;
	int result;

	/* Balance two external pins and both vmap users. Only VMAs keep pages. */
	for (index = 0; index < 2; index++) {
		drm_gem_shmem_unpin(test->shmem);
		drm_gem_vunmap(object, &test->maps[index]);
		if (!index && (refcount_read(&test->shmem->vmap_use_count) != 1 ||
		    READ_ONCE(*(u64 *)test->maps[1].vaddr) != 0xfedcba0987654321ULL))
			return fail(test, __LINE__, -EINVAL);
	}
	vm_unmap_aliases();
	if (object->handle_count || refcount_read(&test->shmem->pages_pin_count) ||
	    refcount_read(&test->shmem->vmap_use_count) || test->shmem->vaddr ||
	    refcount_read(&test->shmem->pages_use_count) != 2 ||
	    probe_kernel_alias(test->maps[0].vaddr, &value) != -EFAULT)
		return fail(test, __LINE__, -EINVAL);
	if (test->final_owner == KOBOX_GEM_FINAL_VMA) {
		drm_gem_object_put(test->lookup);
		test->lookup = NULL;
	}
	drm_gem_object_put(object);
	/* Borrow only while quiescent VMAs own the two remaining references. */
	if (kref_read(&object->refcount) != 2 + !!test->lookup)
		return fail(test, __LINE__, -EINVAL);
	for (index = 0; index < 2; index++) {
		result = filp_close(test->files[index], NULL);
		if (result)
			return fail(test, __LINE__, result);
		test->files[index] = NULL;
		test->workers[index].file = NULL;
	}
	test->drain();
	if (atomic_read(&test->dev->open_count) != 2)
		return fail(test, __LINE__, -EINVAL);
	return 0;
}

static int close_handles(struct gem_case *test)
{
	return revoke_handles(test) ?: release_kernel_owners(test);
}

static int access_after_close(struct gem_case *test)
{
	unsigned int index, faults = test->report->faults;
	int result;

	for (index = 0; index < 2; index++) {
		struct gem_worker *worker = &test->workers[index];

		if (index == 1 && test->client_dead)
			continue;
		worker->address = test->host->start + 2 * PAGE_SIZE;
		worker->write = !index;
		worker->value = 0x37a9e468b2c5d10fULL;
		result = run(worker, GEM_ACCESS);
		if (result || worker->signal || worker->observed != worker->value)
			return fail(test, __LINE__, result ?: -EINVAL);
	}
	/* Cleanup already resolved client 1's first page-2 fault after close. */
	if (test->report->faults < faults + (test->cleanup ? 1 : 2))
		return fail(test, __LINE__, -EINVAL);
	if (READ_ONCE(*(u64 *)page_address(pfn_to_page(test->pfns[2]))) !=
	    0x37a9e468b2c5d10fULL)
		return fail(test, __LINE__, -EINVAL);
	for (index = 0; index < 2; index++) {
		struct gem_worker *worker = &test->workers[index];

		if (index == 1 && test->client_dead)
			continue;
		worker->address = test->host->start;
		worker->write = 0;
		result = run(worker, GEM_ACCESS);
		if (result || worker->signal ||
		    worker->observed != 0xfedcba0987654321ULL)
			return fail(test, __LINE__, result ?: -EINVAL);
	}
	return 0;
}

static int check_live(struct gem_case *test)
{
	unsigned int index, which, count = 0;
	struct page *page = NULL;
	int result = 0;

	if (!kref_read(&test->shmem->base.refcount) ||
	    test->shmem->base.filp->f_mapping->nrpages != BUFFER_PAGES)
		return fail(test, __LINE__, -EINVAL);
	for (which = 0; which < BUFFER_PAGES; which++)
		if (page_ref_count(pfn_to_page(test->pfns[which])) <= 0)
			return fail(test, __LINE__, -EINVAL);
	/* No extra target references. The surviving VMA or object must own it. */
	for (index = 0; index < 64; index++) {
		void *object = kmem_cache_alloc(test->object_cache, GFP_KERNEL);

		if (!object) {
			result = -ENOMEM;
			break;
		}
		test->probes[count++] = object;
		if ((unsigned long)object == test->object_address) {
			result = -EINVAL;
			break;
		}
		page = alloc_page(test->page_gfp);
		if (!page) {
			result = -ENOMEM;
			break;
		}
		for (which = 0; which < BUFFER_PAGES; which++)
			if (page_to_pfn(page) == test->pfns[which])
				result = -EINVAL;
		__free_page(page);
		if (result)
			break;
	}
	for (index = 0; index < count; index++)
		kmem_cache_free(test->object_cache, test->probes[index]);
	if (result)
		return fail(test, __LINE__, result);
	test->report->live_checks++;
	return 0;
}

static int unmap_clients(struct gem_case *test)
{
	struct gem_worker *worker = &test->workers[0];
	unsigned int deferred_ref = !!test->deferred.object;
	unsigned int index;
	int result;

	worker->address = test->host->start + PAGE_SIZE;
	worker->length = PAGE_SIZE;
	result = run(worker, GEM_UNMAP);
	if (result || worker->space->mm->map_count != 2 ||
	    kref_read(&test->shmem->base.refcount) != 3 + !!test->lookup + deferred_ref ||
	    refcount_read(&test->shmem->pages_use_count) != 3)
		return fail(test, __LINE__, result ?: -EINVAL);
	worker->write = 0;
	/* Keep the surviving client executable in the death case, for the
	 * final stale-alias write test after all target PFNs have been reused.
	 */
	if (!test->cleanup) {
		result = run(worker, GEM_ACCESS);
		if (result || worker->signal != SIGSEGV)
			return fail(test, __LINE__, result ?: -EINVAL);
	}
	test->report->partial_unmaps++;
	/* No fault into the removed hole is allowed to resurrect its old PTE. */
	for (index = 0; index < 2; index++) {
		worker = &test->workers[index];
		worker->address = test->host->start;
		worker->length = BUFFER_SIZE;
		result = run(worker, GEM_UNMAP);
		if (result || worker->space->mm->map_count)
			return fail(test, __LINE__, result ?: -EINVAL);
		if (!index) {
			struct gem_worker *live = &test->workers[1];

			if (kref_read(&test->shmem->base.refcount) != 1 + !!test->lookup + deferred_ref ||
			    refcount_read(&test->shmem->pages_use_count) != 1)
				return fail(test, __LINE__, -EINVAL);
			result = check_live(test);
			if (result)
				return result;
			if (test->client_dead)
				continue;
			live->address = test->host->start + 2 * PAGE_SIZE;
			live->write = 0;
			result = run(live, GEM_ACCESS);
			if (result || live->signal ||
			    live->observed != 0x37a9e468b2c5d10fULL)
				return fail(test, __LINE__, result ?: -EINVAL);
		}
	}
	if (test->lookup) {
		if (kref_read(&test->lookup->refcount) != 1 + deferred_ref || test->shmem->pages ||
		    refcount_read(&test->shmem->pages_use_count))
			return fail(test, __LINE__, -EINVAL);
		result = check_live(test);
		if (result)
			return result;
		drm_gem_object_put(test->lookup);
		test->lookup = NULL;
	}
	/* This path no longer owns the object. A deferred consumer, if present,
	 * retains its own reference until finish_deferred() drains its RCU work.
	 */
	test->shmem = NULL;
	test->drain();
	if (atomic_read(&test->dev->open_count))
		return fail(test, __LINE__, -EINVAL);
	return 0;
}

static int reclaim_object(struct gem_case *test)
{
	cpumask_t saved = *current->cpus_ptr;
	unsigned int cpu, index, count = 0;
	bool seen = false;
	int result = 0;

	for_each_online_cpu(cpu) {
		result = set_cpus_allowed_ptr(current, cpumask_of(cpu));
		if (result)
			break;
		for (index = 0; index < REUSE_BATCH && !seen; index++) {
			void *object = kmem_cache_alloc(test->object_cache, GFP_KERNEL);

			if (!object) {
				result = -ENOMEM;
				break;
			}
			test->probes[count++] = object;
			seen = (unsigned long)object == test->object_address;
		}
		if (result)
			break;
	}
	for (index = 0; index < count; index++)
		kmem_cache_free(test->object_cache, test->probes[index]);
	if (set_cpus_allowed_ptr(current, &saved))
		result = -EINVAL;
	if (result || !seen)
		return fail(test, __LINE__, result ?: -ENOENT);
	test->report->object_reclaims++;
	return 0;
}

static int reclaim_pages(struct gem_case *test)
{
	cpumask_t saved = *current->cpus_ptr;
	unsigned int cpu, index, which, count = 0, seen = 0;
	int result = 0;

	test->drain();
	for_each_online_cpu(cpu) {
		result = set_cpus_allowed_ptr(current, cpumask_of(cpu));
		if (result)
			break;
		for (index = 0; index < REUSE_BATCH && seen != GENMASK(2, 0); index++) {
			struct page *page = alloc_page(test->page_gfp);

			if (!page) {
				result = -ENOMEM;
				break;
			}
			test->probes[count++] = page;
			for (which = 0; which < BUFFER_PAGES; which++)
				if (page_to_pfn(page) == test->pfns[which]) {
					seen |= BIT(which);
					memset(page_address(page), 0xa7, PAGE_SIZE);
				}
		}
		if (result)
			break;
	}
	if (!result && seen == GENMASK(2, 0)) {
		struct gem_worker *worker = &test->workers[test->client_dead ? 0 : 1];

		if (test->cleanup) {
			struct gem_worker *alias = &test->workers[0];

			alias->address = test->host->start + 7 * PAGE_SIZE;
			alias->write = 1;
			alias->value = 0xabadcafe;
			result = run(alias, GEM_ACCESS);
			if (result || alias->signal != SIGSEGV)
				result = result ?: -EINVAL;
			else
				test->report->revoked++;
		}
		/* A denied instruction remains fault-stopped. Only the other live
		 * client may perform an additional independent stale-alias probe.
		 */
		if (!result && !test->client_dead) {
			worker->address = test->host->start + 2 * PAGE_SIZE;
			worker->write = 0;
			result = run(worker, GEM_ACCESS);
			if (result || worker->signal != SIGSEGV)
				result = result ?: -EINVAL;
			else
				test->report->revoked++;
		}
	}
	for (index = 0; index < count; index++)
		__free_page(test->probes[index]);
	if (set_cpus_allowed_ptr(current, &saved))
		result = -EINVAL;
	if (result || seen != GENMASK(2, 0))
		return fail(test, __LINE__, result ?: -ENOENT);
	test->report->page_reclaims += BUFFER_PAGES;
	return 0;
}

static int stop_clients(struct gem_case *test)
{
	unsigned int index;
	int result;

	for (index = 0; index < 2; index++) {
		struct gem_worker *worker = &test->workers[index];

		/* kthread_stop owns task lifetime until exit; wake its completion. */
		get_task_struct(worker->task);
		worker->finish = true;
		complete(&worker->go);
		result = kthread_stop(worker->task);
		put_task_struct(worker->task);
		if (result)
			return fail(test, __LINE__, result);
		worker->task = NULL;
		result = kobox_vm_space_destroy(worker->space);
		if (result)
			return fail(test, __LINE__, result);
		worker->space = NULL;
	}
	test->drain();
	return 0;
}

#ifdef CONFIG_FAULT_INJECTION
#define FAILURE_PAGES 9
#define FAILURE_LIMIT 128

enum failure_stage {
	FAIL_CREATE,
	FAIL_HANDLE,
	FAIL_PIN,
	FAIL_VMAP,
	FAIL_MMAP,
	FAIL_FAULT,
	FAILURE_STAGES,
};

static int balanced_backing(struct gem_case *test,
			    struct drm_gem_shmem_object *shmem)
{
	struct address_space *mapping = shmem->base.filp->f_mapping;
	struct folio *folio;
	unsigned int i, pages = 0;

	test->drain();
	if (!(!shmem->pages && !shmem->vaddr &&
		!refcount_read(&shmem->pages_pin_count) &&
		!refcount_read(&shmem->pages_use_count) &&
		!refcount_read(&shmem->vmap_use_count) &&
		!mapping_unevictable(mapping) && file_count(shmem->base.filp) == 1))
		return fail(test, __LINE__, -EINVAL);
	for (i = 0; i < FAILURE_PAGES; i++) {
		folio = filemap_get_folio(mapping, i);
		if (IS_ERR(folio)) {
			if (!(i && PTR_ERR(folio) == -ENOENT))
				return fail(test, __LINE__, -EINVAL);
			continue;
		}
		/* The page cache and this observer are the only references. Partial
		 * backing from a failed get_pages remains legitimate shmem data.
		 */
		if (!(folio_ref_count(folio) == 2))
			return fail(test, __LINE__, -EINVAL);
		if (!i)
			if (!(!memchr_inv(folio_address(folio), 0x5a, PAGE_SIZE)))
				return fail(test, __LINE__, -EINVAL);
		folio_put(folio);
		pages++;
	}
	if (!(mapping->nrpages == pages &&
		SHMEM_I(mapping->host)->alloced == pages &&
		!SHMEM_I(mapping->host)->swapped))
		return fail(test, __LINE__, -EINVAL);
	return 0;
}

static int failure_trial(struct gem_case *test,
	const struct kobox_gem_failure_services *services,
	unsigned int stage, unsigned int nth, unsigned int cpu)
{
	struct kobox_gem_lifetime_report *report = test->report;
	struct shmem_sb_info *sb = services->mount->mnt_sb->s_fs_info;
	struct gem_worker *worker = &test->workers[cpu];
	struct drm_file *file = test->files[cpu]->private_data;
	struct drm_gem_shmem_object *shmem = NULL;
	unsigned long committed, ispace, vmalloc_pages;
	unsigned long address = worker->address, offset = worker->offset;
	long file_refs = file_count(worker->file);
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	struct page *page;
	unsigned int remaining = 0, attempt, faults;
	u32 handle = 0;
	int result;

	test->drain();
	committed = vm_memory_committed();
	ispace = sb->free_ispace;
	vmalloc_pages = services->vmalloc_pages();
	if (stage != FAIL_CREATE) {
		shmem = drm_gem_shmem_create_with_mnt(test->dev,
				FAILURE_PAGES * PAGE_SIZE, services->mount);
		if (IS_ERR(shmem))
			return fail(test, __LINE__, PTR_ERR(shmem));
		page = shmem_read_mapping_page(shmem->base.filp->f_mapping, 0);
		if (IS_ERR(page))
			return fail(test, __LINE__, PTR_ERR(page));
		memset(page_address(page), 0x5a, PAGE_SIZE);
		set_page_dirty(page);
		put_page(page);
	}
	if (stage == FAIL_MMAP) {
		result = drm_gem_handle_create(file, &shmem->base, &handle);
		if (result)
			return fail(test, __LINE__, result);
		worker->offset = drm_vma_node_offset_addr(&shmem->base.vma_node);
		worker->address = test->host->start + 4 * PAGE_SIZE;
	}
	/* The second attempt is on the same object after rollback, with no
	 * injection. Object creation naturally retries without an object.
	 */
	for (attempt = 0; attempt < 2; attempt++) {
		unsigned int inject = attempt ? 0 : nth;

		result = 0;
		if (stage != FAIL_MMAP)
			WRITE_ONCE(current->fail_nth, inject);
		switch (stage) {
		case FAIL_CREATE:
			shmem = drm_gem_shmem_create_with_mnt(test->dev,
					FAILURE_PAGES * PAGE_SIZE, services->mount);
			if (IS_ERR(shmem))
				result = PTR_ERR(shmem);
			break;
		case FAIL_HANDLE:
			result = drm_gem_handle_create(file, &shmem->base, &handle);
			break;
		case FAIL_PIN:
			result = drm_gem_shmem_pin(shmem);
			break;
		case FAIL_VMAP:
			result = drm_gem_vmap(&shmem->base, &map);
			break;
		case FAIL_MMAP:
			worker->fail_nth = inject;
			result = run(worker, GEM_MAP_FAILURE);
			break;
		}
		if (!attempt)
			remaining = stage == FAIL_MMAP ? worker->remaining :
				READ_ONCE(current->fail_nth);
		WRITE_ONCE(current->fail_nth, 0);
		if (inject && !remaining)
			report->injected++;
		if (result) {
			if (!(inject && !remaining && (result == -ENOMEM ||
				(stage == FAIL_CREATE && result == -ENOSPC))))
				return fail(test, __LINE__, -EINVAL);
			report->rollbacks++;
		} else {
			report->recoveries++;
			if (stage == FAIL_CREATE) {
				drm_gem_object_put(&shmem->base);
				shmem = NULL;
			} else if (stage == FAIL_HANDLE) {
				if (!(!drm_gem_handle_delete(file, handle)))
					return fail(test, __LINE__, -EINVAL);
				handle = 0;
			} else if (stage == FAIL_PIN) {
				drm_gem_shmem_unpin(shmem);
			} else if (stage == FAIL_VMAP) {
				if (!(!memchr_inv(map.vaddr, 0x5a, PAGE_SIZE)))
					return fail(test, __LINE__, -EINVAL);
				drm_gem_vunmap(&shmem->base, &map);
			} else if (stage == FAIL_MMAP) {
				faults = report->faults;
				worker->write = 0;
				if (!(!run(worker, GEM_ACCESS) && !worker->signal &&
					worker->observed == 0x5a5a5a5a5a5a5a5aULL &&
					report->faults > faults))
					return fail(test, __LINE__, -EINVAL);
				if (!(!run(worker, GEM_UNMAP)))
					return fail(test, __LINE__, -EINVAL);
			}
		}
		if (stage != FAIL_CREATE) {
			if (!(!balanced_backing(test, shmem)))
				return fail(test, __LINE__, -EINVAL);
			if (!(shmem->base.handle_count == (stage == FAIL_MMAP) &&
				kref_read(&shmem->base.refcount) == 1 + (stage == FAIL_MMAP)))
				return fail(test, __LINE__, -EINVAL);
			if (stage == FAIL_HANDLE)
				if (!(!drm_vma_node_is_allowed(&shmem->base.vma_node, file)))
					return fail(test, __LINE__, -EINVAL);
		}
		if (stage == FAIL_MMAP)
			if (!(worker->space->mm->map_count == 1 &&
				file_count(worker->file) == file_refs))
				return fail(test, __LINE__, -EINVAL);
		if (remaining)
			break;
	}
	if (stage == FAIL_MMAP) {
		if (!(!drm_gem_handle_delete(file, handle)))
			return fail(test, __LINE__, -EINVAL);
		worker->address = address;
		worker->offset = offset;
		worker->fail_nth = 0;
	}
	if (shmem && !IS_ERR(shmem))
		drm_gem_object_put(&shmem->base);
	test->drain();
	if (!(sb->free_ispace == ispace))
		return fail(test, __LINE__, -EINVAL);
	if (!(!percpu_counter_sum(&sb->used_blocks)))
		return fail(test, __LINE__, -EINVAL);
	if (!(vm_memory_committed() == committed))
		return fail(test, __LINE__, -EINVAL);
	if (!(services->vmalloc_pages() == vmalloc_pages))
		return fail(test, __LINE__, -EINVAL);
	return remaining ? 1 : 0;
}

static int failure_sweeps(struct gem_case *test,
			  const struct kobox_gem_failure_services *services)
{
	cpumask_t saved = *current->cpus_ptr;
	unsigned int cpu, stage, nth, previous;
	int result;

	for_each_online_cpu(cpu) {
		result = set_cpus_allowed_ptr(current, cpumask_of(cpu));
		if (result)
			return fail(test, __LINE__, result);
		for (stage = 0; stage < FAIL_FAULT; stage++) {
			test->report->allocation_stage = stage;
			previous = test->report->rollbacks;
			for (nth = 1; nth <= FAILURE_LIMIT; nth++) {
				test->report->fail_nth = nth;
				result = failure_trial(test, services, stage, nth, cpu);
				if (result < 0)
					return result;
				if (result)
					break;
			}
			if (!(nth <= FAILURE_LIMIT && test->report->rollbacks > previous))
				return fail(test, __LINE__, -EINVAL);
			test->report->sweeps++;
		}
	}
	return set_cpus_allowed_ptr(current, &saved);
}

static int fault_failure_sweeps(struct gem_case *test,
			       const struct kobox_gem_failure_services *services)
{
	struct shmem_sb_info *sb = services->mount->mnt_sb->s_fs_info;
	unsigned long committed = vm_memory_committed(), ispace = sb->free_ispace;
	unsigned int cpu, nth, injected;

	for (cpu = 0; cpu < 2; cpu++) {
		struct gem_worker *worker = &test->workers[cpu];
		struct drm_file *file = test->files[cpu]->private_data;
		struct drm_gem_shmem_object *shmem;
		unsigned long original_offset = worker->offset;
		struct page *page;
		u32 handle;

		worker->address = test->host->start;
		if (!(!run(worker, GEM_UNMAP)))
			return fail(test, __LINE__, -EINVAL);
		if (!(!worker->space->mm->map_count &&
			!mm_pgtables_bytes(worker->space->mm)))
			return fail(test, __LINE__, -EINVAL);
		shmem = drm_gem_shmem_create_with_mnt(test->dev,
				FAILURE_PAGES * PAGE_SIZE, services->mount);
		if (!(!IS_ERR(shmem)))
			return fail(test, __LINE__, -EINVAL);
		page = shmem_read_mapping_page(shmem->base.filp->f_mapping, 0);
		if (!(!IS_ERR(page)))
			return fail(test, __LINE__, -EINVAL);
		memset(page_address(page), 0x5a, PAGE_SIZE);
		set_page_dirty(page);
		put_page(page);
		if (!(!drm_gem_handle_create(file, &shmem->base, &handle)))
			return fail(test, __LINE__, -EINVAL);
		worker->offset = drm_vma_node_offset_addr(&shmem->base.vma_node);
		worker->address = test->host->start + 4 * PAGE_SIZE;
		test->report->allocation_stage = FAIL_FAULT;
		injected = 0;
		for (nth = 1; nth <= FAILURE_LIMIT; nth++) {
			unsigned int faults = test->report->faults;

			test->report->fail_nth = nth;
			if (!(!run(worker, GEM_MAP)))
				return fail(test, __LINE__, -EINVAL);
			/* No existing VMA shares this page-table subtree. Each trial
			 * starts cold; unmapping below must free the complete subtree.
			 */
			if (!(!mm_pgtables_bytes(worker->space->mm)))
				return fail(test, __LINE__, -EINVAL);
			worker->fail_nth = nth;
			worker->fault_retries = 0;
			worker->write = 0;
			if (!(!run(worker, GEM_ACCESS_FAILURE) && !worker->signal &&
				worker->observed == 0x5a5a5a5a5a5a5a5aULL &&
				test->report->faults > faults))
				return fail(test, __LINE__, -EINVAL);
			if (!worker->remaining) {
				if (!(worker->fault_retries &&
					test->report->faults >= faults + 2))
					return fail(test, __LINE__, -EINVAL);
				injected++;
				test->report->injected++;
				test->report->fault_retries += worker->fault_retries;
			}
			if (!(mm_pgtables_bytes(worker->space->mm)))
				return fail(test, __LINE__, -EINVAL);
			if (!(!run(worker, GEM_UNMAP)))
				return fail(test, __LINE__, -EINVAL);
			if (!(!worker->space->mm->map_count &&
				!mm_pgtables_bytes(worker->space->mm)))
				return fail(test, __LINE__, -EINVAL);
			if (!(!balanced_backing(test, shmem)))
				return fail(test, __LINE__, -EINVAL);
			test->report->page_table_reclaims++;
			test->report->recoveries++;
			if (worker->remaining)
				break;
		}
		if (!(injected && nth <= FAILURE_LIMIT))
			return fail(test, __LINE__, -EINVAL);
		if (!(!drm_gem_handle_delete(file, handle)))
			return fail(test, __LINE__, -EINVAL);
		drm_gem_object_put(&shmem->base);
		test->drain();
		if (!(sb->free_ispace == ispace &&
			!percpu_counter_sum(&sb->used_blocks) && vm_memory_committed() == committed))
			return fail(test, __LINE__, -EINVAL);
		worker->fail_nth = 0;
		worker->offset = original_offset;
		worker->address = test->host->start;
		if (!(!run(worker, GEM_MAP)))
			return fail(test, __LINE__, -EINVAL);
		test->report->sweeps++;
	}
	return 0;
}
#endif

static int pressure_access(void *argument)
{
	struct gem_case *test = argument;
	const u64 expected[BUFFER_PAGES] = { 0xfedcba0987654321ULL, 0xc35a, 0 };
	unsigned int cpu, index, alias;

	if (refcount_read(&test->shmem->pages_pin_count) != 3 ||
	    refcount_read(&test->shmem->vmap_use_count) != 2 ||
	    refcount_read(&test->shmem->pages_use_count) != 3 + test->pressure_alias)
		return fail(test, __LINE__, -EINVAL);
	for (index = 0; index < BUFFER_PAGES; index++) {
		if (page_to_pfn(test->shmem->pages[index]) != test->pfns[index])
			return fail(test, __LINE__, -EINVAL);
		for (alias = 0; alias < 2; alias++) {
			void *address = test->maps[alias].vaddr + index * PAGE_SIZE;

			if (vmalloc_to_page(address) != test->shmem->pages[index] ||
			    READ_ONCE(*(u64 *)address) != expected[index] ||
			    memchr_inv(address + sizeof(u64), 0, PAGE_SIZE - sizeof(u64)))
				return fail(test, __LINE__, -EINVAL);
		}
	}

	for (cpu = 0; cpu < 2; cpu++) {
		if (access_value(test, cpu, 0, false, expected[0]))
			return -EINVAL;
		test->report->pressure_accesses++;
	}
	return 0;
}

static int create_pressure_alias(struct gem_case *test)
{
	struct gem_worker *worker = &test->workers[0];
	unsigned long address = worker->address, length = worker->length;
	int result;

	worker->address = test->host->start + 7 * PAGE_SIZE;
	worker->length = PAGE_SIZE;
	worker->write = 0;
	result = run(worker, GEM_MAP);
	if (!result)
		result = run(worker, GEM_ACCESS);
	if (result || worker->signal || worker->observed != 0xfedcba0987654321ULL ||
	    worker->space->mm->map_count != 2)
		return fail(test, __LINE__, result ?: -EINVAL);
	worker->address = address;
	worker->length = length;
	test->pressure_alias = true;
	test->report->mappings++;
	return 0;
}

static int unmap_pressure_alias(struct gem_case *test)
{
	struct gem_worker *worker = &test->workers[0];
	unsigned long address = worker->address, length = worker->length;
	unsigned int refs = kref_read(&test->shmem->base.refcount);
	long file_refs = file_count(worker->file);
	int result;

	worker->address = test->host->start + 7 * PAGE_SIZE;
	worker->length = PAGE_SIZE;
	result = run(worker, GEM_UNMAP);
	worker->address = address;
	worker->length = length;
	if (result || worker->space->mm->map_count != 1 ||
	    kref_read(&test->shmem->base.refcount) != refs - 1 ||
	    file_count(worker->file) != file_refs - 1 ||
	    refcount_read(&test->shmem->pages_use_count) != 3)
		return fail(test, __LINE__, result ?: -EINVAL);
	test->pressure_alias = false;
	test->report->pressure_unmaps++;
	return 0;
}

static int cleanup_failed_map(struct gem_case *test)
{
#ifdef CONFIG_FAULT_INJECTION
	struct gem_worker *worker = &test->workers[0];
	unsigned long address = worker->address;
	unsigned long committed = vm_memory_committed();
	long file_refs = file_count(worker->file);
	int result;

	worker->address = test->host->start + 4 * PAGE_SIZE;
	worker->fail_nth = 1;
	result = run(worker, GEM_MAP_FAILURE);
	worker->fail_nth = 0;
	worker->address = address;
	/* The other CPU still owns a pending real fault. Fail native VMA
	 * allocation before admission reaches the now-revoked GEM handle.
	 */
	if (result != -ENOMEM || worker->remaining ||
	    worker->space->mm->map_count != 1 + test->pressure_alias ||
	    file_count(worker->file) != file_refs || vm_memory_committed() != committed)
		return fail(test, __LINE__, result ?: -EINVAL);
	test->report->cleanup_rollbacks++;
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

static int terminate_pending_fault(struct gem_case *test)
{
	struct gem_worker *worker = &test->workers[1];
	struct kobox_vm_space *space = worker->space;
	struct kobox_linux_vm_event event;
	long waited;
	int result;

	if (!test->host->terminate)
		return fail(test, __LINE__, -EOPNOTSUPP);
	result = test->host->terminate(test->host->pids[1]);
	if (result)
		return fail(test, __LINE__, result);
	/* The host must report actual reaped death, not merely a sent signal. */
	waited = wait_event_timeout(space->events,
		(result = space->operations->event(space->host_space, &event)) != -EAGAIN,
		GATE_WAIT);
	if (!waited || result || event.error || event.kind != KOBOX_VM_EVENT_EXIT ||
	    event.sequence != worker->sequence || (event.exit_status & 0x7f) != SIGKILL)
		return fail(test, __LINE__, result ?: -EINVAL);
	test->client_dead = true;
	test->report->client_deaths++;
	return 0;
}

static int pressure_close(void *argument)
{
	struct gem_case *test = argument;
	struct gem_worker *worker = &test->workers[1];
	struct completion received, release;
	unsigned int faults;
	u64 publications;
	int result;

	result = pressure_access(test);
	if (result)
		return result;
	faults = test->report->faults;
	publications = worker->space->publications;
	init_completion(&received);
	init_completion(&release);
	worker->fault_received = &received;
	worker->fault_release = &release;
	worker->address = test->host->start + 2 * PAGE_SIZE;
	worker->write = 0;
	worker->action = GEM_ACCESS;
	reinit_completion(&worker->done);
	complete(&worker->go);
	if (!wait_for_completion_timeout(&received, GATE_WAIT)) {
		result = -ETIMEDOUT;
		goto release;
	}
	result = cleanup_failed_map(test);
	if (result)
		goto release;
	result = unmap_pressure_alias(test);
	if (result)
		goto release;
	if (test->terminate_client) {
		result = terminate_pending_fault(test);
		if (result)
			goto release;
	}
	/* No MM locks are held by the pending machine fault. Native VMA/file
	 * references, not a test-owned page pin, must preserve the backing.
	 */
	result = release_kernel_owners(test);
release:
	complete_all(&release);
	if (!wait_for_completion_timeout(&worker->done, GATE_WAIT))
		__builtin_trap();
	worker->fault_received = NULL;
	worker->fault_release = NULL;
	if (result)
		return fail(test, __LINE__, result);
	if (worker->result != (test->client_dead ? -ESRCH : 0) || worker->signal ||
	    (!test->client_dead && worker->observed) ||
	    test->report->faults != faults + 1 || test->files[0] || test->files[1] ||
	    test->shmem->base.filp->f_mapping->nrpages != BUFFER_PAGES)
		return fail(test, __LINE__, worker->result ?: -EINVAL);
	if (test->client_dead) {
		if (worker->space->publications != publications)
			return fail(test, __LINE__, -EINVAL);
		test->report->aborted_faults++;
	} else {
		test->report->close_faults++;
	}
	return 0;
}

static int queue_deferred(struct gem_deferred *deferred)
{
	unsigned long flags;
	int result;

	spin_lock_irqsave(&deferred->lock, flags);
	if (deferred->stopping) {
		deferred->rejected++;
		result = -ESHUTDOWN;
	} else {
		result = queue_work(system_unbound_wq, &deferred->producer) ? 0 : -EBUSY;
	}
	spin_unlock_irqrestore(&deferred->lock, flags);
	return result;
}

static void retire_buffer(struct work_struct *work)
{
	struct gem_deferred *deferred = container_of(to_rcu_work(work),
						   struct gem_deferred, retire);

	/* RCU work, unlike an RCU callback, may run the native sleeping teardown. */
	drm_gem_object_put(deferred->object);
	deferred->object = NULL;
	deferred->callbacks++;
}

static void produce_buffer_work(struct work_struct *work)
{
	struct gem_deferred *deferred = container_of(work, struct gem_deferred,
						   producer);
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(deferred->object);
	const u64 expected[] = { 0xfedcba0987654321ULL, 0xc35a, 0x37a9e468b2c5d10fULL };
	unsigned int i;

	complete(&deferred->entered);
	if (!wait_for_completion_timeout(&deferred->release, GATE_WAIT)) {
		deferred->result = -ETIMEDOUT;
		return;
	}
	/* Both VMAs and every handle/lookup have gone. This is the sole owner. */
	if (kref_read(&deferred->object->refcount) != 1 || shmem->pages ||
	    refcount_read(&shmem->pages_use_count) ||
	    deferred->object->filp->f_mapping->nrpages != BUFFER_PAGES ||
	    file_count(deferred->object->filp) != 1)
		deferred->result = -EINVAL;
	for (i = 0; i < BUFFER_PAGES; i++) {
		struct folio *folio = filemap_get_folio(deferred->object->filp->f_mapping, i);

		if (IS_ERR(folio)) {
			deferred->result = PTR_ERR(folio);
			break;
		}
		if (READ_ONCE(*(u64 *)folio_address(folio)) != expected[i] ||
		    memchr_inv(folio_address(folio) + sizeof(u64), 0, PAGE_SIZE - sizeof(u64)))
			deferred->result = -EINVAL;
		folio_put(folio);
	}
	if (queue_deferred(deferred) != -ESHUTDOWN)
		deferred->result = -EINVAL;
	/* A terminal release is distinct from new utilization/requeue. Its
	 * dependency is producer -> RCU callback -> sleeping release work.
	 */
	if (!queue_rcu_work(system_unbound_wq, &deferred->retire))
		deferred->result = -EINVAL;
}

static int start_deferred(struct gem_case *test)
{
	struct gem_deferred *deferred = &test->deferred;
	unsigned long flags;

	spin_lock_init(&deferred->lock);
	init_completion(&deferred->entered);
	init_completion(&deferred->release);
	INIT_WORK(&deferred->producer, produce_buffer_work);
	INIT_RCU_WORK(&deferred->retire, retire_buffer);
	deferred->object = &test->shmem->base;
	drm_gem_object_get(deferred->object);
	if (queue_deferred(deferred) ||
	    !wait_for_completion_timeout(&deferred->entered, GATE_WAIT))
		return fail(test, __LINE__, -ETIMEDOUT);
	/* Close admission before the owner starts unmapping or dropping refs. */
	spin_lock_irqsave(&deferred->lock, flags);
	deferred->stopping = true;
	spin_unlock_irqrestore(&deferred->lock, flags);
	if (queue_deferred(deferred) != -ESHUTDOWN)
		return fail(test, __LINE__, -EINVAL);
	return 0;
}

static int finish_deferred(struct gem_case *test)
{
	struct gem_deferred *deferred = &test->deferred;

	complete(&deferred->release);
	cancel_work_sync(&deferred->producer);
	/* GP completion alone does not prove that the release work has run. */
	flush_rcu_work(&deferred->retire);
	test->drain();
	if (deferred->result || deferred->object || deferred->callbacks != 1 ||
	    deferred->rejected != 2 || work_busy(&deferred->producer) ||
	    work_busy(&deferred->retire.work))
		return fail(test, __LINE__, deferred->result ?: -EINVAL);
	test->report->deferred_callbacks += deferred->callbacks;
	test->report->rejected_work += deferred->rejected;
	return 0;
}

static int lifetime_test(const struct kobox_linux_vm_test *host,
	struct kobox_gem_lifetime_report *report, void (*drain)(void),
	enum kobox_gem_final_owner final_owner,
	const struct kobox_gem_failure_services *services)
{
	struct gem_case *test;
	struct kobox_vm_lifetime *task_audit = NULL;
	int result;

	if (!host || host->size != sizeof(*host) || !report || !drain ||
	    !host->operations || !host->probe || !host->spaces[0] ||
	    !host->spaces[1] || host->spaces[0] == host->spaces[1] ||
	    !host->pids[0] || !host->pids[1] || host->pids[0] == host->pids[1] ||
	    host->length < 8 * PAGE_SIZE || num_online_cpus() != 2 ||
	    (final_owner != KOBOX_GEM_FINAL_VMA && final_owner != KOBOX_GEM_FINAL_OBJECT))
		return -EINVAL;
	/* Failed assertions retain test storage until launcher process exit. */
	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->host = host;
	test->report = report;
	test->drain = drain;
	test->final_owner = final_owner;
	test->cleanup = services && services->cleanup;
	test->terminate_client = services && services->cleanup == KOBOX_GEM_CLEANUP_DEATH;
	test->probes = kcalloc(2 * REUSE_BATCH, sizeof(void *), GFP_KERNEL);
	if (!test->probes)
		return fail(test, __LINE__, -ENOMEM);
	report->phase = 1;
	result = create_device(test);
	if (result)
		return result;
	report->phase = 2;
	result = create_buffer(test);
	if (result)
		return result;
	report->phase = 3;
	result = start_clients(test);
	if (result)
		return result;
	if (services) {
#ifdef CONFIG_FAULT_INJECTION
		result = failure_sweeps(test, services) ?: fault_failure_sweeps(test, services);
#else
		result = -EOPNOTSUPP;
#endif
		if (result)
			return result;
	}
	report->phase = 4;
	result = share_content(test);
	if (result)
		return result;
	if (services) {
		if (test->cleanup) {
			result = create_pressure_alias(test) ?: revoke_handles(test);
			if (result)
				return result;
		}
		report->pressure.size = sizeof(report->pressure);
		result = services->pressure(&report->pressure,
				test->cleanup ? pressure_close : pressure_access, test);
		if (result)
			return result;
	}
	report->phase = 5;
	result = test->cleanup ? 0 : close_handles(test);
	if (result)
		return result;
	report->phase = 6;
	result = access_after_close(test);
	if (result)
		return result;
	report->phase = 7;
	if (test->cleanup) {
		result = start_deferred(test);
		if (result)
			return result;
	}
	result = unmap_clients(test);
	if (result)
		return result;
	if (test->cleanup) {
		result = finish_deferred(test);
		if (result)
			return result;
	}
	report->phase = 8;
	result = reclaim_pages(test);
	if (!result)
		result = reclaim_object(test);
	if (result)
		return result;
	report->phase = 9;
	if (services) {
		struct mm_struct *mms[2] = {
			test->workers[0].space->mm, test->workers[1].space->mm,
		};
		struct task_struct *tasks[2] = {
			test->workers[0].task, test->workers[1].task,
		};

		task_audit = services->tasks_begin(mms, tasks, &report->task_lifetime);
		if (IS_ERR(task_audit))
			return fail(test, __LINE__, PTR_ERR(task_audit));
	}
	result = stop_clients(test);
	if (result)
		return result;
	if (task_audit) {
		result = services->tasks_finish(task_audit);
		if (result)
			return fail(test, __LINE__, result);
	}
	drm_dev_unregister(test->dev);
	drm_dev_put(test->dev);
	root_device_unregister(test->parent);
	kern_unmount(test->mnt);
	drain();
	kfree(test->probes);
	kfree(test);
	report->phase = 10;
	return 0;
}

int kobox_gem_lifetime_test(const struct kobox_linux_vm_test *host,
	struct kobox_gem_lifetime_report *report, void (*drain)(void),
	enum kobox_gem_final_owner final_owner)
{
	return lifetime_test(host, report, drain, final_owner, NULL);
}
EXPORT_SYMBOL_GPL(kobox_gem_lifetime_test);

int kobox_gem_failure_test(const struct kobox_linux_vm_test *host,
	struct kobox_gem_lifetime_report *report,
	const struct kobox_gem_failure_services *services,
	enum kobox_gem_final_owner final_owner)
{
	if (!services || !services->mount || !services->drain ||
	    !services->vmalloc_pages || !services->pressure ||
	    !services->tasks_begin || !services->tasks_finish ||
	    services->cleanup > KOBOX_GEM_CLEANUP_DEATH)
		return -EINVAL;
	return lifetime_test(host, report, services->drain, final_owner, services);
}
EXPORT_SYMBOL_GPL(kobox_gem_failure_test);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Real GEM shmem object/handle/two-process mapping lifetime test");
