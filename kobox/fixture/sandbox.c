// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "runtime.h"

#include <kobox2/closure_manifest.h>
#include <kobox2/protocol.h>
#include <kobox2/sha256.h>
#include <kobox2_test/bootstrap.h>
#include <kobox2_test/management.h>
#include <kobox2_test/split_virtqueue.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(KOBOX_FIXTURE_RESULT == KB2_TEST_FIXTURE_RESULT,
	       "fixture result must match the test protocol");

static void close_descriptors(int *descriptors, size_t count)
{
	size_t index;

	for (index = 0; index < count; index++) {
		if (descriptors[index] >= 0) {
			close(descriptors[index]);
			descriptors[index] = -1;
		}
	}
}

static int notification_index(const kb2_test_bootstrap_t *bootstrap,
			      uint32_t notification_id)
{
	size_t index;

	for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; index++) {
		if (bootstrap->notification_ids[index] == notification_id)
			return (int)index;
	}
	return -1;
}

static int notify(int descriptor)
{
	uint64_t value = 1;
	ssize_t bytes;

	do {
		bytes = write(descriptor, &value, sizeof(value));
	} while (bytes < 0 && errno == EINTR);
	return bytes == sizeof(value) ? 0 : -1;
}

static int wait_notification(int descriptor)
{
	struct pollfd poll_descriptor = {
		.fd = descriptor,
		.events = POLLIN,
	};
	uint64_t value;
	ssize_t bytes;
	int status;

	do {
		status = poll(&poll_descriptor, 1, -1);
	} while (status < 0 && errno == EINTR);
	if (status != 1 || !(poll_descriptor.revents & POLLIN))
		return -1;
	do {
		bytes = read(descriptor, &value, sizeof(value));
	} while (bytes < 0 && errno == EINTR);
	return bytes == sizeof(value) && value ? 0 : -1;
}

static int send_event(kb2_test_vq_t *queue, int notification_descriptor,
		      uint64_t generation, uint32_t opcode, uint64_t value)
{
	kb2_test_vq_segment_t segment;
	size_t segment_count;
	uint16_t head;
	int notification_required;

	if (kb2_test_vq_take_available(queue, &head) != KB2_TEST_VQ_OK ||
	    kb2_test_vq_read_chain(queue, head, &segment, 1, &segment_count) !=
		    KB2_TEST_VQ_OK ||
	    segment_count != 1 ||
	    !(segment.flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) ||
	    segment.length < KB2_TEST_MESSAGE_SIZE ||
	    !kb2_test_message_encode(segment.data, KB2_TEST_MESSAGE_SIZE, opcode,
				     KB2_TEST_MESSAGE_FLAG_EVENT, generation, 0,
				     value) ||
	    kb2_test_vq_complete(queue, head, KB2_TEST_MESSAGE_SIZE,
				 &notification_required) != KB2_TEST_VQ_OK ||
	    (notification_required && notify(notification_descriptor)))
		return -1;
	return 0;
}

static int request_chain(kb2_test_vq_t *queue, uint16_t head,
			 kb2_test_vq_segment_t segments[2])
{
	size_t segment_count;

	return kb2_test_vq_read_chain(queue, head, segments, 2,
				      &segment_count) == KB2_TEST_VQ_OK &&
	       segment_count == 2 &&
	       !(segments[0].flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) &&
	       (segments[1].flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) &&
	       segments[0].length == KB2_TEST_MESSAGE_SIZE &&
	       segments[1].length == KB2_TEST_MESSAGE_SIZE;
}

static void wait_for_termination(void)
{
	for (;;)
		pause();
}

static int bind_resource(void *context,
			 const kb2_closure_manifest_resource_t *resource,
			 uint32_t node_id)
{
	(void)context;
	(void)node_id;
	return resource->type == KB2_CLOSURE_RESOURCE_CHANNEL &&
	       resource->minimum_count == 1 && resource->maximum_count == 1 &&
	       resource->required_rights ==
		       (KB2_CLOSURE_CHANNEL_RIGHT_SEND |
			KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE) &&
	       resource->maximum_rights == resource->required_rights
		       ? 0
		       : -1;
}

static int validate_shared(void *context, int descriptor,
			   const kb2_closure_manifest_artifact_t *artifact)
{
	(void)context;
	(void)descriptor;
	return artifact->kind == KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER ? 0 : -1;
}

