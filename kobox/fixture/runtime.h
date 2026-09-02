/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_FIXTURE_RUNTIME_H
#define KOBOX_FIXTURE_RUNTIME_H

#include "fixture.h"

#include "../loader/closure_loader.h"

struct kobox_fixture_runtime {
	struct kobox_closure_loader *closure;
	kobox_fixture_module_run_fn run;
	uint32_t root_node_id;
};

int kobox_fixture_runtime_open(
	struct kobox_fixture_runtime *runtime,
	const struct kobox_closure_loader_config *config);
int kobox_fixture_runtime_run(struct kobox_fixture_runtime *runtime,
			      uint64_t *result_out);
int kobox_fixture_runtime_quiesce(struct kobox_fixture_runtime *runtime);
int kobox_fixture_runtime_close(struct kobox_fixture_runtime *runtime);

#endif
