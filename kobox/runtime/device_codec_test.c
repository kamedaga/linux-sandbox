// SPDX-License-Identifier: GPL-2.0-only
#include "device_codec.h"
#include "device_session.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { \
	if (!(value)) { \
		fprintf(stderr, "device codec check at %u: %s\n", __LINE__, #value); \
		return 1; \
	} \
} while (0)

static int session_test(void)
{
	struct kobox_device_session sender = {.generation = 7};
	struct kobox_device_session owner = {.generation = 7};
	struct kobox_device_packet request, reply, bad;

	CHECK(kobox_device_session_issue(&sender, 1, KB2_DEVICE_PORT_OP_CLOSE,
		NULL, 0, &request) == KOBOX_DEVICE_SESSION_OK);
	CHECK(request.sequence == 1 && sender.sequence == 1);
	bad = request;
	bad.generation--;
	CHECK(kobox_device_session_accept(&owner, &bad, 2) == KOBOX_DEVICE_SESSION_STALE);
	CHECK(!owner.sequence);
	bad = request;
	bad.sequence++;
	CHECK(kobox_device_session_accept(&owner, &bad, 2) == KOBOX_DEVICE_SESSION_STALE);
	CHECK(!owner.sequence);
	CHECK(kobox_device_session_accept(&owner, &request, 2) == KOBOX_DEVICE_SESSION_OK);
	CHECK(kobox_device_request_arguments(&request) == KB2_DEVICE_PORT_STATUS_OK);
	CHECK(kobox_device_session_accept(&owner, &request, 2) == KOBOX_DEVICE_SESSION_STALE);
	reply = request;
	CHECK(kobox_device_session_reply(&request, &reply) == KOBOX_DEVICE_SESSION_OK);
	reply.generation++;
	CHECK(kobox_device_session_reply(&request, &reply) == KOBOX_DEVICE_SESSION_PROTOCOL);
	reply = request;
	reply.status = KB2_DEVICE_PORT_STATUS_STALE;
	reply.count = 1;
	CHECK(kobox_device_session_reply(&request, &reply) == KOBOX_DEVICE_SESSION_PROTOCOL);
	bad = request;
	bad.operation = KB2_DEVICE_PORT_OP_CONFIG_READ;
	bad.count = 2;
	bad.values[0] = UINT64_MAX;
	CHECK(kobox_device_request_arguments(&bad) == KB2_DEVICE_PORT_STATUS_RANGE);
	bad.operation = UINT32_MAX;
	CHECK(kobox_device_request_arguments(&bad) == KB2_DEVICE_PORT_STATUS_UNSUPPORTED);
	bad = (struct kobox_device_packet) {
		.generation = 6, .object = 1, .operation = KB2_DEVICE_PORT_OP_IRQ_EVENT,
		.count = 1, .values = {1},
	};
	CHECK(kobox_device_session_event(&owner, &bad, 2, 2) == KOBOX_DEVICE_SESSION_STALE);
	bad.generation = 7;
	CHECK(kobox_device_session_event(&owner, &bad, 2, 2) == KOBOX_DEVICE_SESSION_OK);
	bad.values[0] = 2;
	CHECK(kobox_device_session_event(&owner, &bad, 2, 2) == KOBOX_DEVICE_SESSION_PROTOCOL);
	sender.sequence = UINT64_MAX;
	CHECK(kobox_device_session_issue(&sender, 1, KB2_DEVICE_PORT_OP_CLOSE,
		NULL, 0, &request) == KOBOX_DEVICE_SESSION_OVERFLOW);
	CHECK(sender.sequence == UINT64_MAX);
	sender.failed = true;
	CHECK(kobox_device_session_issue(&sender, 1, KB2_DEVICE_PORT_OP_CLOSE,
		NULL, 0, &request) == KOBOX_DEVICE_SESSION_FAILED);
	return 0;
}

int main(void)
{
	struct kobox_device_packet input = {
		.generation = 0x0102030405060708ULL, .object = 73, .sequence = 91,
		.operation = KB2_DEVICE_PORT_OP_DMA_MAP,
	}, output, empty = {0};
	unsigned char storage[KB2_DEVICE_PORT_PACKET_SIZE + 2];
	unsigned char *bytes = storage + 1;
	size_t length;
	unsigned int count, index;

	CHECK(!session_test());
	for (count = 0; count <= KB2_DEVICE_PORT_MAX_VALUES; count++) {
		input.count = count;
		for (index = 0; index < KB2_DEVICE_PORT_MAX_VALUES; index++)
			input.values[index] = index < count ? UINT64_MAX - index : 0;
		memset(storage, 0xa5, sizeof(storage));
		CHECK(kobox_device_encode(&input, bytes,
			KB2_DEVICE_PORT_PACKET_SIZE) == KOBOX_DEVICE_CODEC_OK);
		CHECK(storage[0] == 0xa5 && storage[sizeof(storage) - 1] == 0xa5);
		CHECK(bytes[KB2_DEVICE_PORT_PACKET_GENERATION_OFFSET] == 8 &&
		      bytes[KB2_DEVICE_PORT_PACKET_GENERATION_OFFSET + 7] == 1);
		CHECK(kobox_device_decode(bytes, KB2_DEVICE_PORT_PACKET_SIZE,
			&output) == KOBOX_DEVICE_CODEC_OK);
		CHECK(output.generation == input.generation &&
		      output.object == input.object &&
		      output.sequence == input.sequence &&
		      output.operation == input.operation && output.count == count &&
		      !memcmp(output.values, input.values, sizeof(input.values)));
	}
	for (length = 0; length <= KB2_DEVICE_PORT_PACKET_SIZE + 1; length++) {
		if (length == KB2_DEVICE_PORT_PACKET_SIZE)
			continue;
		output = input;
		CHECK(kobox_device_decode(bytes, length, &output) ==
		      KOBOX_DEVICE_CODEC_MALFORMED);
		CHECK(!memcmp(&output, &empty, sizeof(output)));
		CHECK(kobox_device_encode(&input, bytes, length) ==
		      KOBOX_DEVICE_CODEC_INVALID);
	}
	CHECK(kobox_device_decode(NULL, KB2_DEVICE_PORT_PACKET_SIZE, &output) ==
	      KOBOX_DEVICE_CODEC_INVALID);
	CHECK(!memcmp(&output, &empty, sizeof(output)));
	CHECK(kobox_device_decode(bytes, KB2_DEVICE_PORT_PACKET_SIZE, NULL) ==
	      KOBOX_DEVICE_CODEC_INVALID);
	input.count++;
	CHECK(kobox_device_encode(&input, bytes, KB2_DEVICE_PORT_PACKET_SIZE) ==
	      KOBOX_DEVICE_CODEC_INVALID);
	bytes[KB2_DEVICE_PORT_PACKET_COUNT_OFFSET] = KB2_DEVICE_PORT_MAX_VALUES + 1;
	CHECK(kobox_device_decode(bytes, KB2_DEVICE_PORT_PACKET_SIZE, &output) ==
	      KOBOX_DEVICE_CODEC_MALFORMED);
	CHECK(!memcmp(&output, &empty, sizeof(output)));
	puts("Device codec: unaligned storage, every payload count and truncated size verified");
	return 0;
}
