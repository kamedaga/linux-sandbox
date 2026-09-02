// SPDX-License-Identifier: GPL-2.0-only

#include "runtime.h"

#include "../loader/elf64_loader.h"

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

static int core_identity_valid(const struct kobox_fixture_core_ops *ops)
{
	return ops && ops->size == sizeof(*ops) &&
	       ops->identity[0] == KOBOX_FIXTURE_CORE_IDENTITY_0 &&
	       ops->identity[1] == KOBOX_FIXTURE_CORE_IDENTITY_1 &&
	       ops->identity[2] == KOBOX_FIXTURE_CORE_IDENTITY_2 &&
	       ops->identity[3] == KOBOX_FIXTURE_CORE_IDENTITY_3;
}

int kobox_fixture_runtime_open(struct kobox_fixture_runtime *runtime,
			       const char *core_path)
{
	kobox_fixture_get_core_ops_fn get_ops;
	void *symbol;

	if (!runtime || !core_path)
		return -1;
	memset(runtime, 0, sizeof(*runtime));
	runtime->core_handle = dlopen(core_path, RTLD_NOW | RTLD_GLOBAL);
	if (!runtime->core_handle)
		return -1;
	symbol = dlsym(runtime->core_handle, "kobox_fixture_core_get_ops");
	if (!symbol || sizeof(symbol) != sizeof(get_ops))
		goto fail;
	memcpy(&get_ops, &symbol, sizeof(get_ops));
	runtime->ops = get_ops();
	if (!core_identity_valid(runtime->ops))
		goto fail;
	return 0;

fail:
	dlclose(runtime->core_handle);
	memset(runtime, 0, sizeof(*runtime));
	return -1;
}

int kobox_fixture_runtime_run(struct kobox_fixture_runtime *runtime,
			      const char *module_path, uint64_t *result_out)
{
	struct kobox_elf64_module module;
	kobox_fixture_module_init_fn module_init;
	kobox_fixture_module_exit_fn module_exit;
	uintptr_t init_address;
	uintptr_t exit_address;
	int result = -1;

	if (!runtime || !runtime->core_handle || !runtime->ops || !module_path ||
	    !result_out || sizeof(init_address) != sizeof(module_init) ||
	    sizeof(exit_address) != sizeof(module_exit))
		return -1;
	*result_out = 0;
	if (kobox_elf64_module_load(module_path, &module))
		return -1;
	init_address = module.init_address;
	exit_address = module.exit_address;
	memcpy(&module_init, &init_address, sizeof(module_init));
	memcpy(&module_exit, &exit_address, sizeof(module_exit));
	if (!module_init(result_out) && !module_exit())
		result = 0;
	kobox_elf64_module_unload(&module);
	return result;
}

void kobox_fixture_runtime_close(struct kobox_fixture_runtime *runtime)
{
	if (!runtime)
		return;
	if (runtime->core_handle)
		dlclose(runtime->core_handle);
	memset(runtime, 0, sizeof(*runtime));
}
