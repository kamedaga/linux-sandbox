// SPDX-License-Identifier: GPL-2.0-only
#include "fs_shared.h"
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/memfd.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/shmem_fs.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/swap.h>
#include <linux/sort.h>
#include <linux/sched/signal.h>
#include <linux/rwsem.h>
#include <linux/xarray.h>

struct fs_shared_mapping { struct list_head exports; };

struct fs_shared_export {
	struct list_head mapping_link;
	struct fs_shared_mapping *group;
	struct list_head link;
	struct file *file;
	struct vm_area_struct *vma;
	struct folio **folios;
	u64 *pages;
	u64 *dirty;
	size_t dirty_words;
	u64 id, offset, flags, charged;
	size_t count;
	bool writable;
	bool restore_evictable;
};
struct kobox_linux_fs_shared {
	struct rw_semaphore lock;
	struct list_head exports;
	/* Index lifetime is covered by lock; exports retain mapping through file. */
	struct xarray by_mapping;
	struct delayed_work maintenance;
	struct kobox_linux_fs_shared_ops ops;
	u64 bytes;
	bool bound, closing;
};

/* This detached VMA describes filesystem callbacks, not Linux CPU aliases.
 * The actual aliases and their TLB retirement belong to the native VM. Use
 * the VFS compatibility entry so both mmap and mmap_prepare are honoured.
 */
static int fs_shared_prepare(struct fs_shared_export *view, vm_flags_t flags)
{
	struct vm_area_struct *vma = vm_area_alloc(&init_mm);
	int error;
	if (!vma)
		return -ENOMEM;
	vma->vm_start = PAGE_SIZE;
	vma->vm_end = PAGE_SIZE + (view->count << PAGE_SHIFT);
	vma->vm_pgoff = view->offset >> PAGE_SHIFT;
	vma->vm_file = get_file(view->file);
	vm_flags_init(vma, flags);
	vma->vm_page_prot = vm_get_page_prot(flags);
	mmap_write_lock(&init_mm);
	error = vfs_mmap(view->file, vma);
	mmap_write_unlock(&init_mm);
	view->vma = vma;
	if (error)
		return error;
	/* Device, DAX and remapped/stacked files need their own export protocol;
	 * they must never be mistaken for this file's ordinary page cache.
	 */
	if (vma->vm_file != view->file ||
	    (vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP | VM_IO)) ||
	    !vma->vm_ops || (view->writable && !shmem_file(view->file) &&
		!vma->vm_ops->page_mkwrite))
		return -EOPNOTSUPP;
	return 0;
}

static void fs_shared_unprepare(struct fs_shared_export *view)
{
	struct vm_area_struct *vma = view->vma;
	if (!vma)
		return;
	mmap_write_lock(&init_mm);
	if (vma->vm_ops && vma->vm_ops->close)
		vma->vm_ops->close(vma);
	mmap_write_unlock(&init_mm);
	fput(vma->vm_file);
	vm_area_free(vma);
}

static int fs_shared_mkwrite(struct fs_shared_export *view, struct folio *folio,
	pgoff_t index)
{
	struct vm_area_struct *vma = view->vma;
	vm_fault_t result;
	struct vm_fault vmf = {
		.vma = vma,
		.gfp_mask = mapping_gfp_mask(view->file->f_mapping),
		.pgoff = index,
		.address = vma->vm_start + ((index - vma->vm_pgoff) << PAGE_SHIFT),
		.flags = FAULT_FLAG_WRITE | FAULT_FLAG_MKWRITE,
		.page = folio_page(folio, index - folio->index),
	};
	if (!view->writable || !vma->vm_ops->page_mkwrite)
		return 0;
	/* In particular ext4 must reserve/allocate blocks before native writes;
	 * merely dirtying read_mapping_folio() would lose writes into file holes.
	 * The service registry excludes truncate while this callback runs.
	 */
	mmap_read_lock(&init_mm);
	result = vma->vm_ops->page_mkwrite(&vmf);
	if (result & VM_FAULT_LOCKED)
		folio_unlock(folio);
	mmap_read_unlock(&init_mm);
	if (result & VM_FAULT_OOM)
		return -ENOMEM;
	if (result & (VM_FAULT_ERROR | VM_FAULT_NOPAGE | VM_FAULT_RETRY))
		return -EIO;
	return 0;
}

