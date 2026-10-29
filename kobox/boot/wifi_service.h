/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_WIFI_SERVICE_H
#define KOBOX_BOOT_WIFI_SERVICE_H
#include "user_process.h"
#include "wifi_control.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define KOBOX_WIFI_PROGRAM "/usr/sbin/wpa_supplicant"
#define KOBOX_WIFI_CONFIG "/etc/wpa_supplicant.conf"
#define KOBOX_WIFI_CONFIG_MAX 8192U

static inline int kobox_wifi_status_completed(const char *reply, size_t length)
{
	const char completed[] = "wpa_state=COMPLETED";
	const char prefix[] = "wpa_state=";
	size_t start = 0;
	int result = -EPROTO;

	if (!reply || memchr(reply, 0, length))
		return -EPROTO;
	/* Match one complete STATUS field, never an SSID substring or duplicate. */
	while (start < length) {
		const char *end = memchr(reply + start, '\n', length - start);
		size_t size = end ? (size_t)(end - reply - start) : length - start;

		if (size >= sizeof(prefix) - 1 &&
		    !memcmp(reply + start, prefix, sizeof(prefix) - 1)) {
			if (result != -EPROTO)
				return -EPROTO;
			result = size == sizeof(completed) - 1 &&
				!memcmp(reply + start, completed, size);
		}
		start += size + !!end;
	}
	return result;
}

static inline int kobox_wifi_qualify_request(const char *interface,
	const void *request, size_t length, char *qualified, size_t capacity, size_t *used)
{
	const char prefix[] = "IFNAME=";
	size_t name_length = 0, size;

	if (!interface || !qualified || !used)
		return -EINVAL;
	while (name_length < 16 && interface[name_length]) {
		if ((unsigned char)interface[name_length] <= ' ')
			return -EINVAL;
		name_length++;
	}
	size = sizeof(prefix) - 1 + name_length + 1;
	if (!name_length || name_length >= 16 || !request || !length ||
	    length > 4096 - size || capacity <= size + length ||
	    memchr(request, 0, length) || memchr(request, '\n', length) ||
	    memchr(request, '\r', length) ||
	    (length >= sizeof(prefix) - 1 && !memcmp(request, prefix, sizeof(prefix) - 1)))
		return -EINVAL;
	memcpy(qualified, prefix, sizeof(prefix) - 1);
	memcpy(qualified + sizeof(prefix) - 1, interface, name_length);
	qualified[size - 1] = ' ';
	memcpy(qualified + size, request, length);
	*used = size + length;
	qualified[*used] = 0;
	return 0;
}

/* The native bridge never owns EAPOL. Reject malformed/excessive VLAN
 * nesting too, rather than let an ambiguous frame bypass the supplicant. */
static inline int kobox_wifi_data_frame_allowed(const void *frame, size_t length)
{
	const uint8_t *bytes = frame;
	size_t offset = 12;
	unsigned int tags = 0, protocol;

	if (!frame || length < 14)
		return 0;
	protocol = ((unsigned int)bytes[offset] << 8) | bytes[offset + 1];
	while (protocol == 0x8100 || protocol == 0x88a8) {
		if (++tags > 2 || length < offset + 6)
			return 0;
		offset += 4;
		protocol = ((unsigned int)bytes[offset] << 8) | bytes[offset + 1];
	}
	return protocol != 0x888e;
}

#ifdef __KERNEL__
struct kobox_linux_net_port;
struct kobox_linux_wifi_service;
/* Set *out before any operation which can transfer host CLOSE ownership.
 * On error a non-NULL service still needs close; failed close retains it. */
int kobox_linux_wifi_service_open(struct kobox_linux_wifi_service **out,
	struct kobox_linux_net_port *port,
	const struct kobox_linux_user_process_config *config);
int kobox_linux_wifi_service_monitor(void *service);
/* INFO and the monitor share this bounded, nonblocking event/status pump.
 * Events remain pending until service_events explicitly takes them. */
int kobox_linux_wifi_service_poll_events(struct kobox_linux_wifi_service *service);
int kobox_linux_wifi_service_request(struct kobox_linux_wifi_service *service,
	const void *request, size_t length, void *reply, size_t capacity, size_t *received);
int kobox_linux_wifi_service_configure(struct kobox_linux_wifi_service *service,
	const void *config, size_t length);
int kobox_linux_wifi_service_events(struct kobox_linux_wifi_service *service, uint32_t *events);
int kobox_linux_wifi_service_close(struct kobox_linux_wifi_service *service);
#endif
#endif
