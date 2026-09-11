/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_DEVICE_CODEC_H
#define KOBOX_DEVICE_CODEC_H

#include <kobox2/device_port_layout.h>
#include <stddef.h>
#include <stdint.h>

/* Decoded local values, not native structure bytes or transferred handles. */
struct kobox_device_packet {
	uint64_t generation, object, sequence;
	uint32_t operation, status, count;
	uint64_t values[KB2_DEVICE_PORT_MAX_VALUES];
};

enum kobox_device_codec_result {
	KOBOX_DEVICE_CODEC_OK,
	KOBOX_DEVICE_CODEC_INVALID,
	KOBOX_DEVICE_CODEC_MALFORMED,
};

/* Storage must not overlap packet. Decode failure clears the output packet.
 * Operation-specific rights, generation and argument checks belong to the
 * resource owner, not the transport. Neither routine acquires capabilities.
 */
enum kobox_device_codec_result kobox_device_encode(
	const struct kobox_device_packet *packet, void *storage, size_t size);
enum kobox_device_codec_result kobox_device_decode(
	const void *storage, size_t size, struct kobox_device_packet *packet);

#endif
