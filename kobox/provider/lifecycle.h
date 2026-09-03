/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_PROVIDER_LIFECYCLE_H
#define KOBOX_PROVIDER_LIFECYCLE_H

#include "../runtime/module_context.h"

#include <stddef.h>
#include <stdint.h>

enum kobox_provider_state {
	KOBOX_PROVIDER_UNBOUND = 0,
	KOBOX_PROVIDER_BOUND,
	KOBOX_PROVIDER_INITIALIZING,
	KOBOX_PROVIDER_ACTIVE,
	KOBOX_PROVIDER_QUIESCING,
	KOBOX_PROVIDER_QUIESCED,
	KOBOX_PROVIDER_CLEANING,
	KOBOX_PROVIDER_CLEAN,
};

enum kobox_provider_lifecycle_status {
	KOBOX_PROVIDER_LIFECYCLE_OK = 0,
	KOBOX_PROVIDER_LIFECYCLE_INVALID_ARGUMENT,
	KOBOX_PROVIDER_LIFECYCLE_INVALID_TABLE,
	KOBOX_PROVIDER_LIFECYCLE_INVALID_STATE,
	KOBOX_PROVIDER_LIFECYCLE_ENTRY_FAILURE,
};

enum kobox_provider_lifecycle_phase {
	KOBOX_PROVIDER_PHASE_NONE = 0,
	KOBOX_PROVIDER_PHASE_INIT,
	KOBOX_PROVIDER_PHASE_QUIESCE,
	KOBOX_PROVIDER_PHASE_CLEANUP,
};

typedef int (*kobox_provider_step_fn)(
	const struct kobox_module_context *context);

struct kobox_provider_init_entry {
	uint32_t level;
	uint32_t link_order;
	const char *name;
	kobox_provider_step_fn init;
	kobox_provider_step_fn quiesce;
	kobox_provider_step_fn cleanup;
};

struct kobox_provider_lifecycle_failure {
	uint32_t phase;
	size_t entry_index;
	int entry_status;
};

struct kobox_provider_lifecycle {
	uint32_t state;
	uint32_t reserved;
	const struct kobox_provider_init_entry *entries;
	size_t entry_count;
	size_t initialized_count;
	const struct kobox_module_context *context;
	struct kobox_provider_lifecycle_failure failure;
};

enum kobox_provider_lifecycle_status kobox_provider_lifecycle_bind(
	struct kobox_provider_lifecycle *lifecycle,
	const struct kobox_provider_init_entry *entries, size_t entry_count);
enum kobox_provider_lifecycle_status kobox_provider_lifecycle_init(
	struct kobox_provider_lifecycle *lifecycle,
	const struct kobox_module_context *context);
enum kobox_provider_lifecycle_status kobox_provider_lifecycle_quiesce(
	struct kobox_provider_lifecycle *lifecycle,
	const struct kobox_module_context *context);
enum kobox_provider_lifecycle_status kobox_provider_lifecycle_cleanup(
	struct kobox_provider_lifecycle *lifecycle,
	const struct kobox_module_context *context);

#endif