static int open_generic_closure(struct kobox_fixture_runtime *runtime,
				const kb2_test_bootstrap_t *bootstrap,
				const int *descriptors)
{
	kb2_closure_manifest_t manifest;
	struct kobox_closure_loader_config config = {
		.artifact_descriptors = descriptors +
					KB2_TEST_BASE_TRANSFER_FD_COUNT,
		.artifact_count = bootstrap->artifact_count,
		.bind_resource = bind_resource,
		.validate_shared = validate_shared,
	};
	struct stat status;
	uint8_t digest[KB2_SHA256_DIGEST_SIZE];
	void *manifest_bytes;
	int seals;
	int result = -1;

	seals = fcntl(descriptors[1], F_GET_SEALS);
	if (bootstrap->manifest_size > SIZE_MAX ||
	    fstat(descriptors[1], &status) || !S_ISREG(status.st_mode) ||
	    status.st_size < 0 ||
	    seals < 0 ||
	    (seals & (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW |
		      F_SEAL_WRITE)) !=
		    (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) ||
	    (uint64_t)status.st_size != bootstrap->manifest_size)
		return -1;
	manifest_bytes = mmap(NULL, (size_t)bootstrap->manifest_size, PROT_READ,
			      MAP_PRIVATE, descriptors[1], 0);
	if (manifest_bytes == MAP_FAILED)
		return -1;
	kb2_sha256(manifest_bytes, (size_t)bootstrap->manifest_size, digest);
	if (memcmp(digest, bootstrap->manifest_digest, sizeof(digest)) ||
	    kb2_closure_manifest_decode(manifest_bytes,
					(size_t)bootstrap->manifest_size,
					&manifest) != KB2_PROTOCOL_OK ||
	    kb2_closure_manifest_artifact_count(&manifest) !=
		    bootstrap->artifact_count)
		goto out;
	config.manifest = &manifest;
	result = kobox_fixture_runtime_open(runtime, &config);

out:
	munmap(manifest_bytes, (size_t)bootstrap->manifest_size);
	return result;
}

