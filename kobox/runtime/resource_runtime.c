// SPDX-License-Identifier: GPL-2.0-only

#include "resource_runtime.h"

#include <stdlib.h>
#include <string.h>

struct registry_object {
	uint32_t slot_id;
	uint32_t resource_type;
	uint64_t object_id;
	uint64_t granted_rights;
	void *native_object;
};

struct view_slot {
	uint32_t slot_id;
	uint32_t state;
	size_t object_start;
	size_t object_count;
};

struct node_view {
	const struct kobox_resource_runtime *runtime;
	uint32_t node_id;
	struct view_slot *slots;
	size_t slot_count;
};

struct kobox_resource_runtime {
	uint64_t generation;
	struct registry_object *objects;
	size_t object_count;
	struct node_view *views;
	size_t view_count;
	kobox_resource_release_fn release_object;
	void *object_context;
};

static size_t find_view_index(const struct kobox_resource_runtime *runtime,
			      uint32_t node_id)
{
	size_t left = 0;
	size_t right = runtime->view_count;

	while (left < right) {
		size_t middle = left + (right - left) / 2;

		if (runtime->views[middle].node_id == node_id)
			return middle;
		if (runtime->views[middle].node_id < node_id)
			left = middle + 1;
		else
			right = middle;
	}
	return SIZE_MAX;
}

static const struct view_slot *find_view_slot(const struct node_view *view,
					      uint32_t slot_id)
{
	size_t left = 0;
	size_t right = view->slot_count;

	while (left < right) {
		size_t middle = left + (right - left) / 2;

		if (view->slots[middle].slot_id == slot_id)
			return &view->slots[middle];
		if (view->slots[middle].slot_id < slot_id)
			left = middle + 1;
		else
			right = middle;
	}
	return NULL;
}

static int module_context_valid(const struct kobox_module_context *context,
				const struct node_view **view_out)
{
	static const uint8_t identity[KOBOX_MODULE_INTERFACE_IDENTITY_SIZE] =
		KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER;
	const struct node_view *view;

	if (!context || context->size != sizeof(*context) ||
	    memcmp(context->identity, identity, sizeof(identity)) ||
	    context->runtime_operations != kobox_resource_runtime_operations() ||
	    !context->resource_view)
		return 0;
	view = context->resource_view;
	if (!view->runtime || view->node_id != context->node_id ||
	    view->runtime->generation != context->generation)
		return 0;
	*view_out = view;
	return 1;
}

static int module_resource_count(const struct kobox_module_context *context,
				 uint32_t slot_id, uint32_t *state_out,
				 size_t *count_out)
{
	const struct node_view *view;
	const struct view_slot *slot;

	if (!state_out || !count_out || !module_context_valid(context, &view))
		return KOBOX_MODULE_RESOURCE_INVALID_ARGUMENT;
	*state_out = 0;
	*count_out = 0;
	slot = find_view_slot(view, slot_id);
	if (!slot)
		return KOBOX_MODULE_RESOURCE_NOT_VISIBLE;
	*state_out = slot->state;
	*count_out = slot->object_count;
	return KOBOX_MODULE_RESOURCE_OK;
}

static int module_resource_acquire(
	const struct kobox_module_context *context, uint32_t slot_id,
	size_t object_index, uint64_t required_rights,
	struct kobox_module_resource_handle *handle_out)
{
	const struct kobox_resource_runtime *runtime;
	const struct registry_object *object;
	const struct node_view *view;
	const struct view_slot *slot;

	if (!handle_out || !module_context_valid(context, &view))
		return KOBOX_MODULE_RESOURCE_INVALID_ARGUMENT;
	*handle_out = (struct kobox_module_resource_handle){ 0 };
	slot = find_view_slot(view, slot_id);
	if (!slot)
		return KOBOX_MODULE_RESOURCE_NOT_VISIBLE;
	if (slot->state == KOBOX_MODULE_RESOURCE_ABSENT_STATE)
		return KOBOX_MODULE_RESOURCE_ABSENT;
	if (object_index >= slot->object_count)
		return KOBOX_MODULE_RESOURCE_INVALID_ARGUMENT;
	runtime = view->runtime;
	object = &runtime->objects[slot->object_start + object_index];
	if ((required_rights & ~object->granted_rights) != 0)
		return KOBOX_MODULE_RESOURCE_RIGHTS;
	handle_out->generation = runtime->generation;
	handle_out->object_id = object->object_id;
	return KOBOX_MODULE_RESOURCE_OK;
}

