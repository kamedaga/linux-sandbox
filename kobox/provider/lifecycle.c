// SPDX-License-Identifier: GPL-2.0-only

#include "lifecycle.h"

static int name_order(const char *left, const char *right)
{
	while (*left && *left == *right) {
		left++;
		right++;
	}
	return (unsigned char)*left - (unsigned char)*right;
}

static int entry_before_or_equal(const struct kobox_provider_init_entry *left,
				 const struct kobox_provider_init_entry *right)
{
	if (left->level != right->level)
		return left->level < right->level;
	if (left->link_order != right->link_order)
		return left->link_order < right->link_order;
	return name_order(left->name, right->name) < 0;
}

static void record_failure(struct kobox_provider_lifecycle *lifecycle,
			   uint32_t phase, size_t entry_index,
			   int entry_status)
{
	if (lifecycle->failure.phase != KOBOX_PROVIDER_PHASE_NONE)
		return;
	lifecycle->failure.phase = phase;
	lifecycle->failure.entry_index = entry_index;
	lifecycle->failure.entry_status = entry_status;
}

static void rollback_initialized(struct kobox_provider_lifecycle *lifecycle)
{
	size_t index;

	for (index = lifecycle->initialized_count; index > 0; index--) {
		int status = lifecycle->entries[index - 1].quiesce(
			lifecycle->context);

		if (status)
			record_failure(lifecycle, KOBOX_PROVIDER_PHASE_QUIESCE,
				       index - 1, status);
	}
	for (index = lifecycle->initialized_count; index > 0; index--) {
		int status = lifecycle->entries[index - 1].cleanup(
			lifecycle->context);

		if (status)
			record_failure(lifecycle, KOBOX_PROVIDER_PHASE_CLEANUP,
				       index - 1, status);
	}
	lifecycle->initialized_count = 0;
	lifecycle->state = KOBOX_PROVIDER_CLEAN;
}

enum kobox_provider_lifecycle_status kobox_provider_lifecycle_bind(
	struct kobox_provider_lifecycle *lifecycle,
	const struct kobox_provider_init_entry *entries, size_t entry_count)
{
	size_t index;

	if (!lifecycle || !entries || !entry_count)
		return KOBOX_PROVIDER_LIFECYCLE_INVALID_ARGUMENT;
	if (lifecycle->state != KOBOX_PROVIDER_UNBOUND)
		return KOBOX_PROVIDER_LIFECYCLE_INVALID_STATE;
	for (index = 0; index < entry_count; index++) {
		if (!entries[index].name || !entries[index].name[0] ||
		    !entries[index].init || !entries[index].quiesce ||
		    !entries[index].cleanup ||
		    (index && !entry_before_or_equal(&entries[index - 1],
						    &entries[index])))
			return KOBOX_PROVIDER_LIFECYCLE_INVALID_TABLE;
	}
	lifecycle->entries = entries;
	lifecycle->entry_count = entry_count;
	lifecycle->state = KOBOX_PROVIDER_BOUND;
	return KOBOX_PROVIDER_LIFECYCLE_OK;
}

enum kobox_provider_lifecycle_status kobox_provider_lifecycle_init(
	struct kobox_provider_lifecycle *lifecycle,
	const struct kobox_module_context *context)
{
	size_t index;

	if (!lifecycle || !context)
		return KOBOX_PROVIDER_LIFECYCLE_INVALID_ARGUMENT;
	if (lifecycle->state != KOBOX_PROVIDER_BOUND)
		return KOBOX_PROVIDER_LIFECYCLE_INVALID_STATE;
	lifecycle->context = context;
	lifecycle->failure = (struct kobox_provider_lifecycle_failure){ 0 };
	lifecycle->state = KOBOX_PROVIDER_INITIALIZING;
	for (index = 0; index < lifecycle->entry_count; index++) {
		int status = lifecycle->entries[index].init(context);

		if (status) {
			record_failure(lifecycle, KOBOX_PROVIDER_PHASE_INIT, index,
				       status);
			rollback_initialized(lifecycle);
			return KOBOX_PROVIDER_LIFECYCLE_ENTRY_FAILURE;
		}
		lifecycle->initialized_count++;
	}
	lifecycle->state = KOBOX_PROVIDER_ACTIVE;
	return KOBOX_PROVIDER_LIFECYCLE_OK;
}

enum kobox_provider_lifecycle_status kobox_provider_lifecycle_quiesce(
	struct kobox_provider_lifecycle *lifecycle,
	const struct kobox_module_context *context)
{
	enum kobox_provider_lifecycle_status result =
		KOBOX_PROVIDER_LIFECYCLE_OK;
	size_t index;

	if (!lifecycle || !context)
		return KOBOX_PROVIDER_LIFECYCLE_INVALID_ARGUMENT;
	if (lifecycle->state != KOBOX_PROVIDER_ACTIVE ||
	    lifecycle->context != context)
		return KOBOX_PROVIDER_LIFECYCLE_INVALID_STATE;
	lifecycle->failure = (struct kobox_provider_lifecycle_failure){ 0 };
	lifecycle->state = KOBOX_PROVIDER_QUIESCING;
	for (index = lifecycle->initialized_count; index > 0; index--) {
		int status = lifecycle->entries[index - 1].quiesce(context);

		if (!status)
			continue;
		record_failure(lifecycle, KOBOX_PROVIDER_PHASE_QUIESCE, index - 1,
			       status);
		result = KOBOX_PROVIDER_LIFECYCLE_ENTRY_FAILURE;
	}
	lifecycle->state = KOBOX_PROVIDER_QUIESCED;
	return result;
}

enum kobox_provider_lifecycle_status kobox_provider_lifecycle_cleanup(
	struct kobox_provider_lifecycle *lifecycle,
	const struct kobox_module_context *context)
{
	enum kobox_provider_lifecycle_status result =
		KOBOX_PROVIDER_LIFECYCLE_OK;
	size_t index;

	if (!lifecycle || !context)
		return KOBOX_PROVIDER_LIFECYCLE_INVALID_ARGUMENT;
	if (lifecycle->state != KOBOX_PROVIDER_QUIESCED ||
	    lifecycle->context != context)
		return KOBOX_PROVIDER_LIFECYCLE_INVALID_STATE;
	lifecycle->failure = (struct kobox_provider_lifecycle_failure){ 0 };
	lifecycle->state = KOBOX_PROVIDER_CLEANING;
	for (index = lifecycle->initialized_count; index > 0; index--) {
		int status = lifecycle->entries[index - 1].cleanup(context);

		if (!status)
			continue;
		record_failure(lifecycle, KOBOX_PROVIDER_PHASE_CLEANUP, index - 1,
			       status);
		result = KOBOX_PROVIDER_LIFECYCLE_ENTRY_FAILURE;
	}
	lifecycle->initialized_count = 0;
	lifecycle->state = KOBOX_PROVIDER_CLEAN;
	return result;
}
