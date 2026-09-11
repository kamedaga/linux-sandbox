// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "device_channel.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(value) do { \
	if (!(value)) { \
		fprintf(stderr, "device channel check at %u: %s\n", __LINE__, #value); \
		return 1; \
	} \
} while (0)

static int descriptor_count(void)
{
	int i, count = 0;

	for (i = 0; i < 1024; i++)
		if (fcntl(i, F_GETFD) >= 0)
			count++;
	return count;
}

int main(void)
{
	struct kobox_device_packet input = {
		.generation = 0x0102030405060708ULL, .object = 2, .sequence = 39,
		.operation = KB2_DEVICE_PORT_OP_DMA_MAP, .count = 4,
		.values = {0x1020304050607080ULL, 0x1000, 0x4000, 3},
	}, output;
	unsigned char bytes[KB2_DEVICE_PORT_PACKET_SIZE + 1], valid[KB2_DEVICE_PORT_PACKET_SIZE];
	struct stat source, received;
	int sockets[2], backing, descriptor, count, i;

	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets));
	CHECK(!kobox_device_send(sockets[0], &input, -1, 0));
	CHECK(recv(sockets[1], valid, sizeof(valid), 0) == sizeof(valid));
	CHECK(valid[KB2_DEVICE_PORT_PACKET_GENERATION_OFFSET] == 8);
	CHECK(valid[KB2_DEVICE_PORT_PACKET_GENERATION_OFFSET + 7] == 1);
	CHECK(valid[KB2_DEVICE_PORT_PACKET_VALUES_OFFSET] == 0x80);
	CHECK(valid[KB2_DEVICE_PORT_PACKET_VALUES_OFFSET + 7] == 0x10);
	CHECK(send(sockets[0], valid, sizeof(valid), 0) == sizeof(valid));
	CHECK(!kobox_device_receive(sockets[1], &output, &descriptor));
	CHECK(descriptor == -1 && output.generation == input.generation &&
		output.object == input.object && output.sequence == input.sequence &&
		output.operation == input.operation && output.count == input.count &&
		!memcmp(output.values, input.values, sizeof(input.values)));
	backing = memfd_create("device-wire-test", MFD_CLOEXEC);
	CHECK(backing >= 0 && !fstat(backing, &source));
	CHECK(!kobox_device_send(sockets[0], &input, backing, 0));
	CHECK(!kobox_device_receive(sockets[1], &output, &descriptor));
	CHECK(descriptor >= 0 && descriptor != backing && !fstat(descriptor, &received));
	CHECK(source.st_dev == received.st_dev && source.st_ino == received.st_ino);
	CHECK(fcntl(descriptor, F_GETFD) & FD_CLOEXEC);
	CHECK(!close(descriptor));
	for (i = 0; i < 6; i++) {
		size_t length = sizeof(valid);

		memcpy(bytes, valid, sizeof(valid));
		bytes[sizeof(valid)] = 0;
		switch (i) {
		case 0: bytes[KB2_DEVICE_PORT_PACKET_ABI_IDENTITY_OFFSET] ^= 1; break;
		case 1: bytes[KB2_DEVICE_PORT_PACKET_SCHEMA_DIGEST_OFFSET] ^= 1; break;
		case 2: bytes[KB2_DEVICE_PORT_PACKET_COUNT_OFFSET] = 65; break;
		case 3: bytes[KB2_DEVICE_PORT_PACKET_VALUES_OFFSET + 8 * 63] = 1; break;
		case 4: length--; break;
		case 5: length++; break;
		}
		CHECK(send(sockets[0], bytes, length, 0) == (ssize_t)length);
		CHECK(kobox_device_receive(sockets[1], &output, &descriptor) == EPROTO);
		CHECK(descriptor == -1 && !output.generation && !output.count);
	}
	/* Reject surplus capabilities without leaking even one received FD. */
	{
		union {
			struct cmsghdr alignment;
			unsigned char bytes[CMSG_SPACE(2 * sizeof(int))];
		} control = {0};
		struct iovec iov = {.iov_base = valid, .iov_len = sizeof(valid)};
		struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
			.msg_control = control.bytes, .msg_controllen = sizeof(control)};
		struct cmsghdr *header = CMSG_FIRSTHDR(&message);
		int descriptors[] = {backing, backing};

		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(descriptors));
		memcpy(CMSG_DATA(header), descriptors, sizeof(descriptors));
		count = descriptor_count();
		CHECK(sendmsg(sockets[0], &message, MSG_NOSIGNAL) == sizeof(valid));
		CHECK(kobox_device_receive(sockets[1], &output, &descriptor) == EPROTO);
		CHECK(descriptor == -1 && descriptor_count() == count);
		/* Zero-byte packets may still carry SCM_RIGHTS on SEQPACKET. */
		iov.iov_len = 0;
		CHECK(sendmsg(sockets[0], &message, MSG_NOSIGNAL) == 0);
		CHECK(kobox_device_receive(sockets[1], &output, &descriptor) == EPROTO);
		CHECK(descriptor == -1 && descriptor_count() == count);
	}
	CHECK(kobox_device_error(KB2_DEVICE_PORT_STATUS_STALE) == ESTALE);
	CHECK(kobox_device_status(ESTALE) == KB2_DEVICE_PORT_STATUS_STALE);
	CHECK(kobox_device_error(UINT32_MAX) == EPROTO);
	CHECK(!close(backing) && !close(sockets[0]));
	CHECK(kobox_device_receive(sockets[1], &output, &descriptor) == EPIPE);
	CHECK(!close(sockets[1]));
	puts("Device wire: endian, bounds, reserved values, schema and capability ownership verified");
	return 0;
}
