/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_WIFI_CONTROL_H
#define KOBOX_WIFI_CONTROL_H
#include "user_process.h"

#define KOBOX_WIFI_CONTROL_NAME "pacha-wifi"
#define KOBOX_WIFI_CONTROL_ARGUMENT "@abstract:" KOBOX_WIFI_CONTROL_NAME
#define KOBOX_WIFI_CONTROL_BYTES 8192U

enum kobox_wifi_event {
	KOBOX_WIFI_SCAN_COMPLETE = 1U << 0,
	KOBOX_WIFI_SCAN_FAILED = 1U << 1,
	KOBOX_WIFI_CONNECTED = 1U << 2,
	KOBOX_WIFI_DISCONNECTED = 1U << 3,
	KOBOX_WIFI_AUTH_FAILED = 1U << 4,
};
struct kobox_linux_wifi_control;
#ifdef __KERNEL__
/* Same opening-task ownership as the process API. Close the control before
 * destroying its borrowed process. No control data is emitted to logs. */
int kobox_linux_wifi_control_open(struct kobox_linux_user_process *process,
	struct kobox_linux_wifi_control **out, uint32_t timeout_ms);
int kobox_linux_wifi_control_request(struct kobox_linux_wifi_control *control,
	const void *request, size_t length, void *reply, size_t capacity,
	size_t *received, uint32_t timeout_ms);
int kobox_linux_wifi_control_events(struct kobox_linux_wifi_control *control,
	uint32_t *events);
int kobox_linux_wifi_control_close(struct kobox_linux_wifi_control *control);
#endif
#endif
