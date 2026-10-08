// SPDX-License-Identifier: GPL-2.0-only

#include "drm_query.h"

#include <kobox2/amdgpu_info_shape.h>

#include <errno.h>
#include <string.h>
#ifdef __KERNEL__
#include <linux/printk.h>
#endif

enum {
	KOBOX_DRM_PRIME_RDWR = 0x2u,
	KOBOX_DRM_PRIME_CLOEXEC = 0x80000u,
	/* This parser is also compiled by native host tests, so mirror only the
	 * Linux UAPI flag bits admitted by the typed bridge here. */
	KOBOX_DRM_VBLANK_EVENT = 0x04000000u,
	KOBOX_DRM_VBLANK_ALLOWED = 0x0000003fu | 0x04000000u |
		0x10000000u | 0x20000000u,
	KOBOX_DRM_CRTC_SEQUENCE_ALLOWED = 3u,
};

static uint32_t read_u32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint16_t read_u16(const unsigned char *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static void write_u32(unsigned char *bytes, uint32_t value)
{
	size_t index;

	for (index = 0; index < 4; index++)
		bytes[index] = value >> (8 * index);
}

static uint64_t read_u64(const unsigned char *bytes)
{
	return read_u32(bytes) | (uint64_t)read_u32(bytes + 4) << 32;
}

static void write_u64(unsigned char *bytes, uint64_t value)
{
	write_u32(bytes, value);
	write_u32(bytes + 4, value >> 32);
}

static int input_span(const kb2_gpu_command_t *command,
		      const kb2_gpu_region_t *region, size_t vector_index,
		      uint32_t argument_id, uint32_t record_schema_id,
		      size_t offset, size_t count, size_t record_size)
{
	kb2_gpu_argument_t argument;
	kb2_gpu_span_t span;
	size_t bytes;

	if (!count || count > SIZE_MAX / record_size)
		return -EPROTO;
	bytes = count * record_size;
	if (kb2_gpu_command_argument(command, vector_index, &argument) ||
	    kb2_gpu_command_span(command, vector_index - 1, &span) ||
	    argument.argument_id != argument_id ||
	    argument.kind != KB2_GPU_ARGUMENT_SPAN ||
	    argument.flags != KB2_GPU_ARGUMENT_FLAG_INPUT ||
	    argument.record_schema_id != record_schema_id ||
	    argument.value != span.span_id || argument.count != count ||
	    span.region_id != region->region_id || span.offset != offset ||
	    span.length != bytes || span.rights != KB2_GPU_SPAN_RIGHT_READ ||
	    span.record_schema_id != record_schema_id ||
	    span.element_count != count || span.flags != KB2_GPU_SPAN_FLAG_INPUT)
		return -EPROTO;
	return 0;
}

static int output_span(const kb2_gpu_command_t *command,
		       const kb2_gpu_region_t *region, uint32_t argument_id,
		       uint32_t record_schema_id, size_t count,
		       size_t record_size, size_t *offset, size_t *extent)
{
	kb2_gpu_argument_t argument;
	kb2_gpu_span_t span;
	size_t argument_index, span_index, bytes;

	for (argument_index = 0; argument_index < command->counts[0];
	     argument_index++) {
		if (kb2_gpu_command_argument(command, argument_index, &argument))
			return -EPROTO;
		if (argument.argument_id == argument_id)
			break;
	}
	if (!count)
		return argument_index == command->counts[0] ? 0 : -EPROTO;
	if (argument_index == command->counts[0] || count > SIZE_MAX / record_size)
		return -EPROTO;
	bytes = count * record_size;
	for (span_index = 0; span_index < command->counts[1]; span_index++) {
		if (kb2_gpu_command_span(command, span_index, &span))
			return -EPROTO;
		if (span.span_id == argument.value)
			break;
	}
	if (span_index == command->counts[1] ||
	    argument.kind != KB2_GPU_ARGUMENT_SPAN ||
	    argument.flags != KB2_GPU_ARGUMENT_FLAG_OUTPUT ||
	    argument.record_schema_id != record_schema_id ||
	    argument.count != count || span.region_id != region->region_id ||
	    span.length != bytes || span.rights != KB2_GPU_SPAN_RIGHT_WRITE ||
	    span.record_schema_id != record_schema_id ||
	    span.element_count != count || span.flags != KB2_GPU_SPAN_FLAG_OUTPUT ||
	    span.offset > SIZE_MAX || span.length > SIZE_MAX - (size_t)span.offset)
		return -EPROTO;
	*offset = span.offset;
	if (*extent < (size_t)(span.offset + span.length))
		*extent = span.offset + span.length;
	return 0;
}

static int input_handle_span(const kb2_gpu_command_t *command,
			     const kb2_gpu_region_t *region, size_t count,
			     size_t record_size, uint32_t record_schema_id,
			     struct kobox_drm_query *query)
{
	size_t bytes;

	if (!count || count > KB2_GPU_DRM_CORE_MAX_SYNCOBJS ||
	    count > SIZE_MAX / record_size)
		return -EPROTO;
	bytes = count * record_size;
	if (command->counts[0] != 2 || command->counts[1] != 1 ||
	    command->counts[2] || input_span(command, region, 1, 2,
		record_schema_id, 0, count, record_size))
		return -EPROTO;
	query->handle_count = count;
	query->aux_size = bytes;
	query->aux_input = true;
	return 0;
}

int kobox_drm_query_prepare_profile(struct kobox_drm_query *out,
			    uint64_t generation, uint64_t session_id,
			    uint32_t profile_kind, uint32_t queue_class,
			    const unsigned char *bytes,
			    size_t size, const kb2_gpu_region_t *region)
{
	struct kobox_drm_query query = {
		.generation = generation,
		.session_id = session_id,
	};
	kb2_gpu_command_t command;
	const unsigned char *data;
	size_t length, index;
	kb2_protocol_status_t status;

	if (!out || !session_id || !region || region->length > SIZE_MAX)
		return -EINVAL;
	status = kb2_gpu_command_decode(bytes, size, generation,
		profile_kind, queue_class,
		region, 1, &command);
	if (status != KB2_PROTOCOL_OK || command.session_id != session_id)
		return -EPROTO;
	query.command_set_id = command.command_set_id;
	query.command_id = command.command_id;
	/* Only PRIME import currently accepts an input attachment. Refuse other
	 * well-formed attachment commands without accepting exchange ownership.
	 */
	if (command.counts[2] &&
	    (command.command_set_id != KB2_GPU_DRM_CORE_SET_ID ||
	     command.command_id !=
		KB2_GPU_DRM_CORE_COMMAND_PRIME_ATTACHMENT_TO_HANDLE)) {
		*out = query;
		return 0;
	}
	data = kb2_gpu_command_inline_data(&command, &length);
	if (length > sizeof(query.inline_data))
		return -EPROTO;
	if (length)
		memcpy(query.inline_data, data, length);
	query.inline_size = length;
	if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
	    command.command_id == KB2_GPU_DRM_CORE_COMMAND_GET_CAP) {
		if (!data || length != KB2_GPU_DRM_CORE_RECORD_CAP_REQUEST_SIZE)
			return -EPROTO;
		query.capability = read_u32(data) | (uint64_t)read_u32(data + 4) << 32;
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_CORE_COMMAND_GET_CLIENT) {
		if (!data || length != KB2_GPU_DRM_CORE_RECORD_CLIENT_REQUEST_SIZE ||
		    read_u32(data + KB2_GPU_DRM_CORE_RECORD_CLIENT_REQUEST_RESERVED_OFFSET))
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_CORE_COMMAND_POLL_EVENTS) {
		if (!data || length != KB2_GPU_DRM_CORE_RECORD_SCALAR_U32_SIZE ||
		    (read_u32(data) & ~KB2_GPU_DRM_CORE_POLL_READABLE))
			return -EPROTO;
		query.poll_events = read_u32(data);
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_CORE_COMMAND_READ_EVENTS) {
		kb2_gpu_argument_t argument;
		kb2_gpu_span_t span;
		size_t capacity;

		if (!data || length != KB2_GPU_DRM_CORE_RECORD_LENGTH_REQUEST_SIZE)
			return -EPROTO;
		capacity = read_u32(data +
			KB2_GPU_DRM_CORE_RECORD_LENGTH_REQUEST_CAPACITY_OFFSET);
		if (!capacity || capacity > KB2_GPU_DRM_CORE_MAX_EVENT_BYTES ||
		    read_u32(data +
			KB2_GPU_DRM_CORE_RECORD_LENGTH_REQUEST_RESERVED_OFFSET) ||
		    command.counts[0] != 2 || command.counts[1] != 1 ||
		    command.counts[2] ||
		    kb2_gpu_command_argument(&command, 1, &argument) ||
		    kb2_gpu_command_span(&command, 0, &span) ||
		    argument.argument_id !=
			KB2_GPU_DRM_CORE_COMMAND_READ_EVENTS_REQUEST_SPAN_EVENTS_ARGUMENT_ID ||
		    argument.kind != KB2_GPU_ARGUMENT_SPAN ||
		    argument.flags != KB2_GPU_ARGUMENT_FLAG_OUTPUT ||
		    argument.record_schema_id != KB2_GPU_DRM_CORE_RECORD_BYTE ||
		    argument.value != span.span_id || argument.count != capacity ||
		    span.region_id != region->region_id || span.length != capacity ||
		    span.rights != KB2_GPU_SPAN_RIGHT_WRITE ||
		    span.record_schema_id != KB2_GPU_DRM_CORE_RECORD_BYTE ||
		    span.element_count != capacity ||
		    span.flags != KB2_GPU_SPAN_FLAG_OUTPUT || span.offset > SIZE_MAX ||
		    span.length > SIZE_MAX - (size_t)span.offset)
			return -EPROTO;
		query.capacity[0] = capacity;
		query.offset[0] = span.offset;
		query.output_size = span.offset + span.length;
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_CORE_COMMAND_VERSION) {
		bool seen[3] = {false};

		if (!data || length != KB2_GPU_DRM_CORE_RECORD_VERSION_REQUEST_SIZE)
			return -EPROTO;
		for (index = 0; index < 3; index++)
			query.capacity[index] = read_u32(data + 4 * index);
		for (index = 1; index < command.counts[0]; index++) {
			kb2_gpu_argument_t argument;
			kb2_gpu_span_t span;
			size_t span_index;

			if (kb2_gpu_command_argument(&command, index, &argument) ||
			    argument.argument_id < 2 || argument.argument_id > 4 ||
			    argument.kind != KB2_GPU_ARGUMENT_SPAN ||
			    argument.flags != KB2_GPU_ARGUMENT_FLAG_OUTPUT ||
			    argument.record_schema_id != KB2_GPU_DRM_CORE_RECORD_BYTE ||
			    argument.count != query.capacity[argument.argument_id - 2] ||
			    seen[argument.argument_id - 2])
				return -EPROTO;
			seen[argument.argument_id - 2] = true;
			for (span_index = 0; span_index < command.counts[1]; span_index++) {
				if (kb2_gpu_command_span(&command, span_index, &span))
					return -EPROTO;
				if (span.span_id == argument.value)
					break;
			}
			if (span_index == command.counts[1] ||
			    span.region_id != region->region_id ||
			    span.length != query.capacity[argument.argument_id - 2] ||
			    span.rights != KB2_GPU_SPAN_RIGHT_WRITE ||
			    span.record_schema_id != KB2_GPU_DRM_CORE_RECORD_BYTE ||
			    span.element_count != argument.count ||
			    span.flags != KB2_GPU_SPAN_FLAG_OUTPUT || span.offset > SIZE_MAX ||
			    span.length > SIZE_MAX - (size_t)span.offset)
				return -EPROTO;
			query.offset[argument.argument_id - 2] = span.offset;
			if (query.output_size < (size_t)(span.offset + span.length))
				query.output_size = span.offset + span.length;
		}
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_CORE_COMMAND_GEM_CLOSE) {
		if (!data || length != KB2_GPU_DRM_CORE_RECORD_GEM_HANDLE_REQUEST_SIZE ||
		    !read_u32(data) || read_u32(data + 4))
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id ==
			KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT) {
		uint32_t flags;

		if (!data || length != KB2_GPU_DRM_CORE_RECORD_PRIME_EXPORT_REQUEST_SIZE)
			return -EPROTO;
		flags = read_u32(data +
			KB2_GPU_DRM_CORE_RECORD_PRIME_EXPORT_REQUEST_FLAGS_OFFSET);
		if (!read_u32(data +
			KB2_GPU_DRM_CORE_RECORD_PRIME_EXPORT_REQUEST_HANDLE_OFFSET) ||
		    (flags & ~(KOBOX_DRM_PRIME_CLOEXEC | KOBOX_DRM_PRIME_RDWR)) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.prime_handle = read_u32(data +
			KB2_GPU_DRM_CORE_RECORD_PRIME_EXPORT_REQUEST_HANDLE_OFFSET);
		query.prime_flags = flags;
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id ==
			KB2_GPU_DRM_CORE_COMMAND_PRIME_ATTACHMENT_TO_HANDLE) {
		kb2_gpu_argument_t argument;
		kb2_gpu_attachment_t attachment;
		const uint64_t rights = KB2_GPU_SPAN_RIGHT_READ |
			KB2_GPU_SPAN_RIGHT_WRITE;

		if (!data || length != KB2_GPU_DRM_CORE_RECORD_PRIME_IMPORT_REQUEST_SIZE ||
		    read_u32(data +
			KB2_GPU_DRM_CORE_RECORD_PRIME_IMPORT_REQUEST_FLAGS_OFFSET) ||
		    read_u32(data +
			KB2_GPU_DRM_CORE_RECORD_PRIME_IMPORT_REQUEST_RESERVED_OFFSET) ||
		    command.counts[0] != 2 || command.counts[1] ||
		    command.counts[2] != 1 || command.deadline_ns ||
		    kb2_gpu_command_argument(&command, 1, &argument) ||
		    kb2_gpu_command_attachment(&command, 0, &attachment) ||
		    argument.argument_id !=
			KB2_GPU_DRM_CORE_COMMAND_PRIME_ATTACHMENT_TO_HANDLE_REQUEST_ATTACHMENT_DMA_BUFFER_ARGUMENT_ID ||
		    argument.kind != KB2_GPU_ARGUMENT_ATTACHMENT ||
		    argument.flags != KB2_GPU_ARGUMENT_FLAG_INPUT ||
		    argument.record_schema_id || argument.count != 1 ||
		    argument.value != attachment.attachment_id ||
		    attachment.object_class != KB2_GPU_ATTACHMENT_DMA_BUF ||
		    !attachment.exchange_id || attachment.generation != generation ||
		    attachment.rights != rights ||
		    attachment.role !=
			KB2_GPU_DRM_CORE_COMMAND_PRIME_ATTACHMENT_TO_HANDLE_REQUEST_ATTACHMENT_DMA_BUFFER_ROLE ||
		    attachment.ownership != KB2_GPU_ATTACHMENT_SHARE ||
		    attachment.flags != KB2_GPU_ATTACHMENT_FLAG_INPUT)
			return -EPROTO;
		query.prime_token = attachment.exchange_id;
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_CREATE) {
		if (!data || length != KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_CREATE_REQUEST_SIZE ||
		    (read_u32(data) & ~1U) || read_u32(data + 4) ||
		    command.counts[0] != 1 || command.counts[1] || command.counts[2])
			return -EPROTO;
		query.syncobj_flags = read_u32(data);
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_DESTROY) {
		if (!data || length != KB2_GPU_DRM_CORE_RECORD_GEM_HANDLE_REQUEST_SIZE ||
		    !read_u32(data) || read_u32(data + 4) ||
		    command.counts[0] != 1 || command.counts[1] || command.counts[2])
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   (command.command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_WAIT ||
		    command.command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_RESET ||
		    command.command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_SIGNAL)) {
		size_t count;
		int result;

		if (command.command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_WAIT) {
			if (!data || length != KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST_SIZE)
				return -EPROTO;
			count = read_u32(data);
			query.syncobj_flags = read_u32(data + 4);
			query.fence_deadline_ns = read_u64(data + 8);
			query.timeout_nsec = command.deadline_ns == UINT64_MAX ? -1 :
				(int64_t)command.deadline_ns;
			if (!command.deadline_ns ||
			    (command.deadline_ns != UINT64_MAX && command.deadline_ns > INT64_MAX) ||
			    (query.syncobj_flags & ~15U) ||
			    (!(query.syncobj_flags & 8U) && query.fence_deadline_ns))
				return -EPROTO;
		} else {
			if (!data || length != KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_ARRAY_REQUEST_SIZE ||
			    read_u32(data + 4) || command.deadline_ns)
				return -EPROTO;
			count = read_u32(data);
		}
		result = input_handle_span(&command, region, count, sizeof(uint32_t),
			KB2_GPU_DRM_CORE_RECORD_HANDLE_U32, &query);
		if (result)
			return result;
	} else if (command.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_CORE_COMMAND_SET_CLIENT_CAP) {
		if (!data || length != KB2_GPU_DRM_CORE_RECORD_CLIENT_CAP_REQUEST_SIZE)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   (command.command_id == KB2_GPU_DRM_MODE_COMMAND_SET_MASTER ||
		    command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_MAGIC ||
		    command.command_id == KB2_GPU_DRM_MODE_COMMAND_DROP_MASTER)) {
		if (length || command.counts[0] || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_AUTH_MAGIC) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_MAGIC_SIZE ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_WAIT_VBLANK) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_WAIT_VBLANK_REQUEST_SIZE ||
		    read_u64(data) != 1 ||
		    (read_u32(data + 8) & ~KOBOX_DRM_VBLANK_ALLOWED) ||
		    (!(read_u32(data + 8) & KOBOX_DRM_VBLANK_EVENT) &&
			read_u64(data + 16)) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns != UINT64_MAX)
			return -EPROTO;
		query.mode_flags = read_u32(data + 8);
		query.sequence = read_u32(data + 12);
		query.event_token = read_u64(data + 16);
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_CRTC_GET_SEQUENCE) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_CRTC_SEQUENCE_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) || read_u32(data + 12) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.object_id = read_u32(data + 8);
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_CRTC_QUEUE_SEQUENCE) {
		if (!data || length !=
			KB2_GPU_DRM_MODE_RECORD_CRTC_QUEUE_SEQUENCE_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    (read_u32(data + 12) & ~KOBOX_DRM_CRTC_SEQUENCE_ALLOWED) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.object_id = read_u32(data + 8);
		query.mode_flags = read_u32(data + 12);
		query.sequence = read_u64(data + 16);
		query.event_token = read_u64(data + 24);
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   (command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_FB ||
		    command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_FB2)) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_FB_GET_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) || read_u32(data + 12) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.object_id = read_u32(data + 8);
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_RESOURCES) {
		size_t spans = 0;

		if (!data || length != KB2_GPU_DRM_MODE_RECORD_RESOURCES_REQUEST_SIZE ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
		for (index = 0; index < 4; index++) {
			query.capacity[index] = read_u32(data + 4 * index);
			spans += !!query.capacity[index];
			if (output_span(&command, region, index + 2,
				KB2_GPU_DRM_MODE_RECORD_OBJECT_ID,
				query.capacity[index], sizeof(uint32_t),
				&query.offset[index], &query.output_size))
				return -EPROTO;
		}
		if (command.counts[0] != spans + 1 || command.counts[1] != spans)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_CONNECTOR) {
		const uint32_t records[3] = {
			KB2_GPU_DRM_MODE_RECORD_MODE_INFO,
			KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE,
			KB2_GPU_DRM_MODE_RECORD_OBJECT_ID,
		};
		const size_t sizes[3] = {
			KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE,
			KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_SIZE,
			KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_SIZE,
		};
		size_t spans = 0;

		if (!data || length != KB2_GPU_DRM_MODE_RECORD_CONNECTOR_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    read_u32(data + 28) || command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.object_id = read_u32(data + 8);
		query.mode_flags = read_u32(data + 24);
		for (index = 0; index < 3; index++) {
			query.capacity[index] = read_u32(data + 12 + 4 * index);
			if (query.capacity[index] > 65536)
				return -EPROTO;
			spans += !!query.capacity[index];
			if (output_span(&command, region, index + 2, records[index],
				query.capacity[index], sizes[index],
				&query.offset[index], &query.output_size))
				return -EPROTO;
		}
		if (query.mode_flags != (query.capacity[0] ? 0 :
			KB2_GPU_DRM_MODE_CONNECTOR_FLAG_FORCE_PROBE) ||
		    command.counts[0] != spans + 1 || command.counts[1] != spans)
			return -EPROTO;
		if (query.capacity[0] > 32 || query.capacity[1] > 32 ||
		    query.capacity[2] > 16) {
			size_t next = 0;
			for (index = 0; index < 3; index++) {
				if (query.capacity[index] && query.offset[index] != next)
					return -EPROTO;
				next += query.capacity[index] * sizes[index];
			}
			if (query.output_size != next || region->length != next)
				return -EPROTO;
			query.aux_size = query.output_size;
			query.aux_output = true;
			query.output_size = 0;
		}
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_ENCODER) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_ENCODER_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    read_u32(data + 12) || command.counts[0] != 1 ||
		    command.counts[1] || command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.object_id = read_u32(data + 8);
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_CRTC) {
		if (!data ||
		    length != KB2_GPU_DRM_MODE_RECORD_CRTC_GET_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    read_u32(data + 12) || command.counts[0] != 2 ||
		    command.counts[1] != 1 || command.counts[2] ||
		    command.deadline_ns || output_span(&command, region, 2,
			KB2_GPU_DRM_MODE_RECORD_MODE_INFO, 1,
			KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE,
			&query.offset[0], &query.output_size))
			return -EPROTO;
		query.object_id = read_u32(data + 8);
		query.capacity[0] = 1;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_SET_CRTC) {
		size_t vector_index = 1;
		size_t connector_count;
		size_t connector_bytes;

		if (!data || length != KB2_GPU_DRM_MODE_RECORD_CRTC_SET_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    read_u32(data + 24) > 1 || command.counts[2] ||
		    command.deadline_ns)
			return -EPROTO;
		connector_count = read_u32(data + 28);
		if (connector_count > SIZE_MAX / sizeof(uint32_t))
			return -EPROTO;
		connector_bytes = connector_count * sizeof(uint32_t);
		if (command.counts[0] != 1u + (uint32_t)!!connector_count +
				(uint32_t)!!read_u32(data + 24) ||
		    command.counts[1] != (uint32_t)!!connector_count +
				(uint32_t)!!read_u32(data + 24))
			return -EPROTO;
		if (connector_count && input_span(&command, region,
			vector_index++, 2, KB2_GPU_DRM_MODE_RECORD_OBJECT_ID,
			0, connector_count, sizeof(uint32_t)))
			return -EPROTO;
		if (read_u32(data + 24) && input_span(&command, region,
			vector_index, 3, KB2_GPU_DRM_MODE_RECORD_MODE_INFO,
			connector_bytes, 1,
			KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE))
			return -EPROTO;
		if (connector_bytes > SIZE_MAX - read_u32(data + 24) *
				KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE)
			return -EPROTO;
		query.object_id = read_u32(data + 8);
		query.capacity[0] = connector_count;
		query.mode_flags = read_u32(data + 24);
		query.aux_size = connector_bytes + query.mode_flags *
			KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE;
		query.aux_input = !!query.aux_size;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   (command.command_id == KB2_GPU_DRM_MODE_COMMAND_CURSOR ||
		    command.command_id == KB2_GPU_DRM_MODE_COMMAND_CURSOR2)) {
		bool cursor2 = command.command_id == KB2_GPU_DRM_MODE_COMMAND_CURSOR2;
		size_t expected = cursor2 ? KB2_GPU_DRM_MODE_RECORD_CURSOR2_REQUEST_SIZE :
			KB2_GPU_DRM_MODE_RECORD_CURSOR_REQUEST_SIZE;
		uint32_t flags;

		if (!data || length != expected || read_u64(data) != 1 ||
		    !read_u32(data + 12) || command.counts[0] != 1 ||
		    command.counts[1] || command.counts[2] || command.deadline_ns ||
		    (!cursor2 && read_u32(data + 36)))
			return -EPROTO;
		flags = read_u32(data + 8);
		if (!flags || (flags & ~(KB2_GPU_DRM_MODE_CURSOR_FLAG_BUFFER |
			KB2_GPU_DRM_MODE_CURSOR_FLAG_MOVE)))
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_PAGE_FLIP) {
		uint32_t flags;
		uint32_t target;

		if (!data || length != KB2_GPU_DRM_MODE_RECORD_PAGE_FLIP_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    !read_u32(data + 12) || command.counts[0] != 1 ||
		    command.counts[1] || command.counts[2] || command.deadline_ns)
			return -EPROTO;
		flags = read_u32(data + 16);
		target = flags &
			(KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_ABSOLUTE |
			 KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_RELATIVE);
		if ((flags & ~(KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_EVENT |
			KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_ASYNC |
			KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_ABSOLUTE |
			KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_RELATIVE)) ||
		    target == (KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_ABSOLUTE |
			KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_RELATIVE) ||
		    (!target && read_u32(data + 20)) ||
		    (!(flags & KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_EVENT) &&
			read_u64(data + 24)))
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_DIRTY_FB) {
		uint32_t rectangle_count;
		uint32_t flags;

		if (!data || length != KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_SIZE ||
		    read_u64(data +
			KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_TOPOLOGY_EPOCH_OFFSET) != 1 ||
		    command.deadline_ns || command.counts[2])
			return -EPROTO;
		flags = read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_FLAGS_OFFSET);
		rectangle_count = read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_RECTANGLE_COUNT_OFFSET);
		if ((flags & ~(KB2_GPU_DRM_MODE_DIRTY_FLAG_COPY |
			KB2_GPU_DRM_MODE_DIRTY_FLAG_FILL)) ||
		    rectangle_count >
			KB2_GPU_DRM_MODE_COMMAND_DIRTY_FB_REQUEST_SPAN_RECTANGLES_MAXIMUM ||
		    ((flags & KB2_GPU_DRM_MODE_DIRTY_FLAG_COPY) &&
			(rectangle_count & 1)) ||
		    command.counts[0] != 1u + !!rectangle_count ||
		    command.counts[1] != !!rectangle_count)
			return -EPROTO;
		if (rectangle_count && input_span(&command, region, 1, 2,
			KB2_GPU_DRM_MODE_RECORD_RECTANGLE, 0, rectangle_count,
			KB2_GPU_DRM_MODE_RECORD_RECTANGLE_SIZE))
			return -EPROTO;
		query.object_id = read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_FB_ID_OFFSET);
		query.mode_flags = flags;
		query.capacity[0] = rectangle_count;
		query.aux_size = rectangle_count *
			KB2_GPU_DRM_MODE_RECORD_RECTANGLE_SIZE;
		query.aux_input = !!rectangle_count;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_CREATE_DUMB) {
		if (!data ||
		    length != KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_SIZE ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_HEIGHT_OFFSET) ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_WIDTH_OFFSET) ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_BITS_PER_PIXEL_OFFSET) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_DESTROY_DUMB) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_SIZE ||
		    !read_u32(data) || command.counts[0] != 1 ||
		    command.counts[1] || command.counts[2] || command.deadline_ns)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_SET_PROPERTY) {
		if (!data ||
		    length != KB2_GPU_DRM_MODE_RECORD_SET_PROPERTY_REQUEST_SIZE ||
		    read_u64(data +
			KB2_GPU_DRM_MODE_RECORD_SET_PROPERTY_REQUEST_TOPOLOGY_EPOCH_OFFSET) != 1 ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_SET_PROPERTY_REQUEST_CONNECTOR_ID_OFFSET) ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_SET_PROPERTY_REQUEST_PROPERTY_ID_OFFSET) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_SET_PLANE) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_PLANE_SET_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    read_u32(data + 20) || command.counts[0] != 1 ||
		    command.counts[1] || command.counts[2] || command.deadline_ns)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   (command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_GAMMA ||
		    command.command_id == KB2_GPU_DRM_MODE_COMMAND_SET_GAMMA)) {
		const bool set = command.command_id == KB2_GPU_DRM_MODE_COMMAND_SET_GAMMA;
		size_t bytes = 0;
		uint32_t count;

		if (!data || length != KB2_GPU_DRM_MODE_RECORD_GAMMA_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    command.counts[0] != 4 || command.counts[1] != 3 ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
		count = read_u32(data + 12);
		if (!count || count > 65536)
			return -EPROTO;
		for (index = 0; index < 3; ++index) {
			size_t offset = 0;
			if (set ? input_span(&command, region, index + 1,
				index + 2, KB2_GPU_DRM_MODE_RECORD_LUT_VALUE,
				index * (size_t)count * 2, count, sizeof(uint16_t)) :
			    output_span(&command, region, index + 2,
				KB2_GPU_DRM_MODE_RECORD_LUT_VALUE, count,
				sizeof(uint16_t), &offset, &bytes))
				return -EPROTO;
			if (!set && offset != index * (size_t)count * 2)
				return -EPROTO;
		}
		if ((uint64_t)count * 6 > region->length ||
		    (!set && bytes != (size_t)count * 6))
			return -EPROTO;
		query.object_id = read_u32(data + 8);
		query.gamma_count = count;
		query.aux_size = (size_t)count * 6;
		query.aux_input = set;
		query.aux_output = !set;
		/* Three LUTs are transferred via aux, not the fixed output page. */
		query.output_size = 0;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_ADD_FB) {
		if (!data ||
		    length != KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_SIZE ||
		    read_u64(data +
			KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_TOPOLOGY_EPOCH_OFFSET) != 1 ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_WIDTH_OFFSET) ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_HEIGHT_OFFSET) ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_PITCH_OFFSET) ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_BITS_PER_PIXEL_OFFSET) ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_DEPTH_OFFSET) ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_HANDLE_OFFSET) ||
		    read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_RESERVED_OFFSET) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_REMOVE_FB) {
		if (!data ||
		    length != KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST_SIZE ||
		    read_u64(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST_TOPOLOGY_EPOCH_OFFSET) != 1 ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST_OBJECT_ID_OFFSET) ||
		    read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST_RESERVED_OFFSET) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_MAP_DUMB) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST_SIZE ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.mapping_handle = read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST_HANDLE_OFFSET);
		query.mapping_rights = read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST_MAPPING_RIGHTS_OFFSET);
		if (!query.mapping_handle || !query.mapping_rights ||
		    (query.mapping_rights & ~(KB2_GPU_SPAN_RIGHT_READ |
			KB2_GPU_SPAN_RIGHT_WRITE)))
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_ADD_FB2) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_FB2_SIZE ||
		    read_u64(data) != 1 || read_u32(data + 8) ||
		    !read_u32(data + 12) || !read_u32(data + 16) ||
		    !read_u32(data + 20) || !read_u32(data + 28) ||
		    !read_u32(data + 44) || (read_u32(data + 24) & ~2U) ||
		    command.counts[0] != 1 || command.counts[1] ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id ==
			KB2_GPU_DRM_MODE_COMMAND_OBJECT_GET_PROPERTIES) {
		if (!data ||
		    length != KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_SIZE ||
		    read_u64(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_TOPOLOGY_EPOCH_OFFSET) != 1 ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_OBJECT_ID_OFFSET) ||
		    !read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_OBJECT_TYPE_OFFSET) ||
		    read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_RESERVED_OFFSET) ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.object_id = read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_OBJECT_ID_OFFSET);
		query.object_type = read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_OBJECT_TYPE_OFFSET);
		query.capacity[0] = read_u32(data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_PROPERTY_CAPACITY_OFFSET);
		if (output_span(&command, region, 2,
			KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE, query.capacity[0],
			KB2_GPU_DRM_MODE_RECORD_PROPERTY_VALUE_SIZE,
			&query.offset[0], &query.output_size) ||
		    command.counts[0] != 1u + (uint32_t)!!query.capacity[0] ||
		    command.counts[1] != (uint32_t)!!query.capacity[0])
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   (command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_PLANE_RESOURCES ||
		    command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_PLANE)) {
		bool plane = command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_PLANE;
		if (!data)
			return -EPROTO;
		query.capacity[0] = read_u32(data + (plane ? 12 : 0));
		if (length != (plane ?
			KB2_GPU_DRM_MODE_RECORD_PLANE_GET_REQUEST_SIZE :
			KB2_GPU_DRM_MODE_RECORD_PLANE_RESOURCES_REQUEST_SIZE) ||
		    (plane && (read_u64(data) != 1 || !read_u32(data + 8))) ||
		    (!plane && read_u32(data + 4)) || command.counts[2] ||
		    command.deadline_ns || output_span(&command, region, 2,
			KB2_GPU_DRM_MODE_RECORD_OBJECT_ID, query.capacity[0],
			sizeof(uint32_t), &query.offset[0], &query.output_size) ||
		    command.counts[0] != 1 + !!query.capacity[0] ||
		    command.counts[1] != !!query.capacity[0])
			return -EPROTO;
		if (plane)
			query.object_id = read_u32(data + 8);
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY) {
		size_t spans = 0;

		if (!data || length != KB2_GPU_DRM_MODE_RECORD_PROPERTY_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    read_u32(data + 20) || command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.object_id = read_u32(data + 8);
		query.capacity[0] = read_u32(data + 12);
		query.capacity[1] = read_u32(data + 16);
		spans = !!query.capacity[0] + !!query.capacity[1];
		if (output_span(&command, region, 2,
			KB2_GPU_DRM_MODE_RECORD_SCALAR_U64, query.capacity[0],
			sizeof(uint64_t), &query.offset[0], &query.output_size) ||
		    output_span(&command, region, 3,
			KB2_GPU_DRM_MODE_RECORD_PROPERTY_ENUM, query.capacity[1],
			KB2_GPU_DRM_MODE_RECORD_PROPERTY_ENUM_SIZE,
			&query.offset[1], &query.output_size) ||
		    command.counts[0] != 1 + spans || command.counts[1] != spans)
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY_BLOB) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_BLOB_GET_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) || command.counts[2] ||
		    command.deadline_ns)
			return -EPROTO;
		query.object_id = read_u32(data + 8);
		query.capacity[0] = read_u32(data + 12);
		if (output_span(&command, region, 2,
			KB2_GPU_DRM_MODE_RECORD_BYTE, query.capacity[0], 1,
			&query.offset[0], &query.output_size) ||
		    command.counts[0] != 1 + !!query.capacity[0] ||
		    command.counts[1] != !!query.capacity[0])
			return -EPROTO;
		if (query.capacity[0] > 128) {
			if (query.output_size != query.capacity[0] ||
			    query.capacity[0] > KB2_GPU_DRM_MODE_MAX_BLOB_BYTES)
				return -EPROTO;
			query.aux_size = query.output_size;
			query.aux_output = true;
			query.output_size = 0;
		}
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_CREATE_PROPERTY_BLOB) {
		if (!data)
			return -EPROTO;
		query.aux_size = read_u32(data + 8);
		if (length != KB2_GPU_DRM_MODE_RECORD_BLOB_CREATE_REQUEST_SIZE ||
		    read_u64(data) != 1 || !query.aux_size || read_u32(data + 12) ||
		    command.counts[0] != 2 || command.counts[1] != 1 ||
		    command.counts[2] || command.deadline_ns ||
		    input_span(&command, region, 1, 2,
			KB2_GPU_DRM_MODE_RECORD_BYTE, 0, query.aux_size, 1))
			return -EPROTO;
		query.aux_input = true;
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_DESTROY_PROPERTY_BLOB) {
		if (!data || length != KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST_SIZE ||
		    read_u64(data) != 1 || !read_u32(data + 8) ||
		    read_u32(data + 12) || command.counts[0] != 1 ||
		    command.counts[1] || command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query.object_id = read_u32(data + 8);
	} else if (command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   command.command_id == KB2_GPU_DRM_MODE_COMMAND_ATOMIC) {
		size_t objects, properties;

		if (!data || length != KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_SIZE ||
		    read_u64(data) != 1 ||
		    (read_u32(data + 16) &
			~(KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_EVENT |
			  KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_ASYNC |
			  KB2_GPU_DRM_MODE_ATOMIC_FLAG_TEST_ONLY |
			  KB2_GPU_DRM_MODE_ATOMIC_FLAG_NONBLOCK |
			  KB2_GPU_DRM_MODE_ATOMIC_FLAG_ALLOW_MODESET)) ||
		    (!(read_u32(data + 16) & KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_EVENT) &&
		     read_u64(data + 8)) ||
		    read_u32(data + 28) || command.counts[0] != 3 ||
		    command.counts[1] != 2 || command.counts[2] ||
		    command.deadline_ns)
			return -EPROTO;
		objects = read_u32(data + 20);
		properties = read_u32(data + 24);
		if (!objects || objects > 32 || !properties || properties > 192 ||
		    input_span(&command, region, 1, 2,
			KB2_GPU_DRM_MODE_RECORD_ATOMIC_OBJECT, 0, objects,
			KB2_GPU_DRM_MODE_RECORD_ATOMIC_OBJECT_SIZE) ||
		    input_span(&command, region, 2, 3,
			KB2_GPU_DRM_MODE_RECORD_ATOMIC_PROPERTY,
			objects * KB2_GPU_DRM_MODE_RECORD_ATOMIC_OBJECT_SIZE,
			properties, KB2_GPU_DRM_MODE_RECORD_ATOMIC_PROPERTY_SIZE))
			return -EPROTO;
		query.capacity[0] = objects;
		query.capacity[1] = properties;
		query.mode_flags = read_u32(data + 16);
		query.event_token = read_u64(data + 8);
		query.aux_size = objects * KB2_GPU_DRM_MODE_RECORD_ATOMIC_OBJECT_SIZE +
			properties * KB2_GPU_DRM_MODE_RECORD_ATOMIC_PROPERTY_SIZE;
		query.aux_input = true;
	} else if (command.command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
		   command.command_id == KB2_GPU_DRM_AMDGPU_COMMAND_INFO) {
		uint32_t query_id, record_id, count, selectors[4];
		struct kb2_amdgpu_info_shape shape;
		size_t capacity;

		if (!data || length != KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_SIZE ||
		    command.counts[0] != 2 || command.counts[1] != 1 ||
		    command.counts[2] || command.deadline_ns)
			return -EPROTO;
		query_id = read_u32(data +
			KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_QUERY_OFFSET);
		for (unsigned int i = 0; i < 4; ++i)
			selectors[i] = read_u32(data + 16 + i * 4);
		if (!kb2_amdgpu_info_shape(query_id, selectors, &shape))
			return -EPROTO;
		record_id = shape.record; count = shape.count; capacity = shape.bytes;
		if (!record_id ||
		    read_u32(data +
			KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_RESPONSE_RECORD_ID_OFFSET) != record_id ||
		    read_u32(data +
			KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_RESPONSE_CAPACITY_OFFSET) != capacity ||
		    read_u32(data +
			KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_FLAGS_OFFSET) ||
		    output_span(&command, region, 2, record_id, count, capacity / count,
			&query.offset[0], &query.output_size) ||
		    query.offset[0])
			return -EPROTO;
		query.capacity[0] = capacity;
		query.aux_size = capacity;
		query.aux_output = true;
	} else if (command.command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
		   command.command_id == KB2_GPU_DRM_AMDGPU_COMMAND_GEM_OP) {
		uint32_t operation;

		if (!data || length != KB2_GPU_DRM_AMDGPU_RECORD_GEM_OP_REQUEST_SIZE ||
		    !read_u32(data) || read_u32(data + 20) || command.deadline_ns ||
		    command.counts[2])
			return -EPROTO;
		operation = read_u32(data + 4);
		if (operation == 0) {
			if (read_u64(data + 8) || read_u32(data + 16) != 1 ||
			    command.counts[0] != 2 || command.counts[1] != 1 ||
			    output_span(&command, region, 2,
				KB2_GPU_DRM_AMDGPU_RECORD_GEM_CREATE_INFO, 1, 32,
				&query.offset[0], &query.output_size) || query.offset[0])
				return -EPROTO;
			query.capacity[0] = query.aux_size = 32;
			query.aux_output = true;
		} else if (operation == 1) {
			if (read_u32(data + 16) || (read_u64(data + 8) & ~UINT64_C(7)) ||
			    command.counts[0] != 1 || command.counts[1])
				return -EPROTO;
		} else {
			return -EOPNOTSUPP;
		}
	} else if (command.command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
		   command.command_id == KB2_GPU_DRM_AMDGPU_COMMAND_CS) {
		uint32_t count = data ? read_u32(data + 8) : 0;
		size_t offset = 0;
		unsigned int ibs = 0, bos = 0;

		if (!data || length != KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST_SIZE ||
		    !read_u32(data) || read_u32(data + 12) || !count ||
		    count > KB2_AMDGPU_CS_PARTS || command.counts[0] != count + 1 ||
		    command.counts[1] != count || command.counts[2] || command.deadline_ns)
			return -EPROTO;
		for (uint32_t i = 0; i < count; ++i) {
			kb2_gpu_argument_t argument;
			struct kb2_amdgpu_cs_part *p = &query.cs_parts[i];
			uint32_t size;
			if (kb2_gpu_command_argument(&command, i + 1, &argument) ||
			    argument.count > 65536)
				return -EPROTO;
			p->record = argument.record_schema_id;
			p->count = argument.count;
			p->offset = offset;
			size = kb2_amdgpu_cs_element_size(p->record);
			p->bytes = p->count * size;
			if (!kb2_amdgpu_cs_part_valid(p) ||
			    input_span(&command, region, i + 1, i + 2,
				p->record, offset, p->count, size))
				return -EPROTO;
			ibs += p->record == KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_IB;
			if (p->record == KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_BO_HANDLE &&
			    (++bos > 1 || read_u32(data + 4)))
				return -EPROTO;
			offset += p->bytes;
		}
		if (!ibs) return -EPROTO;
		query.cs_count = count;
		query.aux_size = offset;
		query.aux_input = true;
	} else if (command.command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID) {
		size_t expected;

		if (!data)
			return -EPROTO;
		switch (command.command_id) {
		case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_MMAP:
			expected = KB2_GPU_DRM_AMDGPU_RECORD_GEM_MMAP_REQUEST_SIZE;
			if (length != expected)
				return -EPROTO;
			query.mapping_handle = read_u32(data);
			query.mapping_rights = read_u32(data + 4);
			if (!query.mapping_handle || query.mapping_rights !=
			    (KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE))
				return -EPROTO;
			break;
		case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_CREATE:
			expected = KB2_GPU_DRM_AMDGPU_RECORD_GEM_CREATE_REQUEST_SIZE;
			if (length != expected)
				return -EPROTO;
			if (!read_u64(data))
				return -EPROTO;
			break;
		case KB2_GPU_DRM_AMDGPU_COMMAND_CONTEXT:
			expected = KB2_GPU_DRM_AMDGPU_RECORD_CONTEXT_REQUEST_SIZE;
			if (length != expected)
				return -EPROTO;
			if ((read_u32(data) < 1 || read_u32(data) > 4) ||
			    read_u32(data + 4) ||
			    (read_u32(data) == 1 && read_u32(data + 8)) ||
			    (read_u32(data) != 1 && !read_u32(data + 8)))
				return -EPROTO;
			break;
		case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_VA:
			expected = KB2_GPU_DRM_AMDGPU_RECORD_GEM_VA_REQUEST_SIZE;
			if (length != expected)
				return -EPROTO;
			if (read_u32(data + 12) || read_u64(data + 40) ||
			    read_u32(data + 48) || read_u32(data + 52) ||
			    !read_u64(data + 16) || !read_u64(data + 32))
				return -EPROTO;
			break;
		case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_METADATA:
			expected = KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_REQUEST_SIZE;
			if (length != expected || !read_u32(data) ||
			    (read_u32(data + 4) != 1 && read_u32(data + 4) != 2) ||
			    read_u64(data + 8) || read_u32(data + 24) > 256 || read_u32(data + 28))
				return -EPROTO;
			break;
		case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_WAIT_IDLE:
			expected = KB2_GPU_DRM_AMDGPU_RECORD_GEM_WAIT_IDLE_REQUEST_SIZE;
			if (length != expected || !read_u32(data) || read_u32(data + 4))
				return -EPROTO;
			query.fence_deadline_ns = command.deadline_ns;
			break;
		case KB2_GPU_DRM_AMDGPU_COMMAND_WAIT_CS:
			expected = KB2_GPU_DRM_AMDGPU_RECORD_WAIT_CS_REQUEST_SIZE;
			if (length != expected)
				return -EPROTO;
			if (!command.deadline_ns ||
			    command.deadline_ns > INT64_MAX || !read_u32(data + 20))
				return -EPROTO;
			query.timeout_nsec = command.deadline_ns;
			break;
		case KB2_GPU_DRM_AMDGPU_COMMAND_VM:
			expected = KB2_GPU_DRM_AMDGPU_RECORD_VM_REQUEST_SIZE;
			if (length != expected)
				return -EPROTO;
			if ((read_u32(data) != 1 && read_u32(data) != 2) ||
			    read_u32(data + 4))
				return -EPROTO;
			break;
		default:
			return -EOPNOTSUPP;
		}
		if (!data || length != expected || command.counts[0] != 1 ||
		    command.counts[1] || command.counts[2] ||
		    (command.command_id != KB2_GPU_DRM_AMDGPU_COMMAND_WAIT_CS &&
		     command.command_id != KB2_GPU_DRM_AMDGPU_COMMAND_GEM_WAIT_IDLE &&
		     command.deadline_ns))
			return -EPROTO;
	} else if (command.command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID) {
		size_t expected = 0;

		switch (command.command_id) {
		case KB2_GPU_DRM_VIRTGPU_COMMAND_MAP:
			expected = KB2_GPU_DRM_VIRTGPU_RECORD_MAP_REQUEST_SIZE;
			break;
		case KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER:
			expected = KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_SIZE;
			break;
		case KB2_GPU_DRM_VIRTGPU_COMMAND_GETPARAM:
			expected = KB2_GPU_DRM_VIRTGPU_RECORD_GETPARAM_REQUEST_SIZE;
			break;
		case KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_CREATE:
			expected = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_SIZE;
			break;
		case KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_INFO:
			expected = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_INFO_REQUEST_SIZE;
			break;
		case KB2_GPU_DRM_VIRTGPU_COMMAND_TRANSFER_FROM_HOST_3D:
		case KB2_GPU_DRM_VIRTGPU_COMMAND_TRANSFER_TO_HOST_3D:
			expected = KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_SIZE;
			break;
		case KB2_GPU_DRM_VIRTGPU_COMMAND_WAIT:
			expected = KB2_GPU_DRM_VIRTGPU_RECORD_WAIT_REQUEST_SIZE;
			break;
		case KB2_GPU_DRM_VIRTGPU_COMMAND_GET_CAPS:
			expected = KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_REQUEST_SIZE;
			break;
		case KB2_GPU_DRM_VIRTGPU_COMMAND_CONTEXT_INIT:
			expected = KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_SIZE;
			break;
		default:
			break;
		}
		if (expected && (!data || length != expected))
			return -EPROTO;
		if (expected && command.command_id != KB2_GPU_DRM_VIRTGPU_COMMAND_GET_CAPS &&
		    command.command_id != KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER &&
		    command.command_id != KB2_GPU_DRM_VIRTGPU_COMMAND_CONTEXT_INIT &&
		    command.counts[1])
			return -EPROTO;
		if (command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_MAP) {
			query.mapping_handle = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_MAP_REQUEST_HANDLE_OFFSET);
			query.mapping_rights = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_MAP_REQUEST_MAPPING_RIGHTS_OFFSET);
			if (!query.mapping_handle || !query.mapping_rights ||
			    (query.mapping_rights & ~(KB2_GPU_SPAN_RIGHT_READ |
				KB2_GPU_SPAN_RIGHT_WRITE)) || command.counts[1] ||
			    command.counts[2])
				return -EPROTO;
		} else if (command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_CREATE &&
		    read_u32(data +
			KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_RESERVED_OFFSET))
			return -EPROTO;
		if (command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_INFO &&
		    read_u32(data +
			KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_INFO_REQUEST_RESERVED_OFFSET))
			return -EPROTO;
		if (command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_GET_CAPS) {
			kb2_gpu_argument_t argument;
			kb2_gpu_span_t span;
			size_t capacity = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_REQUEST_RESPONSE_CAPACITY_OFFSET);

			if (!capacity || capacity > KB2_GPU_DRM_VIRTGPU_MAX_CAPSET_BYTES ||
			    read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_REQUEST_RESERVED_OFFSET) ||
			    command.counts[0] != 2 || command.counts[1] != 1 ||
			    command.counts[2] ||
			    kb2_gpu_command_argument(&command, 1, &argument) ||
			    kb2_gpu_command_span(&command, 0, &span) ||
			    argument.argument_id !=
				KB2_GPU_DRM_VIRTGPU_COMMAND_GET_CAPS_REQUEST_SPAN_RESPONSE_ARGUMENT_ID ||
			    argument.kind != KB2_GPU_ARGUMENT_SPAN ||
			    argument.flags != KB2_GPU_ARGUMENT_FLAG_OUTPUT ||
			    argument.record_schema_id != KB2_GPU_DRM_VIRTGPU_RECORD_BYTE ||
			    argument.value != span.span_id || argument.count != capacity ||
			    span.region_id != region->region_id || span.offset ||
			    span.length != capacity || span.rights != KB2_GPU_SPAN_RIGHT_WRITE ||
			    span.record_schema_id != KB2_GPU_DRM_VIRTGPU_RECORD_BYTE ||
			    span.element_count != capacity ||
			    span.flags != KB2_GPU_SPAN_FLAG_OUTPUT)
				return -EPROTO;
			query.aux_size = capacity;
			query.aux_output = true;
		} else if (command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER) {
			uint32_t flags = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_FLAGS_OFFSET);
			uint32_t ring_index = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_RING_INDEX_OFFSET);
			size_t command_bytes = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_COMMAND_BYTES_OFFSET);
			size_t handle_count = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_BO_HANDLE_COUNT_OFFSET);
			size_t input_count = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_INPUT_SYNCOBJ_COUNT_OFFSET);
			size_t output_count = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_OUTPUT_SYNCOBJ_COUNT_OFFSET);
			size_t handle_offset = (command_bytes + 7) & ~(size_t)7;
			size_t handle_bytes, input_offset, input_bytes, output_offset;
			size_t output_bytes, vector_index = 1;

			if (!command_bytes ||
			    command_bytes > KB2_GPU_DRM_VIRTGPU_MAX_COMMAND_BYTES ||
			    handle_count > KB2_GPU_DRM_VIRTGPU_MAX_BO_HANDLES ||
			    input_count > KB2_GPU_DRM_VIRTGPU_MAX_SYNCOBJS ||
			    output_count > KB2_GPU_DRM_VIRTGPU_MAX_SYNCOBJS ||
			    handle_count > SIZE_MAX / sizeof(uint32_t) ||
			    input_count > SIZE_MAX /
				KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE ||
			    output_count > SIZE_MAX /
				KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE ||
			    (flags & ~(KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_FENCE_OUT |
				KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_RING_INDEX)) ||
			    (!(flags & KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_RING_INDEX) && ring_index) ||
			    handle_offset < command_bytes)
				return -EPROTO;
			handle_bytes = handle_count * sizeof(uint32_t);
			if (handle_offset > SIZE_MAX - handle_bytes)
				return -EPROTO;
			input_offset = (handle_offset + handle_bytes + 7) & ~(size_t)7;
			input_bytes = input_count *
				KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE;
			if (input_offset < handle_offset + handle_bytes ||
			    input_offset > SIZE_MAX - input_bytes)
				return -EPROTO;
			output_offset = input_offset + input_bytes;
			output_bytes = output_count *
				KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE;
			if (output_offset > SIZE_MAX - output_bytes ||
			    command.counts[0] != (uint32_t)(2 + !!handle_count +
				!!input_count + !!output_count) ||
			    command.counts[1] != (uint32_t)(1 + !!handle_count +
				!!input_count + !!output_count) || command.counts[2] ||
			    input_span(&command, region, vector_index++,
				KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER_REQUEST_SPAN_COMMAND_ARGUMENT_ID,
				KB2_GPU_DRM_VIRTGPU_RECORD_BYTE, 0, command_bytes, 1))
				return -EPROTO;
			if (handle_count && input_span(&command, region, vector_index++,
				KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER_REQUEST_SPAN_BO_HANDLES_ARGUMENT_ID,
				KB2_GPU_DRM_VIRTGPU_RECORD_HANDLE_U32, handle_offset,
				handle_count, sizeof(uint32_t)))
				return -EPROTO;
			if (input_count && input_span(&command, region, vector_index++,
				KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER_REQUEST_SPAN_INPUT_SYNCOBJS_ARGUMENT_ID,
				KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ, input_offset,
				input_count, KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE))
				return -EPROTO;
			if (output_count && input_span(&command, region, vector_index++,
				KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER_REQUEST_SPAN_OUTPUT_SYNCOBJS_ARGUMENT_ID,
				KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ, output_offset,
				output_count, KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE))
				return -EPROTO;
			query.exec_flags = flags;
			query.ring_index = ring_index;
			query.command_size = command_bytes;
			query.handles_offset = handle_offset;
			query.handle_count = handle_count;
			query.input_syncobjs_offset = input_offset;
			query.input_syncobj_count = input_count;
			query.output_syncobjs_offset = output_offset;
			query.output_syncobj_count = output_count;
			query.aux_size = input_count || output_count ?
				output_offset + output_bytes : handle_offset + handle_bytes;
			query.aux_input = true;
		} else if (command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_WAIT) {
			if (!read_u32(data) ||
			    (read_u32(data + 4) & ~1U) || !command.deadline_ns ||
			    command.counts[0] != 1 || command.counts[1] ||
			    command.counts[2])
				return -EPROTO;
		} else if (command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_CONTEXT_INIT) {
			uint32_t mask = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_PARAMETER_MASK_OFFSET);
			uint32_t capset_id = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_CAPSET_ID_OFFSET);
			uint32_t ring_count = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_RING_COUNT_OFFSET);
			size_t debug_bytes = read_u32(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_DEBUG_NAME_BYTES_OFFSET);
			uint64_t poll_mask = read_u64(data +
				KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_POLL_RING_MASK_OFFSET);
			uint32_t known = KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_CAPSET_ID |
				KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_RING_COUNT |
				KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_POLL_RING_MASK |
				KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_DEBUG_NAME;

			if (!mask || (mask & ~known) || capset_id > 63 || ring_count > 64 ||
			    (!(mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_CAPSET_ID) && capset_id) ||
			    (!(mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_RING_COUNT) && ring_count) ||
			    (!(mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_POLL_RING_MASK) && poll_mask) ||
			    ((mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_POLL_RING_MASK) &&
			     (!(mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_RING_COUNT) || !ring_count ||
			      (ring_count < 64 && (poll_mask >> ring_count)))) ||
			    debug_bytes > KB2_GPU_DRM_VIRTGPU_MAX_DEBUG_NAME_BYTES ||
			    (!!(mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_DEBUG_NAME) !=
			     !!debug_bytes) || command.counts[0] != 1 + !!debug_bytes ||
			    command.counts[1] != !!debug_bytes || command.counts[2])
				return -EPROTO;
			if (debug_bytes) {
				kb2_gpu_argument_t argument;
				kb2_gpu_span_t span;

				if (kb2_gpu_command_argument(&command, 1, &argument) ||
				    kb2_gpu_command_span(&command, 0, &span) ||
				    argument.argument_id !=
					KB2_GPU_DRM_VIRTGPU_COMMAND_CONTEXT_INIT_REQUEST_SPAN_DEBUG_NAME_ARGUMENT_ID ||
				    argument.kind != KB2_GPU_ARGUMENT_SPAN ||
				    argument.flags != KB2_GPU_ARGUMENT_FLAG_INPUT ||
				    argument.record_schema_id != KB2_GPU_DRM_VIRTGPU_RECORD_BYTE ||
				    argument.value != span.span_id || argument.count != debug_bytes ||
				    span.region_id != region->region_id || span.offset ||
				    span.length != debug_bytes ||
				    span.rights != KB2_GPU_SPAN_RIGHT_READ ||
				    span.record_schema_id != KB2_GPU_DRM_VIRTGPU_RECORD_BYTE ||
				    span.element_count != debug_bytes ||
				    span.flags != KB2_GPU_SPAN_FLAG_INPUT)
					return -EPROTO;
			}
			query.aux_size = debug_bytes;
			query.aux_input = true;
		}
	}
	if (query.command_set_id == KB2_GPU_DRM_MODE_SET_ID) {
		size_t vectors = 0, strides[4] = {4, 4, 4, 4};
		size_t limits[4] = {16, 16, 16, 16};
		switch (query.command_id) {
		case KB2_GPU_DRM_MODE_COMMAND_GET_RESOURCES: vectors = 4; break;
		case KB2_GPU_DRM_MODE_COMMAND_GET_PLANE_RESOURCES:
			vectors = 1; limits[0] = 32; break;
		case KB2_GPU_DRM_MODE_COMMAND_GET_PLANE:
			vectors = 1; limits[0] = 128; break;
		case KB2_GPU_DRM_MODE_COMMAND_OBJECT_GET_PROPERTIES:
			vectors = 1; limits[0] = 32; strides[0] = 16; break;
		case KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY:
			vectors = 2; limits[0] = 8; limits[1] = 32;
			strides[0] = 8; strides[1] = 40; break;
		default: break;
		}
		bool large = false;
		for (index = 0; index < vectors; index++) {
			if (query.capacity[index] > 65536)
				return -EPROTO;
			large |= query.capacity[index] > limits[index];
		}
		if (large) {
			size_t next = 0;
			for (index = 0; index < vectors; index++) {
				if (query.capacity[index] && query.offset[index] != next)
					return -EPROTO;
				next += query.capacity[index] * strides[index];
			}
			if (query.output_size != next || region->length != next)
				return -EPROTO;
			/* Large topology arrays share the existing auxiliary transport;
			 * otherwise the fixed reply buffer would truncate count/retry. */
			query.aux_size = next;
			query.aux_output = true;
			query.output_size = 0;
		}
	}
	*out = query;
	return 0;
}

