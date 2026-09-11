/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_DEVICE_CHANNEL_H
#define KOBOX_POSIX_DEVICE_CHANNEL_H

#include "../../runtime/device_codec.h"

int kobox_device_send(int socket, const struct kobox_device_packet *packet,
		      int descriptor, int flags);
int kobox_device_receive(int socket, struct kobox_device_packet *packet,
			 int *descriptor);
uint32_t kobox_device_status(int error);
int kobox_device_error(uint32_t status);

#endif