static void fs_shared_put_pages(struct folio **folios, size_t count)
{
	size_t i;
	struct folio_batch batch;
	folio_batch_init(&batch);
	if (folios) {
		for (i = 0; i < count; i++)
			if (folios[i] && !folio_batch_add(&batch, folios[i])) {
				check_move_unevictable_folios(&batch);
				__folio_batch_release(&batch);
			}
		if (batch.nr) {
			check_move_unevictable_folios(&batch);
			__folio_batch_release(&batch);
		}
	}
	kvfree(folios);
}

static void fs_shared_restore_eviction(struct kobox_linux_fs_shared *shared,
	struct fs_shared_export *view)
{
	if (!view->restore_evictable)
		return;
	if (view->group && !list_empty(&view->group->exports))
		return;
	mapping_clear_unevictable(view->file->f_mapping);
}

/* Each populated slot owns a folio ref even when several slots name a large
 * folio. Pins prohibit allocator reuse; a parent RAM VMO reference does not.
 * Keep both old and new arrays alive until native TLB retirement succeeds.
 */
static int fs_shared_pages(struct fs_shared_export *view,
	struct folio ***out_folios, u64 **out_pages)
{
	struct address_space *mapping = view->file->f_mapping;
	struct folio **folios;
	u64 *pages;
	loff_t size = i_size_read(file_inode(view->file));
	const size_t batch_pages = (2UL * 1024 * 1024) / PAGE_SIZE;
	size_t i;
	int error = -ENOMEM;

	folios = kvcalloc(view->count, sizeof(*folios), GFP_KERNEL);
	pages = kvmalloc_array(view->count, sizeof(*pages), GFP_KERNEL);
	if (!folios || !pages)
		goto fail;
	for (i = 0; i < view->count; i++) {
		pgoff_t index = (view->offset >> PAGE_SHIFT) + i;
		struct folio *folio;

		pages[i] = U64_MAX;
		if (size <= 0 || index > ((u64)(size - 1) >> PAGE_SHIFT))
			continue;
		/* An export already needs every in-file page. read_mapping_folio()
		 * alone waits for one cold folio at a time, bypassing the readahead
		 * used by Linux file reads/faults. Submit bounded contiguous batches
		 * through the filesystem's normal readahead path, then retain the
		 * same per-folio validation, error handling and native pin lifetime.
		 * Shmem has no disk reads; EOF slots remain holes, not prefetched RAM.
		 */
		if (!shmem_file(view->file) && !(i % batch_pages)) {
			unsigned long nr = min_t(u64, view->count - i,
				((u64)(size - 1) >> PAGE_SHIFT) - index + 1);
			DEFINE_READAHEAD(ractl, view->file, &view->file->f_ra,
				mapping, index);
			/* Match Linux's forced readahead cap of 2 MiB per batch. */
			page_cache_ra_unbounded(&ractl, min(nr, batch_pages), 0);
		}
		folio = shmem_file(view->file) ? shmem_read_folio(mapping, index) :
			read_mapping_folio(mapping, index, view->file);
		if (IS_ERR(folio)) {
			error = PTR_ERR(folio);
			goto fail;
		}
		folios[i] = folio;
		error = fs_shared_mkwrite(view, folio, index);
		if (error)
			goto fail;
		pages[i] = page_to_pfn(folio_page(folio, index - folio->index));
	}
	*out_folios = folios;
	*out_pages = pages;
	return 0;
fail:
	fs_shared_put_pages(folios, view->count);
	kvfree(pages);
	return error;
}

static void fs_shared_dirty_all(struct fs_shared_export *view)
{
	size_t i;
	if (!view->writable)
		return;
	/* One-time publication/retirement boundaries conservatively cover writes
	 * through aliases that no longer have a PTE to harvest. Periodic and
	 * sync admission use the native logical-page dirty bitmap instead. */
	for (i = 0; i < view->count; i++) {
		struct folio *folio = view->folios[i];
		if (!folio)
			continue;
		/* Writeback can clean an exported folio between native writes.
		 * Filesystems such as ext4 require the folio lock when the first
		 * dirty mark updates its buffer heads again. */
		folio_lock(folio);
		folio_mark_dirty(folio);
		folio_unlock(folio);
	}
}

