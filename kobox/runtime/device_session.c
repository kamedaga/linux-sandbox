// SPDX-License-Identifier: GPL-2.0-only
#include "device_session.h"

#include <string.h>

enum kobox_device_session_result kobox_device_session_issue(
	struct kobox_device_session *session, uint64_t object, uint32_t operation,
	const uint64_t *values, size_t count, struct kobox_device_packet *request)
{
	if (!session || !session->generation || !object || !request ||
	    count > KB2_DEVICE_PORT_MAX_VALUES || (count && !values))
		return KOBOX_DEVICE_SESSION_INVALID;
	if (session->failed)
		return KOBOX_DEVICE_SESSION_FAILED;
	if (session->sequence == UINT64_MAX)
		return KOBOX_DEVICE_SESSION_OVERFLOW;
	*request = (struct kobox_device_packet) {
		.generation = session->generation, .object = object,
		.sequence = ++session->sequence,
		.operation = operation, .count = count,
	};
	if (count)
		memcpy(request->values, values, count * sizeof(*values));
	return KOBOX_DEVICE_SESSION_OK;
}

enum kobox_device_session_result kobox_device_session_accept(
	struct kobox_device_session *session, const struct kobox_device_packet *request,
	uint64_t object_count)
{
	if (!session || !session->generation || !request || !object_count)
		return KOBOX_DEVICE_SESSION_INVALID;
	if (session->failed)
		return KOBOX_DEVICE_SESSION_FAILED;
	if (request->generation != session->generation || !request->object ||
	    request->object > object_count || !request->sequence ||
	    session->sequence == UINT64_MAX ||
	    request->sequence != session->sequence + 1)
		return KOBOX_DEVICE_SESSION_STALE;
	/* Consume an authenticated sequence even if opcode/arguments fail. */
	session->sequence = request->sequence;
	return KOBOX_DEVICE_SESSION_OK;
}

enum kobox_device_session_result kobox_device_session_reply(
	const struct kobox_device_packet *request,
	const struct kobox_device_packet *reply)
{
	if (!request || !reply)
		return KOBOX_DEVICE_SESSION_INVALID;
	if (reply->generation != request->generation ||
	    reply->object != request->object || reply->sequence != request->sequence ||
	    reply->operation != request->operation || (reply->status && reply->count))
		return KOBOX_DEVICE_SESSION_PROTOCOL;
	return KOBOX_DEVICE_SESSION_OK;
}

enum kobox_device_session_result kobox_device_session_event(
	const struct kobox_device_session *session,
	const struct kobox_device_packet *event, uint64_t object_count,
	unsigned int cpu_count)
{
	if (!session || !session->generation || !event || !object_count || !cpu_count)
		return KOBOX_DEVICE_SESSION_INVALID;
	if (!event->object || event->object > object_count ||
	    event->operation != KB2_DEVICE_PORT_OP_IRQ_EVENT || event->status ||
	    event->sequence || event->count != 1 || event->values[0] >= cpu_count)
		return KOBOX_DEVICE_SESSION_PROTOCOL;
	if (event->generation != session->generation)
		return KOBOX_DEVICE_SESSION_STALE;
	return KOBOX_DEVICE_SESSION_OK;
}

uint32_t kobox_device_request_arguments(const struct kobox_device_packet *request)
{
	static const struct {
		unsigned int count, wide;
	} types[] = {
		[KB2_DEVICE_PORT_OP_OPEN] = {2, 3},
		[KB2_DEVICE_PORT_OP_CLOSE] = {0, 0},
		[KB2_DEVICE_PORT_OP_CONFIG_READ] = {2, 0},
		[KB2_DEVICE_PORT_OP_CONFIG_WRITE] = {3, 0},
		[KB2_DEVICE_PORT_OP_MMIO_MAP] = {4, 3},
		[KB2_DEVICE_PORT_OP_MMIO_UNMAP] = {2, 3},
		[KB2_DEVICE_PORT_OP_MMIO_READ] = {2, 1},
		[KB2_DEVICE_PORT_OP_MMIO_WRITE] = {3, 5},
		[KB2_DEVICE_PORT_OP_DMA_ENABLE] = {1, 0},
		[KB2_DEVICE_PORT_OP_DMA_MAP] = {4, 7},
		[KB2_DEVICE_PORT_OP_DMA_UNMAP] = {2, 3},
		[KB2_DEVICE_PORT_OP_IRQ_ALLOCATE] = {4, 0},
		[KB2_DEVICE_PORT_OP_IRQ_RELEASE] = {2, 3},
		[KB2_DEVICE_PORT_OP_IRQ_MASK] = {3, 3},
		[KB2_DEVICE_PORT_OP_IRQ_ACK] = {2, 3},
		[KB2_DEVICE_PORT_OP_IRQ_QUIESCE] = {2, 3},
		[KB2_DEVICE_PORT_OP_IRQ_ACTIVE] = {2, 3},
		[KB2_DEVICE_PORT_OP_IRQ_AFFINITY] = {3, 3},
		[KB2_DEVICE_PORT_OP_IRQ_RETRIGGER] = {2, 3},
		[KB2_DEVICE_PORT_OP_IRQ_NEXT] = {1, 0},
	};
	unsigned int op, i;

	if (!request || request->status)
		return KB2_DEVICE_PORT_STATUS_INVALID;
	op = request->operation;
	if (!op || op >= sizeof(types) / sizeof(types[0]))
		return KB2_DEVICE_PORT_STATUS_UNSUPPORTED;
	if (request->count != types[op].count)
		return KB2_DEVICE_PORT_STATUS_INVALID;
	for (i = 0; i < request->count; i++)
		if (!(types[op].wide & (1U << i)) && request->values[i] > UINT32_MAX)
			return KB2_DEVICE_PORT_STATUS_RANGE;
	return KB2_DEVICE_PORT_STATUS_OK;
}
