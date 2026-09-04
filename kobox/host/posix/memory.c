/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "host.h"

#include <errno.h>
#include <sys/mman.h>

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
