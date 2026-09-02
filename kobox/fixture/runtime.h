/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_FIXTURE_RUNTIME_H
#define KOBOX_FIXTURE_RUNTIME_H

#include "fixture.h"

struct kobox_fixture_runtime {
	void *core_handle;
	const struct kobox_fixture_core_ops *ops;
};

int kobox_fixture_runtime_open(struct kobox_fixture_runtime *runtime,
			       const char *core_path);
int kobox_fixture_runtime_run(struct kobox_fixture_runtime *runtime,
			      const char *module_path, uint64_t *result_out);
void kobox_fixture_runtime_close(struct kobox_fixture_runtime *runtime);

#endif