static int module_resource_info(
	const struct kobox_module_context *context,
	struct kobox_module_resource_handle handle,
	struct kobox_module_resource_info *info_out)
{
	const struct kobox_resource_runtime *runtime;
	const struct node_view *view;
	size_t slot_index;

	if (!info_out || !module_context_valid(context, &view))
		return KOBOX_MODULE_RESOURCE_INVALID_ARGUMENT;
	memset(info_out, 0, sizeof(*info_out));
	if (handle.generation != context->generation)
		return KOBOX_MODULE_RESOURCE_STALE;
	runtime = view->runtime;
	for (slot_index = 0; slot_index < view->slot_count; slot_index++) {
		const struct view_slot *slot = &view->slots[slot_index];
		size_t object_index;

		for (object_index = 0; object_index < slot->object_count;
		     object_index++) {
			const struct registry_object *object =
				&runtime->objects[slot->object_start + object_index];

			if (object->object_id != handle.object_id)
				continue;
			info_out->resource_type = object->resource_type;
			info_out->reserved = 0;
			info_out->granted_rights = object->granted_rights;
			return KOBOX_MODULE_RESOURCE_OK;
		}
	}
	return KOBOX_MODULE_RESOURCE_NOT_VISIBLE;
}

static const struct kobox_module_runtime_operations runtime_operations = {
	.size = sizeof(runtime_operations),
	.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
	.resource_count = module_resource_count,
	.resource_acquire = module_resource_acquire,
	.resource_info = module_resource_info,
};

static void free_runtime(struct kobox_resource_runtime *runtime)
{
	size_t index;

	if (!runtime)
		return;
	for (index = runtime->object_count; index > 0; index--) {
		if (runtime->objects[index - 1].native_object)
			runtime->release_object(runtime->object_context,
						runtime->objects[index - 1].native_object);
	}
	for (index = 0; index < runtime->view_count; index++)
		free(runtime->views[index].slots);
	free(runtime->views);
	free(runtime->objects);
	free(runtime);
}

static enum kobox_resource_runtime_status copy_views(
	const struct kobox_resource_runtime_config *config,
	struct kobox_resource_runtime *runtime)
{
	size_t binding_count =
		kb2_closure_manifest_binding_count(config->manifest);
	size_t artifact_count =
		kb2_closure_manifest_artifact_count(config->manifest);
	size_t resource_count =
		kb2_closure_manifest_resource_count(config->manifest);
	uint32_t prior_slot = 0;
	uint32_t prior_node = 0;
	size_t index;

