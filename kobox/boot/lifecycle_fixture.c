// SPDX-License-Identifier: GPL-2.0-only

#include "lifecycle_fixture.h"
#include "../task/posix_machine.h"

#include <kobox2_test/management.h>

#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

static int publish(struct kobox_lifecycle_fixture *fixture, uint32_t opcode)
{
	uint8_t packet[KB2_TEST_MESSAGE_SIZE];
	ssize_t result;

	if (!kb2_test_message_encode(packet, sizeof(packet), opcode,
			KB2_TEST_MESSAGE_FLAG_EVENT, fixture->generation, 1, 0))
		return -EPROTO;
	do {
		result = send(fixture->socket, packet, sizeof(packet),
			      MSG_DONTWAIT | MSG_NOSIGNAL);
	} while (result < 0 && errno == EINTR);
	return result == sizeof(packet) ? 0 : result < 0 ? -errno : -EIO;
}

static int ready(void *context)
{
	struct kobox_lifecycle_fixture *fixture = context;
	uint64_t start = 1;
	int result = publish(fixture, KB2_TEST_EVENT_READY);

	if (result)
		return result;
	/* The receiver cannot inject an IRQ before the booted core is ready. */
	return write(fixture->start, &start, sizeof(start)) == sizeof(start) ?
		0 : -errno;
}

static int pending(void *context)
{
	struct kobox_lifecycle_fixture *fixture = context;

	return atomic_load_explicit(&fixture->terminal, memory_order_acquire);
}

static void *receive(void *context)
{
	struct kobox_lifecycle_fixture *fixture = context;
	uint8_t packet[KB2_TEST_MESSAGE_SIZE];
	uint64_t start, correlation, value;
	uint32_t opcode;
	struct pollfd gate = {.fd = fixture->start, .events = POLLIN};
	ssize_t length;
	int result = -EPIPE;

	do {
		result = poll(&gate, 1, -1);
	} while (result < 0 && errno == EINTR);
	if (result != 1)
		__builtin_trap();
	result = -EPIPE;
	do {
		length = read(fixture->start, &start, sizeof(start));
	} while (length < 0 && errno == EINTR);
	if (length != sizeof(start))
		__builtin_trap();
	for (;;) {
		do {
			length = recv(fixture->socket, packet, sizeof(packet), MSG_TRUNC);
		} while (length < 0 && errno == EINTR);
		if (length <= 0)
			break;
		if (length != sizeof(packet) ||
		    !kb2_test_message_decode(packet, sizeof(packet), 0,
			fixture->generation, &opcode, &correlation, &value) ||
		    opcode != KB2_TEST_REQUEST_QUIESCE || !correlation || value) {
			/* Reject stale/invalid commands without stopping this owner. */
			if (publish(fixture, KB2_TEST_EVENT_FAULT))
				break;
			continue;
		}
		result = 1;
		break;
	}
	atomic_store_explicit(&fixture->terminal, result, memory_order_release);
	if (kobox_task_posix_operations.cpu_notify(0, KOBOX_LINUX_TASK_CONTROL_EVENT))
		__builtin_trap();
	return NULL;
}

int kobox_lifecycle_fixture_start(struct kobox_lifecycle_fixture *fixture,
				 int socket, uint64_t generation)
{
	int result;

	if (!fixture || socket < 0 || !generation)
		return -EINVAL;
	*fixture = (struct kobox_lifecycle_fixture) {
		.socket = socket, .generation = generation,
		.port = {.size = sizeof(fixture->port), .context = fixture,
			 .ready = ready, .pending = pending},
	};
	atomic_init(&fixture->terminal, 0);
	if (!atomic_is_lock_free(&fixture->terminal))
		return -EOPNOTSUPP;
	fixture->start = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fixture->start < 0)
		return -errno;
	result = pthread_create(&fixture->receiver, NULL, receive, fixture);
	if (result) {
		close(fixture->start);
		return -result;
	}
	return 0;
}

void kobox_lifecycle_fixture_close(struct kobox_lifecycle_fixture *fixture)
{
	uint64_t start = 1;

	/* Also releases a receiver whose native module startup never reached
	 * ready. Called only after the booted Linux module transaction drains.
	 */
	shutdown(fixture->socket, SHUT_RDWR);
	if (write(fixture->start, &start, sizeof(start)) != sizeof(start) ||
	    pthread_join(fixture->receiver, NULL))
		__builtin_trap();
	close(fixture->start);
	close(fixture->socket);
}
