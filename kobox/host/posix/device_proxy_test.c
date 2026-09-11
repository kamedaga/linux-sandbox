// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "device_proxy.h"
#include "device_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { \
	fprintf(stderr, "proxy check failed: %s:%d: %s\n", \
		__FILE__, __LINE__, #value); abort(); } } while (0)

static unsigned int allocation, fail_at, live;
void *__real_calloc(size_t count, size_t size);
void __real_free(void *pointer);

void *__wrap_calloc(size_t count, size_t size)
{
	void *pointer;

	if (++allocation == fail_at)
		return NULL;
	pointer = __real_calloc(count, size);
	if (pointer)
		live++;
	return pointer;
}

void __wrap_free(void *pointer)
{
	if (pointer) {
		CHECK(live);
		live--;
	}
	__real_free(pointer);
}

struct peer {
	int channel;
	atomic_uint notified;
};

static int notify(void *context, unsigned int cpu)
{
	struct peer *peer = context;

	CHECK(cpu == 1);
	atomic_fetch_add(&peer->notified, 1);
	return 0;
}

static void *reply(void *context)
{
	struct peer *peer = context;
	unsigned int i;

	for (i = 0; i < 2; i++) {
		struct kobox_device_packet request;
		int descriptor = -1;

		CHECK(!kobox_device_receive(peer->channel, &request, &descriptor));
		CHECK(request.generation == 2 && request.object == 3);
		if (!i) {
			CHECK(request.operation == KB2_DEVICE_PORT_OP_OPEN && descriptor >= 0);
			CHECK(!close(descriptor));
			request.count = 9;
			request.values[0] = 0;
			request.values[1] = 0;
			request.values[2] = 8;
			request.values[3] = 1;
			request.values[4] = 0x100000;
			request.values[5] = 0x1000;
			request.values[6] = 0x1000;
			request.values[7] = 0xfffff;
			request.values[8] = 1;
		} else {
			CHECK(request.operation == KB2_DEVICE_PORT_OP_CLOSE && descriptor == -1);
			request.count = 0;
		}
		CHECK(!kobox_device_send(peer->channel, &request, -1, 0));
	}
	return NULL;
}

int main(void)
{
	struct kobox_posix_device_proxy *proxy = NULL;
	struct kobox_posix_proxy_device *device = NULL;
	int requests[2], events[2], backing;
	struct peer peer = {0};
	struct kobox_device_packet event = {
		.generation = 1, .object = 3,
		.operation = KB2_DEVICE_PORT_OP_IRQ_EVENT,
		.count = 1, .values = {1},
	};
	size_t retired, delivered;
	pthread_t server;
	unsigned int i;

	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, requests));
	CHECK(!socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, events));
	backing = open("/dev/zero", O_RDONLY | O_CLOEXEC);
	CHECK(backing >= 0);
	for (i = 1; i <= 2; i++) {
		allocation = 0;
		fail_at = i;
		CHECK(kobox_posix_device_proxy_create(requests[0], events[0], 2, 3, 2, &proxy) == ENOMEM);
		CHECK(!proxy && !live);
	}
	fail_at = 0;
	CHECK(kobox_posix_device_proxy_create(requests[0], events[0], 2, SIZE_MAX, 2, &proxy) == EINVAL);
	CHECK(!proxy);
	CHECK(!kobox_posix_device_proxy_create(requests[0], events[0], 2, 3, 2, &proxy));
	CHECK(kobox_posix_device_proxy_open(proxy, 4, backing, 4096, 0, &device) == EINVAL);
	peer.channel = requests[1];
	CHECK(!pthread_create(&server, NULL, reply, &peer));
	CHECK(!kobox_posix_device_proxy_open(proxy, 3, backing, 4096, 0, &device));
	CHECK(kobox_posix_device_proxy_pci(device));
	CHECK(kobox_posix_device_proxy_dma(device));
	CHECK(kobox_posix_device_proxy_irq(device));
	CHECK(!kobox_posix_device_proxy_start(proxy, notify, &peer));
	CHECK(kobox_posix_device_proxy_stop(proxy) == EBUSY);
	CHECK(kobox_posix_device_proxy_statistics(proxy, &retired, &delivered) == EINVAL);
	CHECK(!kobox_device_send(events[1], &event, -1, 0));
	event.generation = 2;
	CHECK(!kobox_device_send(events[1], &event, -1, 0));
	for (i = 0; i < 1000 && !atomic_load(&peer.notified); i++)
		usleep(1000);
	CHECK(atomic_load(&peer.notified) == 1);
	CHECK(!kobox_posix_device_proxy_close(device));
	CHECK(!pthread_join(server, NULL));
	CHECK(!kobox_posix_device_proxy_stop(proxy));
	CHECK(!kobox_posix_device_proxy_statistics(proxy, &retired, &delivered));
	CHECK(retired == 1 && delivered == 1);
	CHECK(!kobox_posix_device_proxy_destroy(&proxy));
	CHECK(!proxy && !live);
	CHECK(fcntl(requests[0], F_GETFD) >= 0 && fcntl(events[0], F_GETFD) >= 0);
	CHECK(!close(backing));
	for (i = 0; i < 2; i++) {
		CHECK(!close(requests[i]));
		CHECK(!close(events[i]));
	}
	puts("Native device proxy: independent channels, authority, rollback and joined notification passed");
	return 0;
}
