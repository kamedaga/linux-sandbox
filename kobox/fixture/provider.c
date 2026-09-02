// SPDX-License-Identifier: GPL-2.0-only

#include "fixture.h"

#include <linux/compiler_attributes.h>

static const char fixture_provider_license[]
	__section(".modinfo") __used __aligned(1) = "license=GPL";
static const char fixture_provider_name[]
	__section(".modinfo") __used __aligned(1) =
		"name=fixture_provider";
static const char fixture_provider_dependency[]
	__section(".modinfo") __used __aligned(1) =
		"depends=fixture_core";
static const char fixture_provider_interface[]
	__section(".modinfo") __used __aligned(1) =
		"kobox_interface=dev";

static const struct kobox_module_context *provider_context;
static const struct kobox_fixture_core_ops *provider_ops;
static void *provider_lock;
static uint64_t provider_total;

static int provider_context_valid(const struct kobox_module_context *context)
{
	return context && context == provider_context &&
	       context->node_id == KOBOX_FIXTURE_PROVIDER_NODE_ID &&
	       context->core_operations == provider_ops;
}

__attribute__((visibility("default")))
int kobox_fixture_provider_init(const struct kobox_module_context *context)
{
	const struct kobox_fixture_core_ops *ops;
	uint32_t state;
	size_t count;

	if (!context || context->node_id != KOBOX_FIXTURE_PROVIDER_NODE_ID ||
	    provider_context || !context->core_operations ||
	    !context->runtime_operations)
		return -1;
	ops = context->core_operations;
	if (ops->size != sizeof(*ops) || !ops->mutex_create ||
	    !ops->mutex_destroy || !ops->mutex_lock || !ops->mutex_unlock ||
	    !ops->lifecycle_record ||
	    context->runtime_operations->resource_count(
		    context, KOBOX_FIXTURE_RESOURCE_SLOT_ID, &state, &count) !=
		    KOBOX_MODULE_RESOURCE_NOT_VISIBLE ||
	    ops->mutex_create(&provider_lock))
		return -1;
	provider_context = context;
	provider_ops = ops;
	provider_total = 0;
	if (ops->lifecycle_record(KOBOX_FIXTURE_PROVIDER_NODE_ID,
				  KOBOX_FIXTURE_LIFECYCLE_INIT)) {
		provider_context = NULL;
		provider_ops = NULL;
		ops->mutex_destroy(provider_lock);
		provider_lock = NULL;
		return -1;
	}
	return 0;
}

__attribute__((visibility("default")))
int kobox_fixture_provider_add(uint64_t value, uint64_t *total_out)
{
	if (!total_out || !provider_context || !provider_ops ||
	    provider_ops->mutex_lock(provider_lock))
		return -1;
	provider_total += value;
	*total_out = provider_total;
	return provider_ops->mutex_unlock(provider_lock);
}

__attribute__((visibility("default")))
int kobox_fixture_provider_quiesce(const struct kobox_module_context *context)
{
	return provider_context_valid(context) &&
		       !provider_ops->lifecycle_record(
			       KOBOX_FIXTURE_PROVIDER_NODE_ID,
			       KOBOX_FIXTURE_LIFECYCLE_QUIESCE)
		       ? 0
		       : -1;
}

__attribute__((visibility("default")))
int kobox_fixture_provider_cleanup(const struct kobox_module_context *context)
{
	if (!provider_context_valid(context) ||
	    provider_ops->lifecycle_record(KOBOX_FIXTURE_PROVIDER_NODE_ID,
					   KOBOX_FIXTURE_LIFECYCLE_CLEANUP))
		return -1;
	provider_ops->mutex_destroy(provider_lock);
	provider_lock = NULL;
	provider_ops = NULL;
	provider_context = NULL;
	return 0;
}