static int fs_shared_collect_dirty(struct kobox_linux_fs_shared *shared,
	struct fs_shared_export *view)
{
	size_t word;
	int error;
	if (!view->writable)
		return 0;
	error = shared->ops.collect_dirty(shared->ops.context, view->id,
		view->dirty, view->dirty_words);
	if (error)
		return error;
	for (word = 0; word < view->dirty_words; word++) {
		u64 bits = view->dirty[word];
		while (bits) {
			unsigned int bit = __ffs64(bits);
			size_t index = word * 64 + bit;
			struct folio *folio;
			bits &= bits - 1;
			if (index >= view->count)
				return -EPROTO;
			folio = view->folios[index];
			if (!folio)
				continue;
			/* ext4 updates buffer-head dirty state under the folio lock.
			 * A native ACK transfers evidence here before Linux writeback;
			 * subsequent CPU writes stay armed for the next collection. */
			folio_lock(folio);
			folio_mark_dirty(folio);
			folio_unlock(folio);
		}
	}
	return 0;
}

static void fs_shared_drop(struct kobox_linux_fs_shared *shared,
	struct fs_shared_export *view)
{
	fs_shared_dirty_all(view);
	list_del(&view->link);
 list_del(&view->mapping_link);
 fs_shared_restore_eviction(shared, view);
 if (list_empty(&view->group->exports)) {
  xa_erase(&shared->by_mapping, (unsigned long)view->file->f_mapping);
  kfree(view->group);
 }
	shared->bytes -= view->charged;
	if (view->writable)
		mapping_unmap_writable(view->file->f_mapping);
	fs_shared_put_pages(view->folios, view->count);
	kvfree(view->pages);
	kvfree(view->dirty);
	fs_shared_unprepare(view);
 /* Reaping is an admission barrier, not eventual garbage collection. The
  * last exported writable OFD must release i_writecount before an exec open
  * or seal request observes it. These regular-file pins cannot reenter the
  * native registry; do not flush unrelated global delayed-fput work. */
 __fput_sync(view->file);
 kfree(view);
}

static void fs_shared_maintenance(struct work_struct *work)
{
	struct kobox_linux_fs_shared *shared = container_of(to_delayed_work(work),
		struct kobox_linux_fs_shared, maintenance);
	struct fs_shared_export *view, *next;

	/* An idle timer must not queue a writer behind an inode-blocked reader:
	 * rwsem writer preference would then block unrelated empty-list readers.
	 * Only a nonempty inventory needs exclusive dirty/reap work. */
	down_read(&shared->lock);
	if (list_empty(&shared->exports)) {
		if (!shared->closing)
			schedule_delayed_work(&shared->maintenance, msecs_to_jiffies(100));
		up_read(&shared->lock);
		return;
	}
	up_read(&shared->lock);
	down_write(&shared->lock);
	list_for_each_entry_safe(view, next, &shared->exports, link) {
		int error = fs_shared_collect_dirty(shared, view);
		if (error) {
			pr_warn_ratelimited("native shared dirty collection failed: %d\n", error);
			continue; /* Preserve export/pins and retry the native inventory. */
		}
		if (shared->ops.retire(shared->ops.context, view->id, 0) == 1)
			fs_shared_drop(shared, view);
	}
	if (!shared->closing)
		schedule_delayed_work(&shared->maintenance, msecs_to_jiffies(100));
	up_write(&shared->lock);
}

struct kobox_linux_fs_shared *kobox_linux_fs_shared_create(void)
{
	struct kobox_linux_fs_shared *shared = kzalloc(sizeof(*shared), GFP_KERNEL);
	if (!shared)
		return ERR_PTR(-ENOMEM);
	init_rwsem(&shared->lock);
	INIT_LIST_HEAD(&shared->exports);
	xa_init(&shared->by_mapping);
	INIT_DELAYED_WORK(&shared->maintenance, fs_shared_maintenance);
	return shared;
}

int kobox_linux_fs_shared_bind(struct kobox_linux_fs_shared *shared,
	const struct kobox_linux_fs_shared_ops *ops)
{
	if (!shared || !ops || !ops->create || !ops->refresh || !ops->collect_dirty || !ops->retire ||
	    !ops->budget_bytes)
		return -EINVAL;
	down_write(&shared->lock);
	if (shared->bound || shared->closing) {
		up_write(&shared->lock);
		return -EBUSY;
	}
	shared->ops = *ops;
	shared->bound = true;
	schedule_delayed_work(&shared->maintenance, msecs_to_jiffies(100));
	up_write(&shared->lock);
	return 0;
}

