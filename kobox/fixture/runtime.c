// SPDX-License-Identifier: GPL-2.0-only

#include "runtime.h"

#include <string.h>

#if defined(__clang__)
#define KOBOX_MANUAL_ELF_CALL __attribute__((no_sanitize("function")))
#else
#define KOBOX_MANUAL_ELF_CALL
#endif

KOBOX_MANUAL_ELF_CALL int kobox_fixture_runtime_open(
	struct kobox_fixture_runtime *runtime,
	const struct kobox_closure_loader_config *config)
{
	uintptr_t run_address;

	if (!runtime || !config)
		return -1;
	memset(runtime, 0, sizeof(*runtime));
	if (kobox_closure_loader_open(config, &runtime->closure) !=
		    KOBOX_CLOSURE_OK ||
	    kobox_closure_loader_root_symbol(
		    runtime->closure, "kobox_fixture_module_run",
		    sizeof("kobox_fixture_module_run") - 1,
		    KB2_CLOSURE_SYMBOL_FUNCTION, &runtime->root_node_id,
		    &run_address) != KOBOX_CLOSURE_OK ||
	    sizeof(run_address) != sizeof(runtime->run))
		goto fail;
	memcpy(&runtime->run, &run_address, sizeof(runtime->run));
	return 0;

fail:
	if (runtime->closure &&
	    kobox_closure_loader_quiesce(runtime->closure) == KOBOX_CLOSURE_OK)
		kobox_closure_loader_close(&runtime->closure);
	memset(runtime, 0, sizeof(*runtime));
	return -1;
}

KOBOX_MANUAL_ELF_CALL int kobox_fixture_runtime_run(
	struct kobox_fixture_runtime *runtime, uint64_t *result_out)
{
	if (!runtime || !runtime->closure || !runtime->run || !result_out)
		return -1;
	return runtime->run(result_out);
}

int kobox_fixture_runtime_quiesce(struct kobox_fixture_runtime *runtime)
{
	return runtime && runtime->closure &&
	       kobox_closure_loader_quiesce(runtime->closure) == KOBOX_CLOSURE_OK
		       ? 0
		       : -1;
}

int kobox_fixture_runtime_close(struct kobox_fixture_runtime *runtime)
{
	int result;

	if (!runtime || !runtime->closure)
		return -1;
	result = kobox_closure_loader_close(&runtime->closure) == KOBOX_CLOSURE_OK
			 ? 0
			 : -1;
	memset(runtime, 0, sizeof(*runtime));
	return result;
}