int main(void)
{
	kb2_test_bootstrap_t bootstrap = { 0 };
	kb2_protocol_channel_t channel;
	kb2_protocol_queue_t queues[2];
	kb2_protocol_region_t regions[KB2_TEST_REGION_COUNT];
	kb2_test_vq_t event_queue;
	kb2_test_vq_t request_queue;
	struct kobox_fixture_runtime runtime;
	int descriptors[KB2_TEST_MAX_TRANSFER_FD_COUNT];
	struct stat memory_status;
	void *shared_memory = MAP_FAILED;
	size_t descriptor_count;
	size_t index;
	size_t queue_count;
	size_t region_count;
	int event_used_index;
	int request_available_index;
	int request_used_index;
	size_t notification_base;
	int runtime_open = 0;
	int result = 1;
	int stage = 1;

	memset(descriptors, -1, sizeof(descriptors));
	if (!kb2_test_receive_bootstrap(KB2_TEST_BOOTSTRAP_FD, &bootstrap,
					descriptors,
					KB2_TEST_MAX_TRANSFER_FD_COUNT,
					&descriptor_count) ||
	    descriptor_count != kb2_test_bootstrap_descriptor_count(&bootstrap) ||
	    bootstrap.shared_memory_size > SIZE_MAX ||
	    fstat(descriptors[0], &memory_status) || memory_status.st_size < 0 ||
	    (uint64_t)memory_status.st_size != bootstrap.shared_memory_size)
		goto out;
	close(KB2_TEST_BOOTSTRAP_FD);
	stage = 2;
	for (index = 0; index < descriptor_count; index++) {
		int flags = fcntl(descriptors[index], F_GETFD);

		if (flags < 0 || !(flags & FD_CLOEXEC))
			goto out;
	}
	notification_base = KB2_TEST_BASE_TRANSFER_FD_COUNT +
			    bootstrap.artifact_count;
	shared_memory = mmap(NULL, (size_t)bootstrap.shared_memory_size,
			     PROT_READ | PROT_WRITE, MAP_SHARED,
			     descriptors[0], 0);
	if (shared_memory == MAP_FAILED ||
	    kb2_protocol_channel_decode(shared_memory,
					bootstrap.channel_descriptor_size,
					&channel, queues, 2, &queue_count,
					regions, KB2_TEST_REGION_COUNT,
					&region_count) != KB2_PROTOCOL_OK ||
	    queue_count != 2 || region_count != KB2_TEST_REGION_COUNT ||
	    channel.generation != bootstrap.generation ||
	    channel.protocol_id != KB2_TEST_PROTOCOL_ID ||
	    channel.flags != KB2_PROTOCOL_CHANNEL_FLAG_MANAGEMENT ||
	    queues[0].role != KB2_PROTOCOL_QUEUE_ROLE_EVENT ||
	    queues[1].role != KB2_PROTOCOL_QUEUE_ROLE_REQUEST ||
	    kb2_test_vq_bind(&event_queue, shared_memory,
			     (size_t)bootstrap.shared_memory_size, &queues[0],
			     regions, region_count, 0) != KB2_TEST_VQ_OK ||
	    kb2_test_vq_bind(&request_queue, shared_memory,
			     (size_t)bootstrap.shared_memory_size, &queues[1],
			     regions, region_count, 0) != KB2_TEST_VQ_OK)
		goto out;
	event_used_index = notification_index(&bootstrap,
					      queues[0].used_notification_id);
	request_available_index = notification_index(
		&bootstrap, queues[1].available_notification_id);
	request_used_index = notification_index(&bootstrap,
						queues[1].used_notification_id);
	if (event_used_index < 0 || request_available_index < 0 ||
	    request_used_index < 0 ||
	    open_generic_closure(&runtime, &bootstrap, descriptors))
		goto out;
	stage = 3;
	runtime_open = 1;
	if (send_event(&event_queue,
		       descriptors[notification_base + (size_t)event_used_index],
		       bootstrap.generation, KB2_TEST_EVENT_READY,
		       bootstrap.generation))
		goto out;

	for (;;) {
		int notify_used = 0;

		if (wait_notification(descriptors[notification_base +
						 (size_t)request_available_index]))
			goto out;
		for (;;) {
			kb2_test_vq_segment_t segments[2];
			kb2_test_vq_status_t queue_status;
			uint64_t correlation;
			uint64_t value;
			uint64_t response_value;
			uint32_t opcode;
			uint16_t head;
			int notification_required;

			queue_status = kb2_test_vq_take_available(&request_queue,
							  &head);
			if (queue_status == KB2_TEST_VQ_EMPTY)
				break;
			if (queue_status != KB2_TEST_VQ_OK ||
			    !request_chain(&request_queue, head, segments) ||
			    !kb2_test_message_decode(segments[0].data,
						     segments[0].length, 0,
						     bootstrap.generation,
						     &opcode, &correlation,
						     &value)) {
				if (send_event(&event_queue,
					       descriptors[notification_base +
							   (size_t)event_used_index],
					       bootstrap.generation,
					       KB2_TEST_EVENT_FAULT,
					       KB2_TEST_PROTOCOL_FAULT_MALFORMED_DESCRIPTOR))
					goto out;
				wait_for_termination();
			}
			response_value = value;
			if (opcode == KB2_TEST_REQUEST_RUN_FIXTURE) {
				if (kobox_fixture_runtime_run(&runtime,
							&response_value) ||
				    response_value != KOBOX_FIXTURE_RESULT) {
					if (send_event(&event_queue,
						       descriptors[notification_base +
								   (size_t)event_used_index],
						       bootstrap.generation,
						       KB2_TEST_EVENT_FAULT, 2))
						goto out;
					wait_for_termination();
				}
			} else if (opcode != KB2_TEST_REQUEST_ECHO &&
				   opcode != KB2_TEST_REQUEST_QUIESCE) {
				if (send_event(&event_queue,
					       descriptors[notification_base +
							   (size_t)event_used_index],
					       bootstrap.generation,
					       KB2_TEST_EVENT_FAULT, 3))
					goto out;
				wait_for_termination();
			}
			if (!kb2_test_message_encode(
				    segments[1].data, segments[1].length, opcode,
				    KB2_TEST_MESSAGE_FLAG_RESPONSE,
				    bootstrap.generation, correlation,
				    response_value) ||
			    kb2_test_vq_complete(&request_queue, head,
						 KB2_TEST_MESSAGE_SIZE,
						 &notification_required) !=
					    KB2_TEST_VQ_OK)
				goto out;
			notify_used |= notification_required;
			if (opcode == KB2_TEST_REQUEST_QUIESCE) {
				if (notify_used &&
				    notify(descriptors[notification_base +
						       (size_t)request_used_index]))
					goto out;
				if (kobox_fixture_runtime_quiesce(&runtime) ||
				    kobox_fixture_runtime_close(&runtime))
					goto out;
				runtime_open = 0;
				if (send_event(&event_queue,
					       descriptors[notification_base +
							   (size_t)event_used_index],
					       bootstrap.generation,
					       KB2_TEST_EVENT_STOPPED, 0))
					goto out;
				result = 0;
				goto out;
			}
		}
		if (notify_used &&
		    notify(descriptors[notification_base +
				       (size_t)request_used_index]))
			goto out;
	}

out:
	if (result)
		fprintf(stderr, "fixture sandbox failed at stage %d\n", stage);
	if (runtime_open)
		kobox_fixture_runtime_close(&runtime);
	if (shared_memory != MAP_FAILED)
		munmap(shared_memory, (size_t)bootstrap.shared_memory_size);
	close_descriptors(descriptors, KB2_TEST_MAX_TRANSFER_FD_COUNT);
	close(KB2_TEST_BOOTSTRAP_FD);
	return result;
}