int kobox_linux_fs_shared_close(struct kobox_linux_fs_shared *shared)
{
	struct fs_shared_export *view, *next;
	int error = 0;
	if (!shared)
		return 0;
	down_write(&shared->lock);
	shared->closing = true;
	up_write(&shared->lock);
	cancel_delayed_work_sync(&shared->maintenance);
	down_write(&shared->lock);
	list_for_each_entry_safe(view, next, &shared->exports, link) {
		int retired = shared->ops.retire(shared->ops.context, view->id, 1);
		if (retired == 1)
			fs_shared_drop(shared, view);
		else
			error = retired < 0 ? retired : -EBUSY;
	}
	up_write(&shared->lock);
	return error;
}

void kobox_linux_fs_shared_destroy(struct kobox_linux_fs_shared *shared)
{
	if (!shared)
		return;
	/* Service close must succeed before mount teardown can release this
	 * registry. Failed revocation is retained ownership, not a safe unpin.
	 */
	BUG_ON(kobox_linux_fs_shared_close(shared));
	xa_destroy(&shared->by_mapping);
	kfree(shared);
}

static bool fs_shared_matches(const struct fs_shared_export *view,
	const struct kobox_linux_fs_shared_guard *guard)
{
	return (!guard->mapping || view->file->f_mapping == guard->mapping) &&
		(!guard->superblock || file_inode(view->file)->i_sb == guard->superblock);
}

/* A target inode lookup must not walk all other native mappings. Index
 * entries disappear only after the last export releases its retained file. */
static struct fs_shared_export *fs_shared_next(struct kobox_linux_fs_shared *shared,
 const struct kobox_linux_fs_shared_guard *guard, struct fs_shared_export *previous)
{
 struct list_head *head, *node;
 if (guard->mapping) {
  struct fs_shared_mapping *group=xa_load(&shared->by_mapping,(unsigned long)guard->mapping);
  if (!group) return NULL;
  head=&group->exports;node=previous?previous->mapping_link.next:head->next;
  return node==head?NULL:list_entry(node,struct fs_shared_export,mapping_link);
 }
 head=&shared->exports;node=previous?previous->link.next:head->next;
 for (;node!=head;node=node->next) {
  struct fs_shared_export *view=list_entry(node,struct fs_shared_export,link);
  if (fs_shared_matches(view,guard)) return view;
 }
 return NULL;
}
static bool fs_shared_has_exports(struct kobox_linux_fs_shared *shared,
 const struct kobox_linux_fs_shared_guard *guard)
{
 return fs_shared_next(shared,guard,NULL)!=NULL;
}

static int fs_shared_enter_mapping(struct kobox_linux_fs_shared *shared, bool nowait,
	bool publishing, struct address_space *mapping, struct super_block *superblock,
	struct kobox_linux_fs_shared_guard *guard)
{
	struct fs_shared_export *view, *next;
	guard->mode = KOBOX_FS_SHARED_NONE;
	guard->mapping = mapping;
	guard->superblock = superblock;
	if (!shared)
		return 0;
	/* Publication can allocate, run VFS mmap callbacks and enter native IPC;
	 * it belongs on a worker, never the nonblocking owner/inline lane. */
	if (publishing && nowait)
		return -EAGAIN;
	if (!publishing) {
		if (nowait) {
			if (!down_read_trylock(&shared->lock))
				return -EAGAIN;
		} else {
			down_read(&shared->lock);
		}
		if (shared->closing) {
			up_read(&shared->lock);
			return -ESHUTDOWN;
		}
		/* No aliases of this file exist. Keep registry read admission until
		 * VFS returns: publication requires write admission, so no export
		 * can race a page replacement. Unrelated files may mutate together;
		 * their pinned aliases neither need refresh nor confer authority here.
		 * A NULL mapping retains the conservative namespace/global scope.
		 */
		if (!fs_shared_has_exports(shared, guard)) {
			guard->mode = KOBOX_FS_SHARED_READ;
			return 0;
		}
		up_read(&shared->lock);
		if (nowait)
			return -EAGAIN;
	}
	/* Never upgrade while holding read admission. Recheck closing/exports
	 * under write exclusion; maintenance may have retired them in between. */
	down_write(&shared->lock);
	if (shared->closing) {
		up_write(&shared->lock);
		return -ESHUTDOWN;
	}
	guard->mode = KOBOX_FS_SHARED_WRITE;
	// Reap before VFS observes mmap-writer counters: after the last native
	// munmap, F_ADD_SEALS must not see a fictitious writer until the timer fires.
	for (view=fs_shared_next(shared,guard,NULL);view;view=next) {
  next=fs_shared_next(shared,guard,view);
  if (shared->ops.retire(shared->ops.context,view->id,0)==1) fs_shared_drop(shared,view);
 }
 for (view=fs_shared_next(shared,guard,NULL);view;view=fs_shared_next(shared,guard,view)) {
		int error;
		if (!fs_shared_matches(view, guard))
			continue;
		error = fs_shared_collect_dirty(shared, view);
		if (error) {
			up_write(&shared->lock);
			guard->mode = KOBOX_FS_SHARED_NONE;
			return error;
		}
	}
	return 0;
}

