// SPDX-License-Identifier: GPL-2.0-only

#include "drm_query.h"

#include <errno.h>
#include <string.h>

static uint32_t read_u32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void write_u32(unsigned char *bytes, uint32_t value)
{
	size_t index;

	for (index = 0; index < 4; index++)
		bytes[index] = value >> (8 * index);
}

int kobox_drm_query_prepare(struct kobox_drm_query *out, uint64_t generation,
			    uint64_t session_id, const unsigned char *bytes,
			    size_t size, const kb2_gpu_region_t *output_region)
{
	struct kobox_drm_query query = {.session_id = session_id};
	kb2_gpu_command_t command;
	const unsigned char *data;
	size_t length, index;
	kb2_protocol_status_t status;

	if (!out || !session_id || !output_region ||
	    output_region->length > SIZE_MAX)
		return -EINVAL;
	status = kb2_gpu_command_decode(bytes, size, generation,
		KB2_GPU_PROFILE_VIRGL, KB2_GPU_QUEUE_EXECUTION,
		output_region, 1, &command);
	if (status != KB2_PROTOCOL_OK || command.session_id != session_id)
		return -EPROTO;
	query.output_size = output_region->length;
	/* Refuse unsupported operations without accepting any exchange ownership.
	 * A well-formed command refusal is a completion, not a channel fault.
	 */
	if (command.command_set_id != KB2_GPU_DRM_CORE_SET_ID ||
	    command.counts[2]) {
		*out = query;
		return 0;
	}
	query.command_id = command.command_id;
	data = kb2_gpu_command_inline_data(&command, &length);
	if (command.command_id == KB2_GPU_DRM_CORE_COMMAND_GET_CAP) {
		if (!data || length != KB2_GPU_DRM_CORE_RECORD_CAP_REQUEST_SIZE)
			return -EPROTO;
		query.capability = read_u32(data) | (uint64_t)read_u32(data + 4) << 32;
	} else if (command.command_id == KB2_GPU_DRM_CORE_COMMAND_VERSION) {
		if (!data || length != KB2_GPU_DRM_CORE_RECORD_VERSION_REQUEST_SIZE)
			return -EPROTO;
		for (index = 0; index < 3; index++)
			query.capacity[index] = read_u32(data + 4 * index);
		for (index = 1; index < command.counts[0]; index++) {
			kb2_gpu_argument_t argument;
			kb2_gpu_span_t span;
			size_t span_index;

			if (kb2_gpu_command_argument(&command, index, &argument) ||
			    argument.argument_id < 2 || argument.argument_id > 4)
				return -EPROTO;
			for (span_index = 0; span_index < command.counts[1]; span_index++) {
				if (kb2_gpu_command_span(&command, span_index, &span))
					return -EPROTO;
				if (span.span_id == argument.value)
					break;
			}
			if (span_index == command.counts[1] ||
			    span.region_id != output_region->region_id ||
			    span.length != query.capacity[argument.argument_id - 2])
				return -EPROTO;
			query.offset[argument.argument_id - 2] = span.offset;
		}
	}
	*out = query;
	return 0;
}

static uint32_t command_status(int result)
{
	switch (result) {
	case 0: return KB2_GPU_STATUS_OK;
	case -EINVAL: return KB2_GPU_STATUS_INVALID;
	case -ENOTTY:
	case -EOPNOTSUPP: return KB2_GPU_STATUS_UNSUPPORTED;
	case -EACCES:
	case -EPERM: return KB2_GPU_STATUS_DENIED;
	case -ENOENT: return KB2_GPU_STATUS_NOT_FOUND;
	case -ENOMEM: return KB2_GPU_STATUS_NO_MEMORY;
	case -EBUSY: return KB2_GPU_STATUS_BUSY;
	default: return KB2_GPU_STATUS_DEVICE_LOST;
	}
}

int kobox_drm_query_execute(const struct kobox_drm_query *query,
			   const struct kobox_drm_query_api *api,
			   struct kobox_linux_drm_file *file,
			   unsigned char *output, size_t output_size,
			   unsigned char *completion, size_t completion_capacity,
			   size_t *completion_size)
{
	struct kobox_linux_drm_version version;
	unsigned char record[KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT_SIZE] = {0};
	kb2_gpu_inline_completion_t reply = {0};
	uint64_t value;
	size_t index;
	int result;

	if (!query || !api || !api->version || !api->get_cap || !file ||
	    !output || output_size != query->output_size || !completion ||
	    !completion_size || completion_capacity < KB2_GPU_COMPLETION_HEADER_SIZE +
		KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE + sizeof(record))
		return -EINVAL;
	reply.session_id = query->session_id;
	for (index = 0; index < 3; index++)
		if (query->offset[index] > output_size ||
		    query->capacity[index] > output_size - query->offset[index])
			return -EPROTO;
	if (query->command_id == KB2_GPU_DRM_CORE_COMMAND_VERSION) {
		result = api->version(file, query->capacity, &version);
		if (!result) {
			const char *strings[] = {version.name, version.date, version.description};
			size_t lengths[] = {version.name_length, version.date_length,
					    version.description_length};

			write_u32(record, version.major);
			write_u32(record + 4, version.minor);
			write_u32(record + 8, version.patchlevel);
			for (index = 0; index < 3; index++) {
				if (lengths[index] > UINT32_MAX)
					return -EOVERFLOW;
				write_u32(record + 16 + 4 * index, lengths[index]);
				memcpy(output + query->offset[index], strings[index],
				       query->capacity[index]);
			}
			reply.record_schema_id = KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT;
			reply.length = sizeof(record);
		}
	} else if (query->command_id == KB2_GPU_DRM_CORE_COMMAND_GET_CAP) {
		result = api->get_cap(file, query->capability, &value);
		if (!result) {
			write_u32(record, value);
			write_u32(record + 4, value >> 32);
			reply.record_schema_id = KB2_GPU_DRM_CORE_RECORD_SCALAR_U64;
			reply.length = KB2_GPU_DRM_CORE_RECORD_SCALAR_U64_SIZE;
		}
	} else {
		result = -EOPNOTSUPP;
	}
	reply.status = command_status(result);
	reply.data = record;
	return kb2_gpu_inline_completion_encode(completion, completion_capacity,
		completion_size, &reply) == KB2_PROTOCOL_OK ? 0 : -EPROTO;
}