	runtime->view_count = artifact_count;
	runtime->views = calloc(artifact_count, sizeof(runtime->views[0]));
	if (artifact_count && !runtime->views)
		return KOBOX_RESOURCE_RUNTIME_NO_MEMORY;
	for (index = 0; index < artifact_count; index++) {
		kb2_closure_manifest_artifact_t artifact;

		if (kb2_closure_manifest_artifact(config->manifest, index,
						  &artifact) != KB2_PROTOCOL_OK)
			return KOBOX_RESOURCE_RUNTIME_MALFORMED;
		runtime->views[index].runtime = runtime;
		runtime->views[index].node_id = artifact.node_id;
	}
	for (index = 0; index < binding_count; index++) {
		kb2_closure_manifest_binding_t binding;
		size_t view_index;

		if (kb2_closure_manifest_binding(config->manifest, index, &binding) !=
			    KB2_PROTOCOL_OK ||
		    (index &&
		     (binding.slot_id < prior_slot ||
		      (binding.slot_id == prior_slot &&
		       binding.node_id <= prior_node))))
			return KOBOX_RESOURCE_RUNTIME_MALFORMED;
		view_index = find_view_index(runtime, binding.node_id);
		if (view_index == SIZE_MAX ||
		    runtime->views[view_index].slot_count == SIZE_MAX)
			return KOBOX_RESOURCE_RUNTIME_MALFORMED;
		runtime->views[view_index].slot_count++;
		prior_slot = binding.slot_id;
		prior_node = binding.node_id;
	}
	for (index = 0; index < artifact_count; index++) {
		struct node_view *view = &runtime->views[index];

		if (!view->slot_count)
			continue;
		view->slots = calloc(view->slot_count, sizeof(view->slots[0]));
		if (!view->slots)
			return KOBOX_RESOURCE_RUNTIME_NO_MEMORY;
		view->slot_count = 0;
	}
	for (index = 0; index < binding_count; index++) {
		kb2_closure_manifest_binding_t binding;
		kb2_resource_grant_slot_t grant_slot;
		struct node_view *view;
		size_t resource_index;
		size_t view_index;

		if (kb2_closure_manifest_binding(config->manifest, index, &binding) !=
		    KB2_PROTOCOL_OK)
			return KOBOX_RESOURCE_RUNTIME_MALFORMED;
		for (resource_index = 0; resource_index < resource_count;
		     resource_index++) {
			kb2_closure_manifest_resource_t resource;

			if (kb2_closure_manifest_resource(config->manifest,
							resource_index,
							&resource) !=
			    KB2_PROTOCOL_OK)
				return KOBOX_RESOURCE_RUNTIME_MALFORMED;
			if (resource.slot_id == binding.slot_id)
				break;
		}
		if (resource_index == resource_count ||
		    kb2_resource_grant_slot(config->grant, resource_index,
					    &grant_slot) != KB2_PROTOCOL_OK)
			return KOBOX_RESOURCE_RUNTIME_MALFORMED;
		view_index = find_view_index(runtime, binding.node_id);
		view = &runtime->views[view_index];
		view->slots[view->slot_count++] = (struct view_slot){
			.slot_id = grant_slot.slot_id,
			.state = grant_slot.state,
			.object_start = grant_slot.object_start,
			.object_count = grant_slot.object_count,
		};
	}
	for (index = 0; index < resource_count; index++) {
		kb2_closure_manifest_resource_t resource;
		size_t matches = 0;
		size_t binding_index;

		if (kb2_closure_manifest_resource(config->manifest, index,
						  &resource) != KB2_PROTOCOL_OK)
			return KOBOX_RESOURCE_RUNTIME_MALFORMED;
		for (binding_index = 0; binding_index < binding_count;
		     binding_index++) {
			kb2_closure_manifest_binding_t binding;

			if (kb2_closure_manifest_binding(config->manifest,
							 binding_index,
							 &binding) !=
			    KB2_PROTOCOL_OK)
				return KOBOX_RESOURCE_RUNTIME_MALFORMED;
			if (binding.slot_id == resource.slot_id)
				matches++;
		}
		if (!matches ||
		    (!(resource.flags & KB2_CLOSURE_RESOURCE_FLAG_SHARED) &&
		     matches != 1))
			return KOBOX_RESOURCE_RUNTIME_MALFORMED;
	}
	return KOBOX_RESOURCE_RUNTIME_OK;
}

static enum kobox_resource_runtime_status import_objects(
	const struct kobox_resource_runtime_config *config,
	struct kobox_resource_runtime *runtime)
{
	size_t index;

