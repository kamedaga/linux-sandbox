/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_DEVICE_SESSION_H
#define KOBOX_DEVICE_SESSION_H

#include "device_codec.h"
#include <stdbool.h>

/* Single ordered request stream, serialized by the transport owner. Native
 * delivery and handle ownership are outside this state. Generation comes
 * from the trusted launch owner and cannot be advanced by peer messages.
 */
struct kobox_device_session {
	uint64_t generation;
	uint64_t sequence;
	bool failed;
};

enum kobox_device_session_result {
	KOBOX_DEVICE_SESSION_OK,
	KOBOX_DEVICE_SESSION_INVALID,
	KOBOX_DEVICE_SESSION_STALE,
	KOBOX_DEVICE_SESSION_OVERFLOW,
	KOBOX_DEVICE_SESSION_PROTOCOL,
	KOBOX_DEVICE_SESSION_FAILED,
};

enum kobox_device_session_result kobox_device_session_issue(
	struct kobox_device_session *session, uint64_t object, uint32_t operation,
	const uint64_t *values, size_t count, struct kobox_device_packet *request);
enum kobox_device_session_result kobox_device_session_accept(
	struct kobox_device_session *session, const struct kobox_device_packet *request,
	uint64_t object_count);
enum kobox_device_session_result kobox_device_session_reply(
	const struct kobox_device_packet *request,
	const struct kobox_device_packet *reply);
enum kobox_device_session_result kobox_device_session_event(
	const struct kobox_device_session *session,
	const struct kobox_device_packet *event, uint64_t object_count,
	unsigned int cpu_count);
/* Returns protocol status, not a native errno value. */
uint32_t kobox_device_request_arguments(const struct kobox_device_packet *request);

#endif