int kobox_drm_query_prepare(struct kobox_drm_query *out, uint64_t generation,
			    uint64_t session_id, uint32_t queue_class,
			    const unsigned char *bytes,
			    size_t size, const kb2_gpu_region_t *region)
{
	return kobox_drm_query_prepare_profile(out, generation, session_id,
		KB2_GPU_PROFILE_VIRGL, queue_class, bytes, size, region);
}

static uint32_t command_status(int result)
{
	switch (result) {
	case 0: return KB2_GPU_STATUS_OK;
	case -EINVAL: return KB2_GPU_STATUS_INVALID;
	case -ENOTTY:
	case -ENOSYS:
	case -EOPNOTSUPP: return KB2_GPU_STATUS_UNSUPPORTED;
	case -EACCES:
	case -EPERM: return KB2_GPU_STATUS_DENIED;
	case -ENOENT: return KB2_GPU_STATUS_NOT_FOUND;
	case -ENOMEM: return KB2_GPU_STATUS_NO_MEMORY;
	case -ENOSPC: return KB2_GPU_STATUS_LIMIT;
	case -EBUSY: return KB2_GPU_STATUS_BUSY;
	case -ETIME:
	case -ETIMEDOUT: return KB2_GPU_STATUS_TIMED_OUT;
	case -ECANCELED: return KB2_GPU_STATUS_CANCELED;
	default: return KB2_GPU_STATUS_DEVICE_LOST;
	}
}

