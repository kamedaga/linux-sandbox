// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "device_channel.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int kobox_device_send(int socket, const struct kobox_device_packet *packet,
		      int descriptor, int flags)
{
	unsigned char bytes[KB2_DEVICE_PORT_PACKET_SIZE] = {0};
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control = {0};
	struct iovec iov = {.iov_base = bytes, .iov_len = sizeof(bytes)};
	struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
	ssize_t result;

	if (kobox_device_encode(packet, bytes, sizeof(bytes)) !=
	    KOBOX_DEVICE_CODEC_OK)
		return EINVAL;
	if (descriptor >= 0) {
		struct cmsghdr *header;

		message.msg_control = control.bytes;
		message.msg_controllen = sizeof(control);
		header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
	}
	do {
		result = sendmsg(socket, &message, MSG_NOSIGNAL | flags);
	} while (result < 0 && errno == EINTR);
	return result < 0 ? errno : (size_t)result == sizeof(bytes) ? 0 : EIO;
}

int kobox_device_receive(int socket, struct kobox_device_packet *packet,
			 int *descriptor)
{
	unsigned char bytes[KB2_DEVICE_PORT_PACKET_SIZE];
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(16 * sizeof(int))];
	} control = {0};
	struct iovec iov = {.iov_base = bytes, .iov_len = sizeof(bytes)};
	struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control)};
	struct cmsghdr *header;
	bool malformed = false;
	int received = -1;
	unsigned int i;
	ssize_t result;

	if (!packet || !descriptor)
		return EINVAL;
	*descriptor = -1;
	memset(packet, 0, sizeof(*packet));
	do {
		result = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		return errno;
	for (header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
		size_t count;

		if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
		    header->cmsg_len < CMSG_LEN(0)) {
			malformed = true;
			continue;
		}
		count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		if ((header->cmsg_len - CMSG_LEN(0)) % sizeof(int))
			malformed = true;
		for (i = 0; i < count; i++) {
			int fd;

			memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(fd));
			if (received >= 0) {
				close(fd);
				malformed = true;
			} else {
				received = fd;
			}
		}
	}
	if (!result) {
		if (received >= 0)
			close(received);
		return received >= 0 || malformed ? EPROTO : EPIPE;
	}
	if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
		malformed = true;
	if (!malformed &&
	    kobox_device_decode(bytes, (size_t)result, packet) !=
	    KOBOX_DEVICE_CODEC_OK)
		malformed = true;
	if (malformed) {
		if (received >= 0)
			close(received);
		memset(packet, 0, sizeof(*packet));
		return EPROTO;
	}
	*descriptor = received;
	return 0;
}

static const int errors[] = {
	[KB2_DEVICE_PORT_STATUS_OK] = 0,
	[KB2_DEVICE_PORT_STATUS_INVALID] = EINVAL,
	[KB2_DEVICE_PORT_STATUS_STALE] = ESTALE,
	[KB2_DEVICE_PORT_STATUS_UNSUPPORTED] = EOPNOTSUPP,
	[KB2_DEVICE_PORT_STATUS_IO] = EIO,
	[KB2_DEVICE_PORT_STATUS_BUSY] = EBUSY,
	[KB2_DEVICE_PORT_STATUS_NO_MEMORY] = ENOMEM,
	[KB2_DEVICE_PORT_STATUS_NO_SPACE] = ENOSPC,
	[KB2_DEVICE_PORT_STATUS_AGAIN] = EAGAIN,
	[KB2_DEVICE_PORT_STATUS_PERMISSION] = EPERM,
	[KB2_DEVICE_PORT_STATUS_RANGE] = ERANGE,
};

uint32_t kobox_device_status(int error)
{
	unsigned int i;

	for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
		if (error == errors[i])
			return i;
	return KB2_DEVICE_PORT_STATUS_IO;
}

int kobox_device_error(uint32_t status)
{
	return status < sizeof(errors) / sizeof(errors[0]) ? errors[status] : EPROTO;
}
