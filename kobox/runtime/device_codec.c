// SPDX-License-Identifier: GPL-2.0-only
#include "device_codec.h"

#include <string.h>

static const unsigned char identity[] = KB2_DEVICE_PORT_ABI_IDENTITY_BYTES;
static const unsigned char digest[] = KB2_DEVICE_PORT_SCHEMA_SHA256_BYTES;

static void put(unsigned char *bytes, uint64_t value, unsigned int width)
{
	unsigned int i;

	for (i = 0; i < width; i++)
		bytes[i] = value >> (8 * i);
}

static uint64_t get(const unsigned char *bytes, unsigned int width)
{
	uint64_t value = 0;
	unsigned int i;

	for (i = 0; i < width; i++)
		value |= (uint64_t)bytes[i] << (8 * i);
	return value;
}

enum kobox_device_codec_result kobox_device_encode(
	const struct kobox_device_packet *packet, void *storage, size_t size)
{
	unsigned char *bytes = storage;
	unsigned int i;

	if (!packet || !bytes || size != KB2_DEVICE_PORT_PACKET_SIZE ||
	    packet->count > KB2_DEVICE_PORT_MAX_VALUES)
		return KOBOX_DEVICE_CODEC_INVALID;
	memset(bytes, 0, size);
	memcpy(bytes + KB2_DEVICE_PORT_PACKET_ABI_IDENTITY_OFFSET,
	       identity, sizeof(identity));
	memcpy(bytes + KB2_DEVICE_PORT_PACKET_SCHEMA_DIGEST_OFFSET,
	       digest, sizeof(digest));
	put(bytes + KB2_DEVICE_PORT_PACKET_STATUS_OFFSET, packet->status, 4);
	put(bytes + KB2_DEVICE_PORT_PACKET_OPCODE_OFFSET, packet->operation, 4);
	put(bytes + KB2_DEVICE_PORT_PACKET_COUNT_OFFSET, packet->count, 4);
	put(bytes + KB2_DEVICE_PORT_PACKET_GENERATION_OFFSET,
	    packet->generation, 8);
	put(bytes + KB2_DEVICE_PORT_PACKET_OBJECT_ID_OFFSET, packet->object, 8);
	put(bytes + KB2_DEVICE_PORT_PACKET_SEQUENCE_OFFSET, packet->sequence, 8);
	for (i = 0; i < packet->count; i++)
		put(bytes + KB2_DEVICE_PORT_PACKET_VALUES_OFFSET + i * 8,
		    packet->values[i], 8);
	return KOBOX_DEVICE_CODEC_OK;
}

enum kobox_device_codec_result kobox_device_decode(
	const void *storage, size_t size, struct kobox_device_packet *packet)
{
	const unsigned char *bytes = storage;
	struct kobox_device_packet decoded = {0};
	unsigned int i;

	if (!packet)
		return KOBOX_DEVICE_CODEC_INVALID;
	memset(packet, 0, sizeof(*packet));
	if (!bytes)
		return KOBOX_DEVICE_CODEC_INVALID;
	if (size != KB2_DEVICE_PORT_PACKET_SIZE ||
	    memcmp(bytes + KB2_DEVICE_PORT_PACKET_ABI_IDENTITY_OFFSET,
		   identity, sizeof(identity)) ||
	    memcmp(bytes + KB2_DEVICE_PORT_PACKET_SCHEMA_DIGEST_OFFSET,
		   digest, sizeof(digest)))
		return KOBOX_DEVICE_CODEC_MALFORMED;
	decoded.status = get(bytes + KB2_DEVICE_PORT_PACKET_STATUS_OFFSET, 4);
	decoded.operation = get(bytes + KB2_DEVICE_PORT_PACKET_OPCODE_OFFSET, 4);
	decoded.count = get(bytes + KB2_DEVICE_PORT_PACKET_COUNT_OFFSET, 4);
	decoded.generation = get(bytes + KB2_DEVICE_PORT_PACKET_GENERATION_OFFSET, 8);
	decoded.object = get(bytes + KB2_DEVICE_PORT_PACKET_OBJECT_ID_OFFSET, 8);
	decoded.sequence = get(bytes + KB2_DEVICE_PORT_PACKET_SEQUENCE_OFFSET, 8);
	if (decoded.count > KB2_DEVICE_PORT_MAX_VALUES)
		return KOBOX_DEVICE_CODEC_MALFORMED;
	for (i = 0; i < KB2_DEVICE_PORT_MAX_VALUES; i++) {
		decoded.values[i] = get(bytes + KB2_DEVICE_PORT_PACKET_VALUES_OFFSET +
				       i * 8, 8);
		if (i >= decoded.count && decoded.values[i])
			return KOBOX_DEVICE_CODEC_MALFORMED;
	}
	*packet = decoded;
	return KOBOX_DEVICE_CODEC_OK;
}
