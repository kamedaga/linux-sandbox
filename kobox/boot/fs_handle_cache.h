/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_FS_HANDLE_CACHE_H
#define KOBOX_FS_HANDLE_CACHE_H

/* Included after the xarray API; every access holds the owning table mutex.
 * Keep the latest file outside the tree so open/close does not allocate and
 * retire an empty leaf for each monotonically increasing handle. This is
 * storage reuse, not handle reuse or a limit: an occupied slot spills into
 * the ordinary tree, transferring its existing ownership reference. */
struct kobox_fs_handle_cache {
	struct file *file;
	unsigned long handle;
};

static inline struct file *kobox_fs_handle_load(struct kobox_fs_handle_cache *cache,
	struct xarray *files, unsigned long handle)
{
	if (cache->file && cache->handle == handle)
		return cache->file;
	return xa_load(files, handle);
}

static inline int kobox_fs_handle_insert(struct kobox_fs_handle_cache *cache,
	struct xarray *files, unsigned long handle, struct file *file)
{
	int error;

	if (cache->file) {
		error = xa_insert(files, cache->handle, cache->file, GFP_KERNEL);
		if (error)
			return error;
	}
	cache->handle = handle;
	cache->file = file;
	return 0;
}

static inline struct file *kobox_fs_handle_erase(struct kobox_fs_handle_cache *cache,
	struct xarray *files, unsigned long handle)
{
	struct file *file;

	if (cache->file && cache->handle == handle) {
		file = cache->file;
		cache->file = NULL;
		cache->handle = 0;
		return file;
	}
	return xa_erase(files, handle);
}
#endif