	runtime->object_count = kb2_resource_grant_object_count(config->grant);
	runtime->objects = calloc(runtime->object_count,
				  sizeof(runtime->objects[0]));
	if (runtime->object_count && !runtime->objects)
		return KOBOX_RESOURCE_RUNTIME_NO_MEMORY;
	for (index = 0; index < runtime->object_count; index++) {
		kb2_resource_grant_object_t object;
		kb2_resource_grant_slot_t slot;
		struct kobox_resource_native_handle *handles = NULL;
		struct registry_object *destination = &runtime->objects[index];
		size_t slot_index;
		size_t handle_index;

		if (kb2_resource_grant_object(config->grant, index, &object) !=
		    KB2_PROTOCOL_OK)
			return KOBOX_RESOURCE_RUNTIME_MALFORMED;
		for (slot_index = 0;
		     slot_index < kb2_resource_grant_slot_count(config->grant);
		     slot_index++) {
			if (kb2_resource_grant_slot(config->grant, slot_index,
						    &slot) != KB2_PROTOCOL_OK)
				return KOBOX_RESOURCE_RUNTIME_MALFORMED;
			if (slot.slot_id == object.slot_id)
				break;
		}
		if (slot_index == kb2_resource_grant_slot_count(config->grant))
			return KOBOX_RESOURCE_RUNTIME_MALFORMED;
		if (object.handle_count) {
			handles = calloc(object.handle_count, sizeof(handles[0]));
			if (!handles)
				return KOBOX_RESOURCE_RUNTIME_NO_MEMORY;
		}
		for (handle_index = 0; handle_index < object.handle_count;
		     handle_index++) {
			kb2_resource_grant_handle_binding_t binding;

			if (kb2_resource_grant_handle_binding(
				    config->grant, object.handle_start + handle_index,
				    &binding) != KB2_PROTOCOL_OK ||
			    binding.transfer_handle_index >=
				    config->native_handle_count) {
				free(handles);
				return KOBOX_RESOURCE_RUNTIME_MALFORMED;
			}
			handles[handle_index].role = binding.role;
			handles[handle_index].handle =
				config->native_handles[binding.transfer_handle_index];
		}
		destination->slot_id = object.slot_id;
		destination->resource_type = slot.resource_type;
		destination->object_id = object.object_id;
		destination->granted_rights = object.granted_rights;
		if (config->import_object(config->object_context, &slot, &object,
					  handles, object.handle_count,
					  &destination->native_object)) {
			free(handles);
			return KOBOX_RESOURCE_RUNTIME_IMPORT_FAILURE;
		}
		free(handles);
		if (!destination->native_object)
			return KOBOX_RESOURCE_RUNTIME_IMPORT_FAILURE;
	}
	return KOBOX_RESOURCE_RUNTIME_OK;
}

enum kobox_resource_runtime_status kobox_resource_runtime_open(
	const struct kobox_resource_runtime_config *config,
	struct kobox_resource_runtime **runtime_out)
{
	struct kobox_resource_runtime *runtime;
	enum kobox_resource_runtime_status status;
	size_t object_count;
	size_t handle_count;

	if (!config || !config->manifest || !config->grant || !runtime_out)
		return KOBOX_RESOURCE_RUNTIME_INVALID_ARGUMENT;
	*runtime_out = NULL;
	object_count = kb2_resource_grant_object_count(config->grant);
	handle_count = kb2_resource_grant_handle_binding_count(config->grant);
	if (handle_count != config->native_handle_count ||
	    (handle_count && !config->native_handles) ||
	    (object_count && (!config->import_object || !config->release_object)) ||
	    kb2_resource_grant_validate_manifest(config->grant,
						 config->manifest) !=
		    KB2_PROTOCOL_OK)
		return KOBOX_RESOURCE_RUNTIME_MALFORMED;
	runtime = calloc(1, sizeof(*runtime));
	if (!runtime)
		return KOBOX_RESOURCE_RUNTIME_NO_MEMORY;
	runtime->generation = config->grant->generation;
	runtime->release_object = config->release_object;
	runtime->object_context = config->object_context;
	status = import_objects(config, runtime);
	if (status == KOBOX_RESOURCE_RUNTIME_OK)
		status = copy_views(config, runtime);
	if (status != KOBOX_RESOURCE_RUNTIME_OK) {
		free_runtime(runtime);
		return status;
	}
	*runtime_out = runtime;
	return KOBOX_RESOURCE_RUNTIME_OK;
}

const void *kobox_resource_runtime_view(
	const struct kobox_resource_runtime *runtime, uint32_t node_id)
{
	size_t index;

	if (!runtime)
		return NULL;
	index = find_view_index(runtime, node_id);
	return index == SIZE_MAX ? NULL : &runtime->views[index];
}

uint64_t kobox_resource_runtime_generation(
	const struct kobox_resource_runtime *runtime)
{
	return runtime ? runtime->generation : 0;
}

const struct kobox_module_runtime_operations *
kobox_resource_runtime_operations(void)
{
	return &runtime_operations;
}

void kobox_resource_runtime_close(struct kobox_resource_runtime **runtime)
{
	if (!runtime || !*runtime)
		return;
	free_runtime(*runtime);
	*runtime = NULL;
}