int kobox_linux_fs_shared_enter(struct kobox_linux_fs_shared *shared, bool nowait,
	bool publishing, struct kobox_linux_fs_shared_guard *guard)
{
	return fs_shared_enter_mapping(shared, nowait, publishing, NULL, NULL, guard);
}

int kobox_linux_fs_shared_enter_file(struct kobox_linux_fs_shared *shared, bool nowait,
	bool publishing, struct file *file, struct kobox_linux_fs_shared_guard *guard)
{
	/* Request admission pins this exact struct file through leave. Never use
	 * an untrusted inode number or a handle that could have been recycled. */
	return fs_shared_enter_mapping(shared, nowait, publishing,
		file ? file->f_mapping : NULL, NULL, guard);
}

/* The pinned namei result is the very path subsequently opened, not an
 * inode guessed from a string then looked up again after native IPC. */
int kobox_linux_fs_shared_enter_path(struct kobox_linux_fs_shared *shared, bool nowait,
 const struct path *path, struct kobox_linux_fs_shared_guard *guard)
{
 return fs_shared_enter_mapping(shared,nowait,false,d_inode(path->dentry)->i_mapping,NULL,guard);
}
int kobox_linux_fs_shared_enter_sb(struct kobox_linux_fs_shared *shared, bool nowait,
 struct file *file, struct kobox_linux_fs_shared_guard *guard)
{
 if (!file) return -EBADF;
 return fs_shared_enter_mapping(shared,nowait,false,NULL,file_inode(file)->i_sb,guard);
}

/* Buffered reads consume the same folios as a native shared alias, so they
 * need only keep publication/revocation out while VFS runs. O_DIRECT may
 * invalidate or write back those folios; inspect f_flags under the read
 * guard, which also excludes the service's F_SETFL write guard. Reuse the
 * full path for direct I/O, including its nowait punt on an exported file.
 */
int kobox_linux_fs_shared_enter_read(struct kobox_linux_fs_shared *shared,
	bool nowait, struct file *file, struct kobox_linux_fs_shared_guard *guard)
{
	bool direct;

	guard->mode = KOBOX_FS_SHARED_NONE;
	guard->mapping = file ? file->f_mapping : NULL;
	guard->superblock = NULL;
	if (!shared)
		return 0;
	if (nowait) {
		if (!down_read_trylock(&shared->lock))
			return -EAGAIN;
	} else {
		down_read(&shared->lock);
	}
	if (shared->closing) {
		up_read(&shared->lock);
		return -ESHUTDOWN;
	}
	direct = file && (READ_ONCE(file->f_flags) & O_DIRECT) &&
		!list_empty(&shared->exports);
	if (direct) {
		up_read(&shared->lock);
		return kobox_linux_fs_shared_enter_file(shared, nowait, false, file, guard);
	}
	guard->mode = KOBOX_FS_SHARED_READ;
	return 0;
}

int kobox_linux_fs_shared_leave(struct kobox_linux_fs_shared *shared, bool refresh,
	struct kobox_linux_fs_shared_guard *guard)
{
	struct fs_shared_export *view;
	int error = 0;
	if (!shared)
		return 0;
	if (guard->mode == KOBOX_FS_SHARED_NONE)
		return -EINVAL;
	if (guard->mode == KOBOX_FS_SHARED_READ) {
		/* Publishers, F_SETFL and maintenance need write admission. An
		 * existing export is safe with buffered reads of its pinned folios. */
		up_read(&shared->lock);
		guard->mode = KOBOX_FS_SHARED_NONE;
		return 0;
	}
	if (refresh)
		for (view=fs_shared_next(shared,guard,NULL);view;view=fs_shared_next(shared,guard,view)) {
			struct folio **folios = NULL;
			u64 *pages = NULL;
			int status;
			if (!fs_shared_matches(view, guard))
				continue;
			status = fs_shared_pages(view, &folios, &pages);
			if (!status && memcmp(pages, view->pages, view->count * sizeof(*pages))) {
				status = shared->ops.refresh(shared->ops.context, view->id,
					pages, view->count);
				/* Native retirement ACK closes the last old-alias writes.
				 * Dirty OLD folios before dropping their lifetime pins. */
				if (!status)
					fs_shared_dirty_all(view);
			}
			if (!status) {
				fs_shared_put_pages(view->folios, view->count);
				kvfree(view->pages);
				view->folios = folios;
				view->pages = pages;
			} else {
				// A Linux mutation may already have completed. Keep its old
				// pins on native failure; never recycle still-aliased pages.
				fs_shared_put_pages(folios, view->count);
				kvfree(pages);
				error = status;
			}
		}
	up_write(&shared->lock);
	guard->mode = KOBOX_FS_SHARED_NONE;
	return error;
}

