/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_INPUT_PORT_H
#define KOBOX_BOOT_INPUT_PORT_H

#include "../task/host.h"

/* Process-local event stream, not an evdev replacement. Linux HID and input
 * drivers own enumeration and decoding; the host only transports their
 * already-decoded events to the system input service. Sequence gaps require
 * a fresh snapshot before consumers can resume a device's state. */
enum kobox_linux_input_record_kind {
	KOBOX_INPUT_DEVICE_ADD = 1,
	KOBOX_INPUT_DEVICE_REMOVE = 2,
	KOBOX_INPUT_EVENT = 3,
};

struct kobox_linux_input_device_info {
	uint32_t id;
	uint16_t bus, vendor, product, version;
	char name[128], phys[128], uniq[128];
	uint64_t property_bits, event_bits;
	uint64_t key_bits[12], key_state[12];
	uint64_t rel_bits, abs_bits, msc_bits;
	uint64_t led_bits, snd_bits, sw_bits;
	uint64_t led_state, snd_state, sw_state;
	int32_t abs_value[64];
};

struct kobox_linux_input_record {
	uint64_t sequence;
	uint64_t monotonic_ns;
	uint32_t device_id;
	uint32_t kind;
	uint16_t type, code;
	int32_t value;
};

#ifdef __KERNEL__
struct kobox_linux_input_port;

int kobox_linux_input_port_open(struct kobox_linux_input_port **out);
void kobox_linux_input_port_close(struct kobox_linux_input_port *port);
int kobox_linux_input_port_read(struct kobox_linux_input_port *port,
			      struct kobox_linux_input_record *records,
			      size_t capacity, size_t *count,
			      uint64_t *overwritten);
int kobox_linux_input_port_snapshot(struct kobox_linux_input_port *port,
			  struct kobox_linux_input_device_info *devices,
			  size_t capacity, size_t *count,
			  uint64_t *next_sequence);
#endif

#endif
