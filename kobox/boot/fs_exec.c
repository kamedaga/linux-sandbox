// SPDX-License-Identifier: GPL-2.0-only
#include "fs_exec.h"
#include <linux/file.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/xarray.h>

struct kobox_linux_exec_file {
	struct file *file;
	u64 client;
	refcount_t references;
};
struct kobox_linux_exec_table {
	struct xarray files;
	struct mutex lock;
	unsigned long next;
#ifdef KOBOX_RUNTIME_GATES
	enum kobox_exec_gate_failure fail_next;
#endif
};
#ifdef KOBOX_RUNTIME_GATES
void kobox_linux_exec_gate_fail_next(struct kobox_linux_exec_table *table,
				   enum kobox_exec_gate_failure failure)
{
	mutex_lock(&table->lock);
	table->fail_next = failure;
	mutex_unlock(&table->lock);
}
#endif

struct kobox_linux_exec_table *kobox_linux_exec_create(void)
{
	struct kobox_linux_exec_table *table = kzalloc(sizeof(*table), GFP_KERNEL);

	if (!table)
		return ERR_PTR(-ENOMEM);
	xa_init(&table->files);
	mutex_init(&table->lock);
	table->next = 1;
	return table;
}

void kobox_linux_exec_put(struct kobox_linux_exec_file *file)
{
	if (!file || !refcount_dec_and_test(&file->references))
		return;
	/* open_exec adds one deny-write reference, not one per table lookup.
	 * Release it exactly once after table and in-flight users are gone.
	 * Ordinary client fput/dup cannot participate in this private lifetime.
	 */
	exe_file_allow_write_access(file->file);
	fput(file->file);
	kfree(file);
}

void kobox_linux_exec_destroy(struct kobox_linux_exec_table *table)
{
	struct kobox_linux_exec_file *file;
	unsigned long index;

	if (!table)
		return;
	xa_for_each(&table->files, index, file)
		kobox_linux_exec_put(file);
	xa_destroy(&table->files);
	kfree(table);
}

int kobox_linux_exec_open(struct kobox_linux_exec_table *table,
	struct kobox_linux_fs_port *port, const struct cred *cred, u64 client,
	u64 directory, const char *name, u64 *handle)
{
	struct kobox_linux_exec_file *entry;
	int error;

	if (!table || !port || !cred || !client || !name || !handle)
		return -EINVAL;
#ifdef KOBOX_RUNTIME_GATES
	mutex_lock(&table->lock);
	if (table->fail_next == KOBOX_EXEC_GATE_ALLOC) {
		table->fail_next = 0;
		mutex_unlock(&table->lock);
		return -ENOMEM;
	}
	mutex_unlock(&table->lock);
#endif
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->file = kobox_linux_fs_open_exec(port, cred, directory, name);
	if (IS_ERR(entry->file)) {
		error = PTR_ERR(entry->file);
		kfree(entry);
		return error;
	}
	entry->client = client;
	refcount_set(&entry->references, 1);
	mutex_lock(&table->lock);
#ifdef KOBOX_RUNTIME_GATES
	if (table->fail_next == KOBOX_EXEC_GATE_INSTALL) {
		table->fail_next = 0;
		error = -ENOMEM;
	} else
#endif
	error = table->next ? xa_insert(&table->files, table->next, entry,
					GFP_KERNEL) : -EOVERFLOW;
	if (!error)
		*handle = table->next++;
	mutex_unlock(&table->lock);
	if (error)
		kobox_linux_exec_put(entry);
	return error;
}

struct kobox_linux_exec_file *kobox_linux_exec_get(
	struct kobox_linux_exec_table *table, u64 client, u64 handle)
{
	struct kobox_linux_exec_file *file;

	if (!table || !client || !handle || handle > ULONG_MAX)
		return ERR_PTR(-EBADF);
	mutex_lock(&table->lock);
	file = xa_load(&table->files, handle);
	if (!file || file->client != client)
		file = ERR_PTR(-EBADF);
	else
		refcount_inc(&file->references);
	mutex_unlock(&table->lock);
	return file;
}

int kobox_linux_exec_close(struct kobox_linux_exec_table *table, u64 client, u64 handle)
{
	struct kobox_linux_exec_file *file;

	if (!table || !client || !handle || handle > ULONG_MAX)
		return -EBADF;
	mutex_lock(&table->lock);
	file = xa_load(&table->files, handle);
	if (!file || file->client != client) {
		mutex_unlock(&table->lock);
		return -EBADF;
	}
	xa_erase(&table->files, handle);
	mutex_unlock(&table->lock);
	kobox_linux_exec_put(file);
	return 0;
}

void kobox_linux_exec_release_client(struct kobox_linux_exec_table *table, u64 client)
{
	struct kobox_linux_exec_file *file;
	unsigned long index = 0;

	/* Drop one detached entry at a time, outside the lock. This teardown
	 * allocates nothing and cannot strand a deny-write guard on ENOMEM.
	 */
	for (;;) {
		mutex_lock(&table->lock);
		file = xa_find(&table->files, &index, ULONG_MAX, XA_PRESENT);
		while (file && file->client != client) {
			if (index == ULONG_MAX) { file = NULL; break; }
			++index;
			file = xa_find(&table->files, &index, ULONG_MAX, XA_PRESENT);
		}
		if (file)
			xa_erase(&table->files, index);
		mutex_unlock(&table->lock);
		if (!file)
			break;
		kobox_linux_exec_put(file);
	}
}

ssize_t kobox_linux_exec_pread(struct kobox_linux_exec_file *file,
	const struct cred *cred, void *buffer, size_t length, loff_t offset)
{
	const struct cred *saved;
	ssize_t result;

	if (!file || !cred || (!buffer && length) || offset < 0)
		return -EINVAL;
	saved = override_creds(cred);
	result = kernel_read(file->file, buffer, length, &offset);
	revert_creds(saved);
	return result;
}

int kobox_linux_exec_stat(struct kobox_linux_exec_file *file,
	const struct cred *cred, int flags, u32 mask, struct kstat *stat)
{
	return file ? kobox_linux_fs_exec_getattr(file->file, cred, flags, mask, stat) : -EBADF;
}