static int fs_shared_folio_order(const void *left, const void *right)
{
	unsigned long a = folio_pfn(*(struct folio * const *)left);
	unsigned long b = folio_pfn(*(struct folio * const *)right);
	return (a > b) - (a < b);
}

static size_t fs_shared_owned_refs(struct folio **pins, size_t count,
	struct folio *folio)
{
	size_t low = 0, high = count, first;
	unsigned long pfn = folio_pfn(folio);
	while (low < high) {
		size_t mid = low + (high - low) / 2;
		if (folio_pfn(pins[mid]) < pfn)
			low = mid + 1;
		else
			high = mid;
	}
	first = low;
	high = count;
	while (low < high) {
		size_t mid = low + (high - low) / 2;
		if (folio_pfn(pins[mid]) <= pfn)
			low = mid + 1;
		else
			high = mid;
	}
	return low - first;
}

static bool fs_shared_has_other_pins(struct address_space *mapping,
	struct folio **pins, size_t count)
{
	XA_STATE(xas, &mapping->i_pages, 0);
	struct folio *folio;
	unsigned int latency = 0;
	bool busy = false;

	xas_lock_irq(&xas);
	xas_for_each(&xas, folio, ULONG_MAX) {
		if (!xa_is_value(folio)) {
			size_t expected;
			int baseline = folio_expected_ref_count(folio);
			if (baseline < 0 || check_add_overflow((size_t)baseline,
				fs_shared_owned_refs(pins, count, folio), &expected) ||
			    expected > INT_MAX || folio_ref_count(folio) != expected) {
				busy = true;
				break;
			}
		}
		if (++latency < XA_CHECK_SCHED)
			continue;
		latency = 0;
		xas_pause(&xas);
		xas_unlock_irq(&xas);
		cond_resched();
		xas_lock_irq(&xas);
	}
	xas_unlock_irq(&xas);
	return busy;
}

bool kobox_linux_fs_shared_add_seals(struct kobox_linux_fs_shared *shared,
	struct file *file, unsigned long argument, long *result)
{
	const unsigned int all_seals = F_SEAL_SEAL | F_SEAL_EXEC | F_SEAL_SHRINK |
		F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE;
	struct fs_shared_export *view;
	struct inode *inode = file_inode(file);
	struct address_space *mapping = file->f_mapping;
	struct folio **pins = NULL;
	size_t capacity = 0, count = 0, i;
	unsigned int seals = (unsigned int)argument, *current_seals;
	int error, scan;

	if (!shared || !shmem_file(file) ||
	    !(seals & (F_SEAL_WRITE | F_SEAL_EXEC)))
		return false;
	lockdep_assert_held(&shared->lock);
	if (list_empty(&shared->exports))
		return false;
	list_for_each_entry(view, &shared->exports, link) {
		if (view->file->f_mapping != mapping)
			continue;
		/* Current PROT_READ is not sufficient: mprotect may restore WRITE.
		 * Any writable lifetime ceiling must use Linux's normal rejection.
		 */
		if (view->writable)
			return false;
		for (i = 0; i < view->count; i++)
			if (view->folios[i] && check_add_overflow(capacity,
				(size_t)1, &capacity)) {
				*result = -ENOMEM;
				return true;
			}
	}
	if (!capacity)
		return false;
	lockdep_assert_held_write(&shared->lock);

