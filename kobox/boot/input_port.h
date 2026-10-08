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
	/* Capture time in the host operations' monotonic clock domain. */
	uint64_t monotonic_ns;
	uint32_t device_id;
	uint32_t kind;
	uint16_t type, code;
	int32_t value;
};

/* Process-local host callback, never a wire pointer. notify must not sleep or
 * reenter the port: it runs under the ring lock, also from input IRQ context.
 * The host context stays alive until close has quiesced the input handler. */
struct kobox_linux_input_notify {
	void *context;
	int (*notify)(void *context);
};

#ifdef __KERNEL__
struct kobox_linux_input_port;

int kobox_linux_input_port_open(struct kobox_linux_input_port **out);
void kobox_linux_input_port_close(struct kobox_linux_input_port *port);
int kobox_linux_input_port_bind_notify(struct kobox_linux_input_port *port,
			 const struct kobox_linux_input_notify *notify);
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
