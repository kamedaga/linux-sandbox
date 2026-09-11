/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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
	int descriptor;
	int status;

	if (!backing || !size || !page_aligned(size) ||
	    !valid_file_offset(size) || backing->initialized)
		return EINVAL;
	descriptor = memfd_create("kobox2-ram", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (descriptor < 0)
		return errno;
	status = ftruncate(descriptor, (off_t)size) == 0 ? 0 : errno;
	/* Install before granting the FD. After revoke shrinks it to zero,
	 * no retained FD can grow this inode back into a live generation or
	 * add F_SEAL_SHRINK to take away the owner's revocation authority.
	 */
	if (!status && fcntl(descriptor, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SEAL) != 0)
		status = errno;
	if (status) {
		(void)close(descriptor);
		return status;
	}
	backing->descriptor = descriptor;
	backing->size = size;
	backing->initialized = true;
	backing->revoked = false;
	return 0;
}

int kobox_posix_memory_backing_revoke(
	struct kobox_posix_memory_backing *backing)
{
	struct stat state;
	int seals;

	if (!backing || !backing->initialized)
		return EINVAL;
	backing->revoked = true;
	seals = fcntl(backing->descriptor, F_GET_SEALS);
	if (seals < 0)
		return errno;
	if (!(seals & F_SEAL_GROW))
		return EPERM;
	/* Host shmem truncation invalidates shared and private/COW aliases,
	 * including remote processes. It also releases pages, so the caller
	 * must have stopped every DMA user before reaching this operation.
	 */
	if (ftruncate(backing->descriptor, 0))
		return errno;
	if (fstat(backing->descriptor, &state))
		return errno;
	return state.st_size == 0 ? 0 : EIO;
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
	if (backing->revoked)
		return ESTALE;
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