	/* Mirror the real fcntl admission under the caller's credential scope.
	 * Linux's private memfd pin scanner cannot distinguish native RO aliases
	 * from writable GUP. This bridge changes ONLY that refcount comparison;
	 * it neither drops physical pins nor fabricates Linux PTE/mapcount state.
	 * Keep admission in sync with fs/fcntl.c and mm/memfd.c; unrelated fcntl
	 * operations and files still go through the unmodified Linux syscall.
	 */
	error = -EBADF;
	if (file->f_mode & FMODE_PATH)
		goto done;
	error = security_file_fcntl(file, F_ADD_SEALS, argument);
	if (error)
		goto done;
	error = -EPERM;
	if (!(file->f_mode & FMODE_WRITE))
		goto done;
	error = -EINVAL;
	if (seals & ~all_seals)
		goto done;
	/* Lock order stays registry -> inode -> xarray; no folio lock is held
	 * across inode acquisition. Registry ownership keeps every counted pin
	 * alive throughout retries, including duplicate slots in large folios.
	 * Orphan folios remain pinned but cannot match a different cache folio.
	 */
	inode_lock(inode);
	current_seals = &SHMEM_I(inode)->seals;
	error = -EPERM;
	if (*current_seals & F_SEAL_SEAL)
		goto unlock;
	if ((seals & F_SEAL_EXEC) && (inode->i_mode & 0111))
		seals |= F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE;
	if ((seals & F_SEAL_WRITE) && !(*current_seals & F_SEAL_WRITE)) {
		error = mapping_deny_writable(mapping);
		if (error)
			goto unlock;
		pins = kvmalloc_array(capacity, sizeof(*pins), GFP_KERNEL);
		if (!pins) {
			error = -ENOMEM;
			goto allow_writable;
		}
		list_for_each_entry(view, &shared->exports, link)
			if (view->file->f_mapping == mapping)
				for (i = 0; i < view->count; i++)
					if (view->folios[i])
						pins[count++] = view->folios[i];
		sort(pins, count, sizeof(*pins), fs_shared_folio_order, NULL);
		lru_add_drain();
		for (scan = 0; scan <= 4; scan++) {
			if (!fs_shared_has_other_pins(mapping, pins, count))
				break;
			if (scan == 4) {
				error = -EBUSY;
				goto allow_writable;
			}
			if (!scan)
				lru_add_drain_all();
			else if (schedule_timeout_killable((HZ << scan) / 200)) {
				if (fs_shared_has_other_pins(mapping, pins, count)) {
					error = -EBUSY;
					goto allow_writable;
				}
				break;
			}
		}
	}
	*current_seals |= seals;
	error = 0;
	goto unlock;
allow_writable:
	mapping_allow_writable(mapping);
unlock:
	inode_unlock(inode);
	kvfree(pins);
done:
	*result = error;
	return true;
}

