// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "core.h"
#include "../../boot/image_layout.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

struct kobox_posix_core {
	struct kobox_boot_core boot;
	struct kobox_fixed_image image;
	void *file;
	size_t file_size;
	void *base;
};

static _Thread_local struct kobox_runtime_thread_state runtime_thread;

static int native_protection(unsigned int protection)
{
	int native = PROT_NONE;

	if (protection & KOBOX_FIXED_IMAGE_READ)
		native |= PROT_READ;
	if (protection & KOBOX_FIXED_IMAGE_WRITE)
		native |= PROT_WRITE;
	if (protection & KOBOX_FIXED_IMAGE_EXECUTE)
		native |= PROT_EXEC;
	return native;
}

static struct kobox_runtime_thread_state *thread_state(void *context)
{
	(void)context;
	return &runtime_thread;
}

static void *lookup(void *context, const char *name)
{
	struct kobox_posix_core *core = context;
	size_t offset;

	if (kobox_fixed_image_symbol(&core->image, name, &offset) !=
	    KOBOX_FIXED_IMAGE_OK)
		return NULL;
	return (unsigned char *)core->base + offset;
}

static int map_image(struct kobox_posix_core *core)
{
	void *base;
	unsigned int i;

	base = mmap((void *)(uintptr_t)KOBOX_CORE_LINK_BASE,
		    core->image.image_size, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (base == MAP_FAILED)
		return errno;
	if ((uintptr_t)base != KOBOX_CORE_LINK_BASE) {
		(void)munmap(base, core->image.image_size);
		return EADDRNOTAVAIL;
	}
	core->base = base;
	for (i = 0; i < core->image.segment_count; i++) {
		const struct kobox_fixed_image_segment *segment =
			&core->image.segments[i];
		void *address = (unsigned char *)base + segment->image_offset;

		if (mprotect(address, segment->mapping_size,
			     PROT_READ | PROT_WRITE))
			goto fail;
		memcpy(address, (const unsigned char *)core->file +
		       segment->file_offset, segment->file_size);
		memset((unsigned char *)address + segment->file_size, 0,
		       segment->memory_size - segment->file_size);
		if (mprotect(address, segment->mapping_size,
			     native_protection(segment->protection)))
			goto fail;
	}
	return 0;
fail:
	{
		int status = errno;

		(void)munmap(base, core->image.image_size);
		core->base = NULL;
		return status;
	}
}

int kobox_posix_core_open(const char *path, struct kobox_posix_core **out)
{
	struct kobox_runtime_host runtime;
	struct kobox_posix_core *core;
	struct stat state;
	long page_size;
	int descriptor = -1;
	int status = ENOEXEC;

	if (!out)
		return EINVAL;
	*out = NULL;
	if (!path)
		return EINVAL;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return ENOTSUP;
	core = calloc(1, sizeof(*core));
	if (!core)
		return ENOMEM;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		status = errno;
		goto fail;
	}
	if (fstat(descriptor, &state)) {
		status = errno;
		goto fail;
	}
	if (state.st_size <= 0 || (uintmax_t)state.st_size > SIZE_MAX)
		goto fail;
	core->file_size = (size_t)state.st_size;
	core->file = mmap(NULL, core->file_size, PROT_READ, MAP_PRIVATE,
			  descriptor, 0);
	if (core->file == MAP_FAILED) {
		core->file = NULL;
		status = errno;
		goto fail;
	}
	if (kobox_fixed_image_open(core->file, core->file_size,
				   (size_t)page_size, &core->image) !=
	    KOBOX_FIXED_IMAGE_OK)
		goto fail;
	status = map_image(core);
	if (status)
		goto fail;
	runtime = (struct kobox_runtime_host) {
		.size = sizeof(runtime),
		.context = core,
		.thread_state = thread_state,
	};
	if (kobox_boot_core_prepare(&core->boot, core, lookup, &runtime) !=
	    KOBOX_BOOT_CORE_OK) {
		status = ENOEXEC;
		goto fail;
	}
	(void)close(descriptor);
	*out = core;
	return 0;
fail:
	if (core->base)
		(void)munmap(core->base, core->image.image_size);
	if (core->file)
		(void)munmap(core->file, core->file_size);
	if (descriptor >= 0)
		(void)close(descriptor);
	free(core);
	return status;
}

int kobox_posix_core_close(struct kobox_posix_core **pointer)
{
	struct kobox_posix_core *core;
	int status;

	if (!pointer || !*pointer)
		return EINVAL;
	core = *pointer;
	if (core->boot.started)
		return EBUSY;
	status = munmap(core->base, core->image.image_size) ? errno : 0;
	if (!status && munmap(core->file, core->file_size))
		status = errno;
	if (status)
		return status;
	*pointer = NULL;
	free(core);
	return 0;
}

struct kobox_boot_core *kobox_posix_core_boot(struct kobox_posix_core *core)
{
	return core ? &core->boot : NULL;
}

const struct kobox_fixed_image *kobox_posix_core_image(
	struct kobox_posix_core *core)
{
	return core ? &core->image : NULL;
}

void *kobox_posix_core_base(struct kobox_posix_core *core)
{
	return core ? core->base : NULL;
}
