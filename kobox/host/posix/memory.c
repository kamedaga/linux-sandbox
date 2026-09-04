/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_uint_fast64_t backing_sequence;

static int valid_range(size_t total, size_t offset, size_t size)
{
	return size && offset <= total && size <= total - offset;
}

static int page_aligned(size_t value)
{
	long page_size = sysconf(_SC_PAGESIZE);

	return page_size > 0 && value % (size_t)page_size == 0;
}

static int valid_file_offset(size_t value)
{
	off_t converted = (off_t)value;

	return converted >= 0 && (size_t)converted == value;
}

static int native_protection(unsigned int protection, int *native_out)
{
	const unsigned int known = KOBOX_POSIX_MEMORY_READ |
		KOBOX_POSIX_MEMORY_WRITE | KOBOX_POSIX_MEMORY_EXECUTE;
	int native = PROT_NONE;

	if (!native_out || protection & ~known)
		return EINVAL;
	if (protection & KOBOX_POSIX_MEMORY_READ)
		native |= PROT_READ;
	if (protection & KOBOX_POSIX_MEMORY_WRITE)
		native |= PROT_WRITE;
	if (protection & KOBOX_POSIX_MEMORY_EXECUTE)
		native |= PROT_EXEC;
	*native_out = native;
	return 0;
}

int kobox_posix_memory_map(
	size_t size,
	unsigned int protection,
	void **address_out)
{
	void *address;
	int native;
	int status;

	if (!size || !address_out)
		return EINVAL;
	status = native_protection(protection, &native);
	if (status)
		return status;
	address = mmap(NULL, size, native, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (address == MAP_FAILED)
		return errno;
	*address_out = address;
	return 0;
}

int kobox_posix_memory_protect(
	void *address,
	size_t size,
	unsigned int protection)
{
	int native;
	int status;

	if (!address || !size)
		return EINVAL;
	status = native_protection(protection, &native);
	if (status)
		return status;
	return mprotect(address, size, native) == 0 ? 0 : errno;
}

int kobox_posix_memory_unmap(void *address, size_t size)
{
	if (!address || !size)
		return EINVAL;
	return munmap(address, size) == 0 ? 0 : errno;
}

int kobox_posix_memory_backing_init(
	struct kobox_posix_memory_backing *backing,
	size_t size)
{
	char name[80];
	uint_fast64_t sequence;
	int descriptor;
	int status;

	if (!backing || !size || !page_aligned(size) ||
	    !valid_file_offset(size) || backing->initialized)
		return EINVAL;
	sequence = atomic_fetch_add_explicit(
		&backing_sequence, 1, memory_order_relaxed);
	status = snprintf(name, sizeof(name), "/kobox2-%ld-%llu",
		(long)getpid(), (unsigned long long)sequence);
	if (status < 0 || (size_t)status >= sizeof(name))
		return EOVERFLOW;
	descriptor = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (descriptor < 0)
		return errno;
	status = shm_unlink(name) == 0 ? 0 : errno;
	if (!status && ftruncate(descriptor, (off_t)size) != 0)
		status = errno;
	if (status) {
		(void)close(descriptor);
		return status;
	}
	backing->descriptor = descriptor;
	backing->size = size;
	backing->initialized = true;
	return 0;
}

int kobox_posix_memory_backing_destroy(
	struct kobox_posix_memory_backing *backing)
{
	int status;

	if (!backing || !backing->initialized)
		return EINVAL;
	status = close(backing->descriptor) == 0 ? 0 : errno;
	if (status)
		return status;
	*backing = (struct kobox_posix_memory_backing){0};
	return 0;
}

int kobox_posix_memory_window_init(
	struct kobox_posix_memory_window *window,
	size_t size)
{
	void *address;

	if (!window || !size || !page_aligned(size) || window->initialized)
		return EINVAL;
	address = mmap(NULL, size, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (address == MAP_FAILED)
		return errno;
	window->address = address;
	window->size = size;
	window->initialized = true;
	return 0;
}

int kobox_posix_memory_window_map(
	struct kobox_posix_memory_window *window,
	size_t window_offset,
	struct kobox_posix_memory_backing *backing,
	size_t backing_offset,
	size_t size,
	unsigned int protection,
	void **address_out)
{
	void *requested;
	void *address;
	int native;
	int status;

	if (!window || !window->initialized || !backing ||
	    !backing->initialized || !address_out ||
	    !valid_range(window->size, window_offset, size) ||
	    !valid_range(backing->size, backing_offset, size) ||
	    !page_aligned(window_offset) || !page_aligned(backing_offset) ||
	    !page_aligned(size) || !valid_file_offset(backing_offset))
		return EINVAL;
	status = native_protection(protection, &native);
	if (status)
		return status;
	requested = (unsigned char *)window->address + window_offset;
	address = mmap(requested, size, native, MAP_SHARED | MAP_FIXED,
		       backing->descriptor, (off_t)backing_offset);
	if (address == MAP_FAILED)
		return errno;
	if (address != requested)
		return EIO;
	*address_out = address;
	return 0;
}

int kobox_posix_memory_window_reset(
	struct kobox_posix_memory_window *window,
	size_t window_offset,
	size_t size)
{
	void *requested;
	void *address;

	if (!window || !window->initialized ||
	    !valid_range(window->size, window_offset, size) ||
	    !page_aligned(window_offset) || !page_aligned(size))
		return EINVAL;
	requested = (unsigned char *)window->address + window_offset;
	address = mmap(requested, size, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (address == MAP_FAILED)
		return errno;
	return address == requested ? 0 : EIO;
}

int kobox_posix_memory_window_destroy(
	struct kobox_posix_memory_window *window)
{
	int status;

	if (!window || !window->initialized)
		return EINVAL;
	status = munmap(window->address, window->size) == 0 ? 0 : errno;
	if (status)
		return status;
	*window = (struct kobox_posix_memory_window){0};
	return 0;
}