int kobox_linux_fs_shared_export(struct kobox_linux_fs_shared *shared,
	struct file *file, u64 client, u64 sequence, u64 offset, u64 length,
	u64 flags, u32 prot, u64 *id, u64 *file_size)
{
	struct fs_shared_export *view;
	struct fs_shared_export *other;
	u64 count, charged, dirty_bytes;
	vm_flags_t vm_flags;
	int error;

	/* Caller owns registry write admission, a retained authenticated file and the
	 * request credential scope. Only regular page-cache/shmem mappings enter.
	 */
	if (!shared || !shared->bound)
		return -EOPNOTSUPP;
	lockdep_assert_held_write(&shared->lock);
	if (flags & ~3ULL || prot & ~7U || !length || (offset | length) & (PAGE_SIZE - 1) ||
	    offset > S64_MAX || length > S64_MAX - offset)
		return -EINVAL;
	if (!S_ISREG(file_inode(file)->i_mode))
		return -ENODEV;
	if (!can_mmap_file(file) || IS_DAX(file_inode(file)) ||
	    (!shmem_file(file) && !file->f_mapping->a_ops->read_folio))
		return -ENODEV;
	if (!(file->f_mode & FMODE_READ))
		return -EACCES;
	if ((prot & 2) && (!(file->f_mode & FMODE_WRITE) || !(flags & 1)))
		return -EACCES;
	if ((prot & 4) && (!(flags & 2) || path_noexec(&file->f_path)))
		return -EACCES;
	vm_flags = VM_SHARED | VM_MAYREAD |
		((flags & 1) && (file->f_mode & FMODE_WRITE) ? VM_MAYWRITE : 0) |
		((flags & 2) && !path_noexec(&file->f_path) ? VM_MAYEXEC : 0) |
		((prot & 1) ? VM_READ : 0) | ((prot & 2) ? VM_WRITE : 0) |
		((prot & 4) ? VM_EXEC : 0);
	error = memfd_check_seals_mmap(file, &vm_flags);
	if (error)
		return error;
	error = security_mmap_file(file, prot, MAP_SHARED);
	if (error)
		return error;
	/* Seals/noexec can attenuate future mprotect without rejecting a valid
	 * initially read-only mapping. Publish the actual lifetime ceiling. */
	flags = ((vm_flags & VM_MAYWRITE) ? 1 : 0) | ((vm_flags & VM_MAYEXEC) ? 2 : 0);
	count = length >> PAGE_SHIFT;
	if (count > (U64_MAX - sizeof(*view)) / (2 * (sizeof(u64) + sizeof(void *))))
		return -ENOMEM;
	charged = sizeof(*view) + sizeof(struct fs_shared_mapping) + count * 2 * (sizeof(u64) + sizeof(void *));
	dirty_bytes = (flags & 1) ? DIV_ROUND_UP_ULL(count, 64) * sizeof(u64) : 0;
	if (charged > U64_MAX - dirty_bytes)
		return -ENOMEM;
	charged += dirty_bytes;
	if (charged > shared->ops.budget_bytes || shared->bytes > shared->ops.budget_bytes - charged)
		return -ENOMEM;
	view = kzalloc(sizeof(*view), GFP_KERNEL);
	if (!view)
		return -ENOMEM;
	view->file = get_file(file);
 view->group=xa_load(&shared->by_mapping,(unsigned long)file->f_mapping);
 if (!view->group) {
  view->group=kzalloc(sizeof(*view->group),GFP_KERNEL);
  if (!view->group) { fput(view->file);kfree(view);return -ENOMEM; }
  INIT_LIST_HEAD(&view->group->exports);
  error=xa_insert(&shared->by_mapping,(unsigned long)file->f_mapping,view->group,GFP_KERNEL);
  if (error) { kfree(view->group);fput(view->file);kfree(view);return error; }
 }

	view->offset = offset;
	view->count = count;
	view->flags = flags;
	view->charged = charged;
	view->writable = flags & 1;
	if (view->writable) {
		view->dirty_words = DIV_ROUND_UP_ULL(count, 64);
		view->dirty = kvcalloc(view->dirty_words, sizeof(*view->dirty), GFP_KERNEL);
		if (!view->dirty) {
			error = -ENOMEM;
			goto fail;
		}
	}
	error = fs_shared_prepare(view, vm_flags);
	if (error)
		goto fail;
	view->restore_evictable = !mapping_unevictable(file->f_mapping);
	list_for_each_entry(other, &view->group->exports, mapping_link) {
			view->restore_evictable = other->restore_evictable;
			break;
		}
	/* Like Linux GEM's shmem pinning, a folio reference alone is not enough:
	 * swapout must not replace the cache entry underneath native CPU aliases.
	 * The service owns all filesystem/shmem mutation in this sandbox. Restore
	 * only our own unevictable grant, after the last export of this inode.
	 */
	mapping_set_unevictable(file->f_mapping);
	if (view->writable) {
		/* This is the same write/seal admission counter used by Linux mmap.
		 * It must stay elevated until all native aliases have retired.
		 */
		error = mapping_map_writable(file->f_mapping);
		if (error)
			goto fail;
	}
	error = fs_shared_pages(view, &view->folios, &view->pages);
	if (!error)
		error = shared->ops.create(shared->ops.context, client, sequence,
			view->pages, view->count, flags, &view->id);
	if (error) {
		if (view->writable)
			mapping_unmap_writable(file->f_mapping);
		goto fail;
	}
	shared->bytes += charged;
	list_add_tail(&view->link, &shared->exports);
	list_add_tail(&view->mapping_link, &view->group->exports);
	fs_shared_dirty_all(view);
	*id = view->id;
	*file_size = i_size_read(file_inode(file));
	file_accessed(file);
	return 0;
fail:
 fs_shared_restore_eviction(shared, view);
 if (view->group && list_empty(&view->group->exports)) {
  xa_erase(&shared->by_mapping,(unsigned long)view->file->f_mapping);
  kfree(view->group);
 }
	fs_shared_put_pages(view->folios, view->count);
	kvfree(view->pages);
	kvfree(view->dirty);
	fs_shared_unprepare(view);
	fput(view->file);
	kfree(view);
	return error;
}