int kobox_drm_query_execute_service(const struct kobox_drm_query *query,
			   const struct kobox_drm_query_api *api,
			   struct kobox_linux_drm_service *service,
			   uint64_t file_cookie,
			   struct kobox_linux_drm_file *file,
			   unsigned char *output, size_t output_size,
			   unsigned char *aux, size_t aux_size,
			   unsigned char *completion, size_t completion_capacity,
			   size_t *completion_size,
			   struct kobox_drm_query_result *query_result)
{
	struct kobox_linux_drm_version version;
	unsigned char record[KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_RESULT_SIZE] = {0};
	kb2_gpu_inline_completion_t reply = {0};
	uint64_t value;
	size_t index;
	int result;

	if (!query || !api || !file ||
	    (!output && query->output_size) || output_size < query->output_size ||
	    aux_size != query->aux_size || (aux_size && !aux) || (!aux_size && aux) ||
	    !completion ||
	    !completion_size || completion_capacity < KB2_GPU_COMPLETION_HEADER_SIZE +
		KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE + sizeof(record))
		return -EINVAL;
	reply.session_id = query->session_id;
	for (index = 0; index < 4; index++) {
		size_t extent = query->aux_output ? aux_size : output_size;
		if (query->offset[index] > extent ||
		    query->capacity[index] > extent - query->offset[index])
			return -EPROTO;
	}
	if (query_result)
		*query_result = (struct kobox_drm_query_result) {0};
	if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
	    query->command_id == KB2_GPU_DRM_CORE_COMMAND_VERSION) {
		if (!api->version)
			return -EINVAL;
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
			reply.length = KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_CORE_COMMAND_GET_CLIENT) {
		struct kobox_linux_drm_client client;

		if (!api->get_client)
			return -EINVAL;
		result = api->get_client(file, (int32_t)read_u32(query->inline_data),
			&client);
		if (!result) {
			if (client.authenticated > 1)
				return -EPROTO;
			write_u32(record + KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_AUTHENTICATED_OFFSET,
				client.authenticated);
			write_u64(record + KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_PROCESS_ID_OFFSET,
				client.process_id);
			write_u64(record + KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_USER_ID_OFFSET,
				client.user_id);
			write_u64(record + KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_MAGIC_OFFSET,
				client.magic);
			write_u64(record + KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_IOCTL_COUNT_OFFSET,
				client.ioctl_count);
			reply.record_schema_id = KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT;
			reply.length = KB2_GPU_DRM_CORE_RECORD_CLIENT_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_CORE_COMMAND_GET_CAP) {
		if (!api->get_cap)
			return -EINVAL;
		result = api->get_cap(file, query->capability, &value);
		if (!result) {
			write_u64(record, value);
			reply.record_schema_id = KB2_GPU_DRM_CORE_RECORD_SCALAR_U64;
			reply.length = KB2_GPU_DRM_CORE_RECORD_SCALAR_U64_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_CORE_COMMAND_POLL_EVENTS) {
		uint32_t ready = 0;

		if (!api->poll_events)
			return -EINVAL;
		result = api->poll_events(file, query->poll_events, &ready);
		if (!result) {
			if (ready & ~KB2_GPU_DRM_CORE_POLL_READABLE)
				return -EPROTO;
			write_u32(record, ready);
			reply.record_schema_id = KB2_GPU_DRM_CORE_RECORD_SCALAR_U32;
			reply.length = KB2_GPU_DRM_CORE_RECORD_SCALAR_U32_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_CORE_COMMAND_READ_EVENTS) {
		size_t bytes = 0;

		if (!api->read_events)
			return -EINVAL;
		result = api->read_events(file, output + query->offset[0],
			query->capacity[0], &bytes);
		if (!result) {
			if (bytes > query->capacity[0] || bytes > UINT32_MAX)
				return -EPROTO;
			write_u32(record, bytes);
			write_u32(record + 4, bytes);
			reply.record_schema_id = KB2_GPU_DRM_CORE_RECORD_LENGTH_RESULT;
			reply.length = KB2_GPU_DRM_CORE_RECORD_LENGTH_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_CORE_COMMAND_GEM_CLOSE) {
		if (!api->gem_close)
			return -EINVAL;
		result = api->gem_close(file, read_u32(query->inline_data));
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id ==
			KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT) {
		struct kobox_linux_drm_service_prime prime;

		if (!api->prime_export || !query_result)
			result = -EINVAL;
		else if (!service)
			result = -ENODEV;
		else if (!file_cookie)
			result = -ENOENT;
		else if (!query->mapping_page_capacity)
			result = -ENOSPC;
		else if (query->mapping_page_capacity > aux_size / sizeof(uint64_t))
			result = -ENOMEM;
		else
			result = api->prime_export(service, file_cookie,
				query->prime_handle, query->prime_flags,
				(uint64_t *)aux, query->mapping_page_capacity, &prime);
		if (!result) {
			const uint64_t rights = KB2_GPU_SPAN_RIGHT_READ |
				KB2_GPU_SPAN_RIGHT_WRITE;
			const kb2_gpu_attachment_completion_t exported = {
				.session_id = query->session_id,
				.status = KB2_GPU_STATUS_OK,
				.argument_id =
					KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT_COMPLETION_ATTACHMENT_DMA_BUFFER_ARGUMENT_ID,
				.attachment = {
					.attachment_id = 1,
					.object_class = KB2_GPU_ATTACHMENT_DMA_BUF,
					.exchange_id = prime.prime_id,
					.generation = query->generation,
					.rights = rights,
					.role =
						KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT_COMPLETION_ATTACHMENT_DMA_BUFFER_ROLE,
					.ownership = KB2_GPU_ATTACHMENT_MOVE,
					.flags = KB2_GPU_ATTACHMENT_FLAG_OUTPUT,
				},
			};
			*query_result = (struct kobox_drm_query_result) {
				.mapping_id = prime.prime_id,
				.mapping_length = prime.length,
				.mapping_page_count = prime.page_count,
				.mapping_rights = rights,
				.mapping_cache_policy = prime.cache_policy,
				.mapping_backing_kind = prime.backing_kind,
				.attachment_class = KB2_GPU_ATTACHMENT_DMA_BUF,
			};
			return kb2_gpu_attachment_completion_encode(completion,
				completion_capacity, completion_size, &exported) ==
				KB2_PROTOCOL_OK ? 0 : -EPROTO;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id ==
			KB2_GPU_DRM_CORE_COMMAND_PRIME_ATTACHMENT_TO_HANDLE) {
		uint32_t handle = 0;

		if (!api->prime_import)
			return -EINVAL;
		if (!service)
			result = -ENODEV;
		else if (!file_cookie)
			result = -ENOENT;
		else
			result = api->prime_import(service, file_cookie,
				query->prime_token, &handle);
		if (!result) {
			if (!handle)
				return -EPROTO;
			write_u32(record, handle);
			reply.record_schema_id =
				KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_HANDLE_RESULT;
			reply.length = KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_HANDLE_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_CREATE) {
		uint32_t handle = 0;

		if (!api->syncobj_create)
			return -EINVAL;
		result = api->syncobj_create(file, query->syncobj_flags, &handle);
		if (!result) {
			if (!handle)
				return -EPROTO;
			write_u32(record, handle);
			reply.record_schema_id = KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_HANDLE_RESULT;
			reply.length = KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_HANDLE_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_DESTROY) {
		if (!api->syncobj_destroy)
			return -EINVAL;
		result = api->syncobj_destroy(file, read_u32(query->inline_data));
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_WAIT) {
		uint32_t first = 0;

		if (!api->syncobj_wait)
			return -EINVAL;
		result = api->syncobj_wait(file, aux, query->handle_count,
			query->timeout_nsec, query->syncobj_flags,
			query->fence_deadline_ns, &first);
		if (!result) {
			if (first >= query->handle_count)
				return -EPROTO;
			write_u32(record, first);
			reply.record_schema_id = KB2_GPU_DRM_CORE_RECORD_WAIT_RESULT;
			reply.length = KB2_GPU_DRM_CORE_RECORD_WAIT_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   (query->command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_RESET ||
		    query->command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_SIGNAL)) {
		if (!api->syncobj_array)
			return -EINVAL;
		result = api->syncobj_array(file, aux, query->handle_count,
			query->command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_SIGNAL);
	} else if (query->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_CORE_COMMAND_SET_CLIENT_CAP) {
		if (!api->set_client_cap)
			return -EINVAL;
		result = api->set_client_cap(file,
			read_u64(query->inline_data +
				KB2_GPU_DRM_CORE_RECORD_CLIENT_CAP_REQUEST_CAPABILITY_OFFSET),
			read_u64(query->inline_data +
				KB2_GPU_DRM_CORE_RECORD_CLIENT_CAP_REQUEST_VALUE_OFFSET));
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_MAGIC) {
		uint32_t magic;

		if (!api->get_magic)
			return -EINVAL;
		result = api->get_magic(file, &magic);
		if (!result) {
			write_u32(record, magic);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_MAGIC;
			reply.length = KB2_GPU_DRM_MODE_RECORD_MAGIC_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_AUTH_MAGIC) {
		if (!api->auth_magic)
			return -EINVAL;
		result = api->auth_magic(file, read_u32(query->inline_data));
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   (query->command_id == KB2_GPU_DRM_MODE_COMMAND_SET_MASTER ||
		    query->command_id == KB2_GPU_DRM_MODE_COMMAND_DROP_MASTER)) {
		if (!api->master)
			return -EINVAL;
		result = api->master(file,
			query->command_id == KB2_GPU_DRM_MODE_COMMAND_SET_MASTER);
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_WAIT_VBLANK) {
		struct kobox_linux_drm_vblank vblank;

		if (!api->wait_vblank)
			return -EINVAL;
		result = api->wait_vblank(file, query->mode_flags,
			(uint32_t)query->sequence, query->event_token, &vblank);
		if (!result) {
			write_u32(record, vblank.type);
			write_u32(record + 4, vblank.sequence);
			write_u64(record + 8, vblank.timestamp_ns);
			reply.record_schema_id =
				KB2_GPU_DRM_MODE_RECORD_WAIT_VBLANK_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_WAIT_VBLANK_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_CRTC_GET_SEQUENCE) {
		struct kobox_linux_drm_crtc_sequence sequence;

		if (!api->crtc_get_sequence)
			return -EINVAL;
		result = api->crtc_get_sequence(file, query->object_id, &sequence);
		if (!result) {
			if (sequence.active > 1)
				return -EPROTO;
			write_u32(record, sequence.active);
			write_u64(record + 8, sequence.sequence);
			write_u64(record + 16, sequence.timestamp_ns);
			reply.record_schema_id =
				KB2_GPU_DRM_MODE_RECORD_CRTC_SEQUENCE_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_CRTC_SEQUENCE_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_CRTC_QUEUE_SEQUENCE) {
		uint64_t sequence = 0;

		if (!api->crtc_queue_sequence)
			return -EINVAL;
		result = api->crtc_queue_sequence(file, query->object_id,
			query->mode_flags, query->sequence, query->event_token,
			&sequence);
		if (!result) {
			write_u64(record, sequence);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_SEQUENCE_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_SEQUENCE_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_FB) {
		struct kobox_linux_drm_fb fb;

		if (!api->get_fb)
			return -EINVAL;
		result = api->get_fb(file, query->object_id, &fb);
		if (!result) {
			if (fb.fb_id != query->object_id)
				return -EPROTO;
			write_u64(record, 1);
			write_u32(record + 8, fb.fb_id);
			write_u32(record + 12, fb.width);
			write_u32(record + 16, fb.height);
			write_u32(record + 20, fb.pitch);
			write_u32(record + 24, fb.bits_per_pixel);
			write_u32(record + 28, fb.depth);
			write_u32(record + 32, fb.handle);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_FB_LEGACY;
			reply.length = KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_FB2) {
		struct kobox_linux_drm_fb2 fb;
		size_t index;

		if (!api->get_fb2)
			return -EINVAL;
		result = api->get_fb2(file, query->object_id, &fb);
		if (!result) {
			if (fb.fb_id != query->object_id)
				return -EPROTO;
			write_u64(record, 1);
			write_u32(record + 8, fb.fb_id);
			write_u32(record + 12, fb.width);
			write_u32(record + 16, fb.height);
			write_u32(record + 20, fb.pixel_format);
			write_u32(record + 24, fb.flags);
			for (index = 0; index < 4; index++) {
				write_u32(record + 28 + index * 4, fb.handles[index]);
				write_u32(record + 44 + index * 4, fb.pitches[index]);
				write_u32(record + 60 + index * 4, fb.offsets[index]);
				write_u64(record + 76 + index * 8, fb.modifiers[index]);
			}
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_FB2;
			reply.length = KB2_GPU_DRM_MODE_RECORD_FB2_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_RESOURCES) {
		struct kobox_linux_drm_resources resources;
		unsigned char *target = query->aux_output ? aux : output;

		if (!api->resources)
			return -EINVAL;
		result = api->resources(file,
			query->capacity[0] ? (uint32_t *)(target + query->offset[0]) : NULL,
			query->capacity[0],
			query->capacity[1] ? (uint32_t *)(target + query->offset[1]) : NULL,
			query->capacity[1],
			query->capacity[2] ? (uint32_t *)(target + query->offset[2]) : NULL,
			query->capacity[2],
			query->capacity[3] ? (uint32_t *)(target + query->offset[3]) : NULL,
			query->capacity[3], &resources);
		if (!result) {
			write_u64(record, 1);
			write_u32(record + 8, resources.fb_count);
			write_u32(record + 12, resources.crtc_count);
			write_u32(record + 16, resources.connector_count);
			write_u32(record + 20, resources.encoder_count);
			write_u32(record + 24, resources.min_width);
			write_u32(record + 28, resources.max_width);
			write_u32(record + 32, resources.min_height);
			write_u32(record + 36, resources.max_height);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_RESOURCES_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_RESOURCES_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_CONNECTOR) {
		struct kobox_linux_drm_connector connector;
		unsigned char *target = query->aux_output ? aux : output;

		if (!api->connector)
			return -EINVAL;
		result = api->connector(file, query->object_id,
			query->capacity[0] ?
				(struct kobox_linux_drm_mode *)(target + query->offset[0]) : NULL,
			query->capacity[0],
			query->capacity[1] ?
				(struct kobox_linux_drm_property_value *)(target + query->offset[1]) : NULL,
			query->capacity[1],
			query->capacity[2] ?
				(uint32_t *)(target + query->offset[2]) : NULL,
			query->capacity[2], &connector);
		if (!result) {
			if (connector.connector_id != query->object_id)
				return -EPROTO;
			/* Linux returns required counts and leaves undersized vectors
			 * untouched, so the caller can allocate and retry. */
			write_u64(record, 1);
			write_u32(record + 8, connector.encoder_id);
			write_u32(record + 12, connector.connector_id);
			write_u32(record + 16, connector.connector_type);
			write_u32(record + 20, connector.connector_type_id);
			write_u32(record + 24, connector.connection);
			write_u32(record + 28, connector.width_mm);
			write_u32(record + 32, connector.height_mm);
			write_u32(record + 36, connector.subpixel);
			write_u32(record + 40, connector.mode_count);
			write_u32(record + 44, connector.property_count);
			write_u32(record + 48, connector.encoder_count);
			reply.record_schema_id =
				KB2_GPU_DRM_MODE_RECORD_CONNECTOR_RESULT;
			reply.length =
				KB2_GPU_DRM_MODE_RECORD_CONNECTOR_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_ENCODER) {
		struct kobox_linux_drm_encoder encoder;

		if (!api->encoder)
			return -EINVAL;
		result = api->encoder(file, query->object_id, &encoder);
		if (!result) {
			if (encoder.encoder_id != query->object_id)
				return -EPROTO;
			write_u64(record, 1);
			write_u32(record + 8, encoder.encoder_id);
			write_u32(record + 12, encoder.encoder_type);
			write_u32(record + 16, encoder.crtc_id);
			write_u32(record + 20, encoder.possible_crtcs);
			write_u32(record + 24, encoder.possible_clones);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_ENCODER_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_ENCODER_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_CRTC) {
		struct kobox_linux_drm_crtc crtc;

		if (!api->get_crtc)
			return -EINVAL;
		result = api->get_crtc(file, query->object_id, &crtc);
		if (!result) {
			if (crtc.crtc_id != query->object_id || crtc.mode_valid > 1)
				return -EPROTO;
			memcpy(output + query->offset[0], &crtc.mode,
			       sizeof(crtc.mode));
			write_u64(record, 1);
			write_u32(record + 8, crtc.crtc_id);
			write_u32(record + 12, crtc.fb_id);
			write_u32(record + 16, crtc.x);
			write_u32(record + 20, crtc.y);
			write_u32(record + 24, crtc.gamma_size);
			write_u32(record + 28, crtc.mode_valid);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_CRTC_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_CRTC_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_SET_CRTC) {
		struct kobox_linux_drm_mode mode;
		const unsigned char *mode_bytes = query->mode_flags ? aux +
			query->capacity[0] * sizeof(uint32_t) : NULL;

		if (!api->set_crtc)
			return -EINVAL;
		for (index = 0; index < query->capacity[0]; index++)
			if (!read_u32(aux + index * sizeof(uint32_t)))
				return -EPROTO;
		if (query->mode_flags) {
			mode = (struct kobox_linux_drm_mode) {
				.clock = read_u32(mode_bytes),
				.hdisplay = read_u16(mode_bytes + 4),
				.hsync_start = read_u16(mode_bytes + 6),
				.hsync_end = read_u16(mode_bytes + 8),
				.htotal = read_u16(mode_bytes + 10),
				.hskew = read_u16(mode_bytes + 12),
				.vdisplay = read_u16(mode_bytes + 14),
				.vsync_start = read_u16(mode_bytes + 16),
				.vsync_end = read_u16(mode_bytes + 18),
				.vtotal = read_u16(mode_bytes + 20),
				.vscan = read_u16(mode_bytes + 22),
				.vrefresh = read_u32(mode_bytes + 24),
				.flags = read_u32(mode_bytes + 28),
				.type = read_u32(mode_bytes + 32),
			};
			memcpy(mode.name, mode_bytes + 36, sizeof(mode.name));
		}
		result = api->set_crtc(file, query->object_id,
			read_u32(query->inline_data + 12),
			read_u32(query->inline_data + 16),
			read_u32(query->inline_data + 20),
			query->capacity[0] ? (const uint32_t *)aux : NULL,
			query->capacity[0], query->mode_flags ? &mode : NULL);
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   (query->command_id == KB2_GPU_DRM_MODE_COMMAND_CURSOR ||
		    query->command_id == KB2_GPU_DRM_MODE_COMMAND_CURSOR2)) {
		bool cursor2 = query->command_id == KB2_GPU_DRM_MODE_COMMAND_CURSOR2;
		struct kobox_linux_drm_cursor cursor = {
			.flags = read_u32(query->inline_data + 8),
			.crtc_id = read_u32(query->inline_data + 12),
			.x = (int32_t)read_u32(query->inline_data + 16),
			.y = (int32_t)read_u32(query->inline_data + 20),
			.width = read_u32(query->inline_data + 24),
			.height = read_u32(query->inline_data + 28),
			.handle = read_u32(query->inline_data + 32),
			.hot_x = cursor2 ? (int32_t)read_u32(query->inline_data + 36) : 0,
			.hot_y = cursor2 ? (int32_t)read_u32(query->inline_data + 40) : 0,
		};

		if (!api->cursor)
			return -EINVAL;
		result = api->cursor(file, &cursor);
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_PAGE_FLIP) {
		if (!api->page_flip)
			return -EINVAL;
		result = api->page_flip(file,
			read_u32(query->inline_data + 8),
			read_u32(query->inline_data + 12),
			read_u32(query->inline_data + 16),
			read_u32(query->inline_data + 20),
			read_u64(query->inline_data + 24));
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_DIRTY_FB) {
		if (!api->dirty_fb)
			return -EINVAL;
		result = api->dirty_fb(file, query->object_id,
			query->mode_flags,
			read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_COLOR_OFFSET),
			query->capacity[0] ?
				(const struct kobox_linux_drm_rectangle *)aux : NULL,
			query->capacity[0]);
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_CREATE_DUMB) {
		struct kobox_linux_drm_dumb_buffer buffer = {
			.height = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_HEIGHT_OFFSET),
			.width = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_WIDTH_OFFSET),
			.bits_per_pixel = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_BITS_PER_PIXEL_OFFSET),
			.flags = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_FLAGS_OFFSET),
		};

		if (!api->create_dumb)
			return -EINVAL;
		result = api->create_dumb(file, &buffer);
		if (!result) {
			if (!buffer.handle || !buffer.pitch || !buffer.size)
				return -EPROTO;
			write_u32(record +
				KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT_HANDLE_OFFSET,
				buffer.handle);
			write_u32(record +
				KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT_PITCH_OFFSET,
				buffer.pitch);
			write_u64(record +
				KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT_SIZE_OFFSET,
				buffer.size);
			reply.record_schema_id =
				KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT;
			reply.length =
				KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_ADD_FB) {
		struct kobox_linux_drm_fb framebuffer = {
			.width = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_WIDTH_OFFSET),
			.height = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_HEIGHT_OFFSET),
			.pitch = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_PITCH_OFFSET),
			.bits_per_pixel = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_BITS_PER_PIXEL_OFFSET),
			.depth = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_DEPTH_OFFSET),
			.handle = read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_FB_LEGACY_CREATE_HANDLE_OFFSET),
		};

		if (!api->add_fb)
			return -EINVAL;
		result = api->add_fb(file, &framebuffer);
		if (!result) {
			if (!framebuffer.fb_id)
				return -EPROTO;
			write_u64(record, 1);
			write_u32(record + 8, framebuffer.fb_id);
			reply.record_schema_id =
				KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_REMOVE_FB) {
		if (!api->remove_fb)
			return -EINVAL;
		result = api->remove_fb(file, read_u32(query->inline_data +
			KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_REQUEST_OBJECT_ID_OFFSET));
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_DESTROY_DUMB) {
		if (!api->destroy_dumb)
			return -EINVAL;
		result = api->destroy_dumb(file, read_u32(query->inline_data));
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_SET_PROPERTY) {
		if (!api->set_property)
			return -EINVAL;
		result = api->set_property(file,
			read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_SET_PROPERTY_REQUEST_CONNECTOR_ID_OFFSET),
			read_u32(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_SET_PROPERTY_REQUEST_PROPERTY_ID_OFFSET),
			read_u64(query->inline_data +
				KB2_GPU_DRM_MODE_RECORD_SET_PROPERTY_REQUEST_VALUE_OFFSET));
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_SET_PLANE) {
		const unsigned char *data = query->inline_data;
		const struct kobox_linux_drm_plane_set plane = {
			.plane_id = read_u32(data + 8),
			.crtc_id = read_u32(data + 12),
			.fb_id = read_u32(data + 16),
			.flags = read_u32(data + 20),
			.crtc_x = (int32_t)read_u32(data + 24),
			.crtc_y = (int32_t)read_u32(data + 28),
			.crtc_width = read_u32(data + 32),
			.crtc_height = read_u32(data + 36),
			.source_x = read_u32(data + 40),
			.source_y = read_u32(data + 44),
			.source_width = read_u32(data + 48),
			.source_height = read_u32(data + 52),
		};
		if (!api->set_plane)
			return -EINVAL;
		result = api->set_plane(file, &plane);
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   (query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_GAMMA ||
		    query->command_id == KB2_GPU_DRM_MODE_COMMAND_SET_GAMMA)) {
		const size_t one = (size_t)query->gamma_count * sizeof(uint16_t);
		const bool set = query->command_id == KB2_GPU_DRM_MODE_COMMAND_SET_GAMMA;
		if (!api->gamma || !aux || aux_size != one * 3)
			return -EINVAL;
		result = api->gamma(file, set, query->object_id, query->gamma_count,
			(uint16_t *)aux, (uint16_t *)(aux + one),
			(uint16_t *)(aux + one * 2));
		if (!result && !set) {
			write_u32(record, query->gamma_count);
			write_u32(record + 4, query->gamma_count);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_COUNT_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_COUNT_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_ADD_FB2) {
		struct kobox_linux_drm_fb2 framebuffer = {
			.width = read_u32(query->inline_data + 12),
			.height = read_u32(query->inline_data + 16),
			.pixel_format = read_u32(query->inline_data + 20),
			.flags = read_u32(query->inline_data + 24),
		};

		if (!api->add_fb2)
			return -EINVAL;
		for (index = 0; index < 4; index++) {
			framebuffer.handles[index] =
				read_u32(query->inline_data + 28 + index * 4);
			framebuffer.pitches[index] =
				read_u32(query->inline_data + 44 + index * 4);
			framebuffer.offsets[index] =
				read_u32(query->inline_data + 60 + index * 4);
			framebuffer.modifiers[index] =
				read_u64(query->inline_data + 76 + index * 8);
		}
		result = api->add_fb2(file, &framebuffer);
		if (!result) {
			if (!framebuffer.fb_id)
				return -EPROTO;
			write_u64(record, 1);
			write_u32(record + 8, framebuffer.fb_id);
			reply.record_schema_id =
				KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id ==
			KB2_GPU_DRM_MODE_COMMAND_OBJECT_GET_PROPERTIES) {
		uint32_t property_count = 0;
		unsigned char *target = query->aux_output ? aux : output;

		if (!api->object_properties)
			return -EINVAL;
		result = api->object_properties(file, query->object_id,
			query->object_type,
			query->capacity[0] ?
				(struct kobox_linux_drm_property_value *)
				(target + query->offset[0]) : NULL,
			query->capacity[0], &property_count);
		if (!result) {
			const uint32_t count = property_count < query->capacity[0] ?
				property_count : query->capacity[0];
			write_u64(record, 1);
			write_u32(record +
				KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT_COUNT_OFFSET,
				count);
			write_u32(record +
				KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT_REQUIRED_COUNT_OFFSET,
				property_count);
			reply.record_schema_id =
				KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT;
			reply.length =
				KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_PLANE_RESOURCES) {
		uint32_t count = 0;
		unsigned char *target = query->aux_output ? aux : output;

		if (!api->plane_resources)
			return -EINVAL;
		result = api->plane_resources(file,
			query->capacity[0] ? (uint32_t *)(target + query->offset[0]) : NULL,
			query->capacity[0], &count);
		if (!result) {
			write_u64(record, 1);
			write_u32(record + 8, count < query->capacity[0] ?
				  count : query->capacity[0]);
			write_u32(record + 12, count);
			reply.record_schema_id =
				KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_TOPOLOGY_COUNT_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_PLANE) {
		struct kobox_linux_drm_plane plane;
		unsigned char *target = query->aux_output ? aux : output;

		if (!api->plane)
			return -EINVAL;
		result = api->plane(file, query->object_id,
			query->capacity[0] ? (uint32_t *)(target + query->offset[0]) : NULL,
			query->capacity[0], &plane);
		if (!result) {
			write_u64(record, 1);
			write_u32(record + 8, plane.plane_id);
			write_u32(record + 12, plane.crtc_id);
			write_u32(record + 16, plane.fb_id);
			write_u32(record + 20, plane.possible_crtcs);
			write_u32(record + 24, plane.gamma_size);
			write_u32(record + 28, plane.format_count);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_PLANE_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_PLANE_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY) {
		struct kobox_linux_drm_property property;
		unsigned char *target = query->aux_output ? aux : output;

		if (!api->property)
			return -EINVAL;
		result = api->property(file, query->object_id,
			query->capacity[0] ? (uint64_t *)(target + query->offset[0]) : NULL,
			query->capacity[0],
			query->capacity[1] ? (struct kobox_linux_drm_property_enum *)
				(target + query->offset[1]) : NULL,
			query->capacity[1], &property);
		if (!result) {
			write_u64(record, 1);
			write_u32(record + 8, property.flags);
			write_u32(record + 12, property.value_count);
			write_u32(record + 16, property.enum_count);
			memcpy(record + 24, property.name, sizeof(property.name));
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_PROPERTY_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_PROPERTY_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY_BLOB) {
		uint32_t required = 0;

		if (!api->property_blob)
			return -EINVAL;
		result = api->property_blob(file, query->object_id,
			query->capacity[0] ?
				(query->aux_output ? aux : output) + query->offset[0] : NULL,
			query->capacity[0], &required);
		if (!result) {
			/* Linux copies a property blob only for an exact length match. */
			write_u32(record, required == query->capacity[0] ? required : 0);
			write_u32(record + 4, required);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_LENGTH_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_LENGTH_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_CREATE_PROPERTY_BLOB) {
		uint32_t blob_id = 0;

		if (!api->create_property_blob)
			return -EINVAL;
		result = api->create_property_blob(file, aux, query->aux_size, &blob_id);
		if (!result) {
			if (!blob_id)
				return -EPROTO;
			write_u64(record, 1);
			write_u32(record + 8, blob_id);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_OBJECT_ID_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_DESTROY_PROPERTY_BLOB) {
		if (!api->destroy_property_blob)
			return -EINVAL;
		result = api->destroy_property_blob(file, query->object_id);
	} else if (query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		   query->command_id == KB2_GPU_DRM_MODE_COMMAND_ATOMIC) {
		struct kobox_linux_drm_atomic_object objects[32];
		struct kobox_linux_drm_atomic_property properties[192];
		size_t cursor = 0, base = query->capacity[0] *
			KB2_GPU_DRM_MODE_RECORD_ATOMIC_OBJECT_SIZE;

		if (!api->atomic)
			return -EINVAL;
		for (index = 0; index < query->capacity[0]; index++) {
			const unsigned char *item = aux + index *
				KB2_GPU_DRM_MODE_RECORD_ATOMIC_OBJECT_SIZE;
			objects[index] = (struct kobox_linux_drm_atomic_object) {
				.object_id = read_u32(item),
				.first_property = read_u32(item + 4),
				.property_count = read_u32(item + 8),
			};
			if (!objects[index].object_id || !objects[index].property_count ||
			    objects[index].first_property != cursor ||
			    read_u32(item + 12) ||
			    objects[index].property_count > query->capacity[1] - cursor)
				return -EPROTO;
			cursor += objects[index].property_count;
		}
		if (cursor != query->capacity[1])
			return -EPROTO;
		for (index = 0; index < query->capacity[1]; index++) {
			const unsigned char *item = aux + base + index *
				KB2_GPU_DRM_MODE_RECORD_ATOMIC_PROPERTY_SIZE;
			properties[index] = (struct kobox_linux_drm_atomic_property) {
				.property_id = read_u32(item),
				.value_kind = read_u32(item + 4),
				.value = read_u64(item + 8),
			};
			if (!properties[index].property_id ||
			    properties[index].value_kind < 1 ||
			    properties[index].value_kind > 3 ||
			    read_u32(item + 16) || read_u32(item + 20) ||
			    read_u64(item + 24))
				return -EPROTO;
		}
		result = api->atomic(file, query->mode_flags, objects,
			query->capacity[0], properties, query->capacity[1],
			query->event_token);
		if (!result) {
			write_u64(record, 1);
			reply.record_schema_id = KB2_GPU_DRM_MODE_RECORD_ATOMIC_RESULT;
			reply.length = KB2_GPU_DRM_MODE_RECORD_ATOMIC_RESULT_SIZE;
		}
	} else if ((query->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
		    query->command_id == KB2_GPU_DRM_MODE_COMMAND_MAP_DUMB) ||
		   (query->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
		    query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_MAP) ||
		   (query->command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
		    query->command_id == KB2_GPU_DRM_AMDGPU_COMMAND_GEM_MMAP)) {
		struct kobox_linux_drm_service_mapping mapping;
		const bool mode_map =
			query->command_set_id == KB2_GPU_DRM_MODE_SET_ID;
		const bool amdgpu_map =
			query->command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID;

		if (!api->virtgpu_map || !query_result)
			result = -EINVAL;
		else if (!service)
			result = -ENODEV;
		else if (!file_cookie)
			result = -ENOENT;
		else if (!query->mapping_page_capacity)
			result = -ENOSPC;
		else if (query->mapping_page_capacity >
			 aux_size / sizeof(uint64_t))
			result = -ENOMEM;
		else {
			int map_stage = 0;

			result = amdgpu_map ?
				(!api->amdgpu_mmap_validate ? -EINVAL :
				 api->amdgpu_mmap_validate(file,
					 query->mapping_handle)) : 0;
			if (!result) {
				map_stage = 1;
				result = api->virtgpu_map(service, file_cookie,
					query->mapping_handle, query->mapping_rights,
					(uint64_t *)aux, query->mapping_page_capacity,
					&mapping);
			}
#ifdef __KERNEL__
			if (amdgpu_map && result)
				pr_warn("kobox-drm: amdgpu map failure stage=%d handle=%u status=%d pages=%zu\n",
					map_stage, query->mapping_handle, result,
					query->mapping_page_capacity);
#endif
			(void)map_stage;
		}
		if (!result) {
			*query_result = (struct kobox_drm_query_result) {
				.mapping_id = mapping.mapping_id,
				.mapping_length = mapping.length,
				.mapping_page_count = mapping.page_count,
				.mapping_rights = query->mapping_rights,
				.mapping_cache_policy = mapping.cache_policy,
				.mapping_backing_kind = mapping.backing_kind,
				.attachment_class = KB2_GPU_ATTACHMENT_MEMORY,
			};
			const kb2_gpu_virtgpu_map_completion_t mapped = {
				.session_id = query->session_id,
				.status = KB2_GPU_STATUS_OK,
				.mapping_id = mapping.mapping_id,
				.length = mapping.length,
				.cache_policy = mapping.cache_policy,
				.exchange_id = mapping.mapping_id,
				.generation = query->generation,
				.rights = query->mapping_rights,
			};
			const kb2_protocol_status_t encoded = amdgpu_map ?
				kb2_gpu_amdgpu_map_completion_encode(completion,
					completion_capacity, completion_size, &mapped) :
				mode_map ?
				kb2_gpu_mode_map_completion_encode(completion,
					completion_capacity, completion_size, &mapped) :
				kb2_gpu_virtgpu_map_completion_encode(completion,
					completion_capacity, completion_size, &mapped);
			return encoded ==
				KB2_PROTOCOL_OK ? 0 : -EPROTO;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_AMDGPU_COMMAND_INFO) {
		uint32_t query_id = read_u32(query->inline_data +
			KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_QUERY_OFFSET);
		uint32_t record_id;
		uint32_t selectors[4];
		uint32_t count;
		for (size_t i = 0; i < 4; ++i)
			selectors[i] = read_u32(query->inline_data +
				KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_SELECTOR_OFFSET + i * 4);
		struct kb2_amdgpu_info_shape shape;
		if (!kb2_amdgpu_info_shape(query_id, selectors, &shape))
			return -EPROTO;
		count = shape.count;
		record_id = shape.record;

		if (!api->amdgpu_info || !record_id ||
		    aux_size != query->capacity[0])
			return -EINVAL;
		result = api->amdgpu_info(file, query_id, selectors, aux, aux_size);
		if (!result) {
			write_u32(record +
				KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_RESPONSE_RECORD_ID_OFFSET,
				record_id);
			write_u32(record +
				KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_RESPONSE_COUNT_OFFSET, count);
			write_u32(record +
				KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_RESPONSE_BYTES_OFFSET,
				aux_size);
			write_u32(record +
				KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_REQUIRED_COUNT_OFFSET, count);
			reply.record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT;
			reply.length = KB2_GPU_DRM_AMDGPU_RECORD_INFO_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_AMDGPU_COMMAND_GEM_OP) {
		uint32_t record_id = 0;
		size_t record_size = 0;
		bool get_info = read_u32(query->inline_data + 4) == 0;

		if (!api->amdgpu_simple)
			return -EINVAL;
		result = api->amdgpu_simple(file, query->command_id,
			query->inline_data, query->inline_size, 0,
			record, sizeof(record), &record_id, &record_size);
		if (!result) {
			if (get_info) {
				if (record_id != KB2_GPU_DRM_AMDGPU_RECORD_GEM_CREATE_INFO ||
				    record_size != 32 || aux_size != 32)
					return -EPROTO;
				memcpy(aux, record, 32);
			} else if (record_id || record_size) {
				return -EPROTO;
			}
			memset(record, 0, sizeof(record));
			write_u32(record, record_id);
			write_u32(record + 4, get_info);
			write_u32(record + 8, get_info);
			reply.record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_GEM_OP_RESULT;
			reply.length = KB2_GPU_DRM_AMDGPU_RECORD_GEM_OP_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_AMDGPU_COMMAND_CS) {
		uint64_t sequence = 0;

		if (!api->amdgpu_cs || !aux ||
		    aux_size != query->aux_size)
			return -EINVAL;
		result = api->amdgpu_cs(file, read_u32(query->inline_data),
			read_u32(query->inline_data + 4), query->cs_parts,
			query->cs_count, aux, aux_size, &sequence);
		if (!result) {
			if (!sequence)
				return -EPROTO;
			write_u64(record, sequence);
			reply.record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_CS_RESULT;
			reply.length = KB2_GPU_DRM_AMDGPU_RECORD_CS_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID) {
		uint32_t record_id = 0;
		size_t record_size = 0;

		if (!api->amdgpu_simple)
			return -EINVAL;
		result = api->amdgpu_simple(file, query->command_id,
			query->inline_data, query->inline_size,
			query->command_id == KB2_GPU_DRM_AMDGPU_COMMAND_WAIT_CS ?
				(uint64_t)query->timeout_nsec : query->fence_deadline_ns,
			record, sizeof(record), &record_id, &record_size);
		if (!result) {
			reply.record_schema_id = record_id;
			reply.length = record_size;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_GETPARAM) {
		if (!api->virtgpu_getparam)
			return -EINVAL;
		result = api->virtgpu_getparam(file, read_u64(query->inline_data), &value);
		if (!result) {
			write_u64(record, value);
			reply.record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_SCALAR_U64;
			reply.length = KB2_GPU_DRM_VIRTGPU_RECORD_SCALAR_U64_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_GET_CAPS) {
		if (!api->virtgpu_get_caps)
			return -EINVAL;
		result = api->virtgpu_get_caps(file,
			read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_REQUEST_CAPSET_ID_OFFSET),
			read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_REQUEST_CAPSET_VERSION_OFFSET),
			aux, aux_size);
		if (!result) {
			/* The public Linux ioctl does not expose the host capset length.
			 * Report the deterministic, zero-initialized transport extent. */
			write_u32(record, aux_size);
			write_u32(record + 4, aux_size);
			reply.record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_RESULT;
			reply.length = KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_CONTEXT_INIT) {
		if (!api->virtgpu_context_init)
			return -EINVAL;
		result = api->virtgpu_context_init(file,
			read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_PARAMETER_MASK_OFFSET),
			read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_CAPSET_ID_OFFSET),
			read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_RING_COUNT_OFFSET),
			read_u64(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_POLL_RING_MASK_OFFSET),
			aux, aux_size);
	} else if (query->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER) {
		if (!api->virtgpu_execbuffer || query->command_size > aux_size ||
		    query->handles_offset > aux_size ||
		    query->handle_count >
			(aux_size - query->handles_offset) / sizeof(uint32_t) ||
		    (query->input_syncobj_count &&
		     (query->input_syncobjs_offset > aux_size ||
		      query->input_syncobj_count >
			(aux_size - query->input_syncobjs_offset) /
			KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE)) ||
		    (query->output_syncobj_count &&
		     (query->output_syncobjs_offset > aux_size ||
		      query->output_syncobj_count >
			(aux_size - query->output_syncobjs_offset) /
			KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE)))
			return -EINVAL;
		result = api->virtgpu_execbuffer(service, file_cookie,
			query->session_id, query->fence_correlation, query->exec_flags,
			query->ring_index, aux, query->command_size,
			query->handle_count ? aux + query->handles_offset : NULL,
			query->handle_count,
			query->input_syncobj_count ?
				aux + query->input_syncobjs_offset : NULL,
			query->input_syncobj_count,
			query->output_syncobj_count ?
				aux + query->output_syncobjs_offset : NULL,
			query->output_syncobj_count);
		if (!result) {
			reply.record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT;
			reply.length = KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_CREATE) {
		struct kobox_linux_virtgpu_resource_create resource;

		if (!api->virtgpu_resource_create)
			return -EINVAL;
		resource = (struct kobox_linux_virtgpu_resource_create) {
			.target = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_TARGET_OFFSET),
			.format = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_FORMAT_OFFSET),
			.bind = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_BIND_OFFSET),
			.width = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_WIDTH_OFFSET),
			.height = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_HEIGHT_OFFSET),
			.depth = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_DEPTH_OFFSET),
			.array_size = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_ARRAY_SIZE_OFFSET),
			.last_level = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_LAST_LEVEL_OFFSET),
			.sample_count = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_SAMPLE_COUNT_OFFSET),
			.flags = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_FLAGS_OFFSET),
			.backing_handle = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_BACKING_HANDLE_OFFSET),
			.size = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_REQUESTED_SIZE_OFFSET),
			.stride = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_REQUESTED_STRIDE_OFFSET),
		};
		result = api->virtgpu_resource_create(file, &resource);
		if (!result) {
			write_u32(record, resource.bo_handle);
			write_u32(record + 4, resource.resource_handle);
			write_u32(record + 8, resource.size);
			write_u32(record + 12, resource.stride);
			reply.record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_RESULT;
			reply.length = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_INFO) {
		struct kobox_linux_virtgpu_resource_info resource = {
			.bo_handle = read_u32(query->inline_data),
		};

		if (!api->virtgpu_resource_info)
			return -EINVAL;
		result = api->virtgpu_resource_info(file, &resource);
		if (!result) {
			write_u32(record, resource.resource_handle);
			write_u32(record + 4, resource.size);
			write_u32(record + 8, resource.blob_memory);
			reply.record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_INFO_RESULT;
			reply.length = KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_INFO_RESULT_SIZE;
		}
	} else if (query->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
		   (query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_TRANSFER_FROM_HOST_3D ||
		    query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_TRANSFER_TO_HOST_3D)) {
		struct kobox_linux_virtgpu_transfer transfer;

		if (!api->virtgpu_transfer)
			return -EINVAL;
		transfer = (struct kobox_linux_virtgpu_transfer) {
			.bo_handle = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_BO_HANDLE_OFFSET),
			.x = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_X_OFFSET),
			.y = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_Y_OFFSET),
			.z = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_Z_OFFSET),
			.width = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_WIDTH_OFFSET),
			.height = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_HEIGHT_OFFSET),
			.depth = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_DEPTH_OFFSET),
			.level = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_LEVEL_OFFSET),
			.offset = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_OFFSET_OFFSET),
			.stride = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_STRIDE_OFFSET),
			.layer_stride = read_u32(query->inline_data +
				KB2_GPU_DRM_VIRTGPU_RECORD_TRANSFER_REQUEST_LAYER_STRIDE_OFFSET),
		};
		result = api->virtgpu_transfer(file,
			query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_TRANSFER_FROM_HOST_3D,
			&transfer);
	} else if (query->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
		   query->command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_WAIT) {
		if (!api->virtgpu_wait)
			return -EINVAL;
		result = api->virtgpu_wait(file, read_u32(query->inline_data),
			read_u32(query->inline_data + 4));
	} else {
		result = -EOPNOTSUPP;
	}
	reply.status = command_status(result);
	reply.data = record;
	return kb2_gpu_inline_completion_encode(completion, completion_capacity,
		completion_size, &reply) == KB2_PROTOCOL_OK ? 0 : -EPROTO;
}

int kobox_drm_query_execute(const struct kobox_drm_query *query,
			   const struct kobox_drm_query_api *api,
			   struct kobox_linux_drm_file *file,
			   unsigned char *output, size_t output_size,
			   unsigned char *aux, size_t aux_size,
			   unsigned char *completion, size_t completion_capacity,
			   size_t *completion_size)
{
	return kobox_drm_query_execute_service(query, api, NULL, 0, file,
		output, output_size, aux, aux_size, completion,
		completion_capacity, completion_size, NULL);
}
