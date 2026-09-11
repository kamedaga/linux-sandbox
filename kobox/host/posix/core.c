// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "core.h"
#include "../../arch/x86_64/tls.h"

#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>

struct kobox_posix_core {
	struct kobox_boot_core boot;
	void *library;
	void *base;
	size_t tls_module;
	size_t tls_size;
	void *(*tls_resolve)(const struct kobox_x86_tls_index *index);
};

static int collect_tls(struct dl_phdr_info *info, size_t size, void *argument)
{
	struct kobox_posix_core *core = argument;
	size_t i;

	(void)size;
	if ((void *)info->dlpi_addr != core->base)
		return 0;
	for (i = 0; i < info->dlpi_phnum; i++)
		if (info->dlpi_phdr[i].p_type == PT_TLS)
			core->tls_size = info->dlpi_phdr[i].p_memsz;
	return 1;
}

static void *tls_address(void *context, unsigned long module,
			 unsigned long offset)
{
	struct kobox_posix_core *core = context;
	const struct kobox_x86_tls_index index = {module, offset};

	if (module != core->tls_module || offset >= core->tls_size)
		return NULL;
	return core->tls_resolve(&index);
}

static void *lookup(void *context, const char *name)
{
	struct kobox_posix_core *core = context;

	return dlsym(core->library, name);
}

int kobox_posix_core_open(const char *path, struct kobox_posix_core **out)
{
	struct kobox_runtime_host runtime;
	struct kobox_posix_core *core;
	struct link_map *map;
	void *resolver;
	int error = ENOEXEC;

	if (!out)
		return EINVAL;
	*out = NULL;
	if (!path)
		return EINVAL;
	core = calloc(1, sizeof(*core));
	if (!core)
		return ENOMEM;
	core->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!core->library)
		goto fail;
	if (dlinfo(core->library, RTLD_DI_LINKMAP, &map) || !map->l_addr ||
	    dlinfo(core->library, RTLD_DI_TLS_MODID, &core->tls_module) ||
	    !core->tls_module)
		goto fail;
	core->base = (void *)map->l_addr;
	dl_iterate_phdr(collect_tls, core);
	if (!core->tls_size)
		goto fail;
	resolver = dlsym(RTLD_DEFAULT, "__tls_get_addr");
	if (!resolver)
		goto fail;
	memcpy(&core->tls_resolve, &resolver, sizeof(core->tls_resolve));
	runtime = (struct kobox_runtime_host) {
		.size = sizeof(runtime), .context = core,
		.tls_address = tls_address,
	};
	if (kobox_boot_core_prepare(&core->boot, core, lookup, &runtime) !=
	    KOBOX_BOOT_CORE_OK)
		goto fail;
	*out = core;
	return 0;
fail:
	if (core->library)
		dlclose(core->library);
	free(core);
	return error;
}

int kobox_posix_core_close(struct kobox_posix_core **pointer)
{
	struct kobox_posix_core *core;

	if (!pointer || !*pointer)
		return EINVAL;
	core = *pointer;
	if (core->boot.started)
		return EBUSY;
	if (dlclose(core->library))
		return EIO;
	*pointer = NULL;
	free(core);
	return 0;
}

struct kobox_boot_core *kobox_posix_core_boot(struct kobox_posix_core *core)
{
	return core ? &core->boot : NULL;
}

void *kobox_posix_core_library(struct kobox_posix_core *core)
{
	return core ? core->library : NULL;
}

void *kobox_posix_core_base(struct kobox_posix_core *core)
{
	return core ? core->base : NULL;
}
