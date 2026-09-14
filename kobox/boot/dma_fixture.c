// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "boot_test.h"
#include "dma_gate.h"
#include "dma_fixture.h"
#include "iommu_resource.h"
#include "pci_config_fixture.h"
#include "resource_registry.h"
#include "../runtime/resource_runtime.h"

#include <kobox2/iommu_domain_layout.h>
#include <kobox2/sha256.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct dma_mapping {
	struct dma_mapping *next;
	uint64_t iova;
	size_t length;
	void *address;
	unsigned int protection;
};

struct kobox_dma_fixture {
	struct kobox_pci_config_fixture pci;
	struct kobox_linux_pci_host pci_host;
	struct dma_mapping *mappings;
	int ram;
	size_t ram_size;
	unsigned int enabled, fail_after, maps, unmaps, transfers, revoked;
	int socket;
	pid_t engine;
	pthread_mutex_t lock;
	const char *core_path;
	struct kobox_linux_dma_test dma;
	struct kobox_boot_iommu_resource iommu;
	struct kobox_resource_runtime *runtime;
	struct kobox_linux_resource_port port;
	unsigned int imported, released, rejected;
	uint64_t generation, object_id, device_id;
	dev_t anchor_device;
	ino_t anchor_inode;
	int borrowed_socket;
	int fail_unmap, unmap_failed;
};

struct imported_domain {
	struct kobox_dma_fixture *owner;
	int socket;
};

static int open_grant(struct kobox_dma_fixture *fixture);

/* Test-only control protocol: fixed little-endian words, no guest pointers
 * or host FD numbers. The RAM capability is transferred with SCM_RIGHTS.
 * The engine is forked before loading Linux or creating its RAM mappings.
 */
enum command {
	PREPARE = 1, ENABLE, MAP, UNMAP, TRANSFER, FAIL_MAP, STOP,
};

#define MESSAGE_WORDS 10

static int send_message(int socket, const uint64_t words[MESSAGE_WORDS], int fd)
{
	unsigned char bytes[MESSAGE_WORDS * 8];
	union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } control;
	struct iovec iov = {.iov_base = bytes, .iov_len = sizeof(bytes)};
	struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
	unsigned int i, byte;
	ssize_t sent;

	for (i = 0; i < MESSAGE_WORDS; i++)
		for (byte = 0; byte < 8; byte++)
			bytes[i * 8 + byte] = words[i] >> (8 * byte);
	if (fd >= 0) {
		struct cmsghdr *header;

		memset(&control, 0, sizeof(control));
		message.msg_control = control.bytes;
		message.msg_controllen = sizeof(control.bytes);
		header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(fd));
		memcpy(CMSG_DATA(header), &fd, sizeof(fd));
	}
	do {
		sent = sendmsg(socket, &message, MSG_NOSIGNAL);
	} while (sent < 0 && errno == EINTR);
	return sent == sizeof(bytes) ? 0 : sent < 0 ? -errno : -EIO;
}

static int receive_message(int socket, uint64_t words[MESSAGE_WORDS], int *fd)
{
	unsigned char bytes[MESSAGE_WORDS * 8];
	union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } control;
	struct iovec iov = {.iov_base = bytes, .iov_len = sizeof(bytes)};
	struct msghdr message = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
	};
	struct cmsghdr *header;
	unsigned int i, byte;
	int malformed = 0;
	ssize_t received;

	*fd = -1;
	do {
		received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
	} while (received < 0 && errno == EINTR);
	if (received < 0)
		return -errno;
	for (header = CMSG_FIRSTHDR(&message); header;
	     header = CMSG_NXTHDR(&message, header)) {
		if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
			size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			size_t index;

			for (index = 0; index < count; index++) {
				int incoming;

				memcpy(&incoming, CMSG_DATA(header) + index * sizeof(int), sizeof(int));
				if (*fd < 0)
					*fd = incoming;
				else {
					close(incoming);
					malformed = 1;
				}
			}
		} else {
			malformed = 1;
		}
	}
	if (received != sizeof(bytes) || message.msg_flags & (MSG_TRUNC | MSG_CTRUNC) || malformed) {
		if (*fd >= 0)
			close(*fd);
		*fd = -1;
		return -EPROTO;
	}
	for (i = 0; i < MESSAGE_WORDS; i++) {
		words[i] = 0;
		for (byte = 0; byte < 8; byte++)
			words[i] |= (uint64_t)bytes[i * 8 + byte] << (8 * byte);
	}
	return 0;
}

static int call_socket(struct kobox_dma_fixture *fixture, int socket,
		       uint64_t words[MESSAGE_WORDS], int fd)
{
	int result, received_fd = -1;

	if (pthread_mutex_lock(&fixture->lock))
		abort();
	result = send_message(socket, words, fd);
	if (!result)
		result = receive_message(socket, words, &received_fd);
	if (received_fd >= 0) {
		close(received_fd);
		result = -EPROTO;
	}
	if (!result)
		result = words[0] <= 4095 ? -(int)words[0] : -EPROTO;
	if (pthread_mutex_unlock(&fixture->lock))
		abort();
	return result;
}

static int call(struct kobox_dma_fixture *fixture, uint64_t words[MESSAGE_WORDS], int fd)
{
	return call_socket(fixture, fixture->socket, words, fd);
}

static int prepare(void *context, int descriptor, size_t length)
{
	struct kobox_dma_fixture *fixture = context;
	uint64_t words[MESSAGE_WORDS] = {PREPARE, length};
	int result;

	result = call(context, words, descriptor);
	if (result)
		return result;
	fixture->ram_size = length;
	result = open_grant(fixture);
	if (!result)
		result = kobox_boot_resource_port(fixture->runtime, &fixture->port);
	if (!result)
		result = kobox_boot_iommu_resource(&fixture->port, 2, 0, 101,
						  length, &fixture->iommu);
	if (!result)
		fixture->dma.host = fixture->iommu.host;
	return result;
}

static int enable(void *context, uint32_t enabled)
{
	struct imported_domain *domain = context;
	uint64_t words[MESSAGE_WORDS] = {ENABLE, enabled};

	return call_socket(domain->owner, domain->socket, words, -1);
}

static int engine_map(struct kobox_dma_fixture *fixture, uint64_t iova, uint64_t offset,
		      size_t length, unsigned int protection)
{
	struct dma_mapping *mapping;
	int prot = 0;

	if (!length || length % 4096 || iova % 4096 || offset % 4096 ||
	    iova < 0x40000000 || length > 0x41000000 - iova ||
	    offset > fixture->ram_size || length > fixture->ram_size - offset ||
	    !protection || protection & ~(KOBOX_DMA_DEVICE_READ | KOBOX_DMA_DEVICE_WRITE))
		return -EINVAL;
	for (mapping = fixture->mappings; mapping; mapping = mapping->next)
		if (iova <= mapping->iova + mapping->length - 1 &&
		    mapping->iova <= iova + length - 1)
			return -EEXIST;
	if (fixture->fail_after && !--fixture->fail_after)
		return -ENOMEM;
	mapping = calloc(1, sizeof(*mapping));
	if (!mapping)
		return -ENOMEM;
	if (protection & KOBOX_DMA_DEVICE_READ)
		prot |= PROT_READ;
	if (protection & KOBOX_DMA_DEVICE_WRITE)
		prot |= PROT_WRITE;
	mapping->address = mmap(NULL, length, prot, MAP_SHARED, fixture->ram, offset);
	if (mapping->address == MAP_FAILED) {
		int error = -errno;

		free(mapping);
		return error;
	}
	mapping->iova = iova;
	mapping->length = length;
	mapping->protection = protection;
	mapping->next = fixture->mappings;
	fixture->mappings = mapping;
	fixture->maps++;
	return 0;
}

static int denied_access(void *address, int write)
{
	pid_t owner = getpid(), child = fork(), waited;
	int status;

	if (child < 0)
		return -errno;
	if (!child) {
		const struct rlimit limit = {0, 0};

		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != owner)
			_exit(1);
		setrlimit(RLIMIT_CORE, &limit);
		if (write)
			*(volatile unsigned char *)address = 0xa5;
		else
			(void)*(volatile unsigned char *)address;
		_exit(1);
	}
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	return waited == child && WIFSIGNALED(status) &&
		WTERMSIG(status) == SIGSEGV ? 0 : -EIO;
}

static int engine_unmap(struct kobox_dma_fixture *fixture, uint64_t iova, size_t length)
{
	struct dma_mapping **entry = &fixture->mappings, *mapping;

	if (!length || length % 4096)
		return -EINVAL;
	for (; (mapping = *entry); entry = &mapping->next) {
		if (mapping->iova != iova || mapping->length != length)
			continue;
		if (fixture->fail_unmap) {
			fixture->unmap_failed = 1;
			fprintf(stderr, "DMA invalidation denied: maps=%u unmaps=%u retained=1\n",
				fixture->maps, fixture->unmaps);
			return -EIO;
		}
		if (munmap(mapping->address, length))
			return -errno;
		if (denied_access(mapping->address, 0) || denied_access(mapping->address, 1))
			abort();
		fixture->revoked++;
		*entry = mapping->next;
		free(mapping);
		fixture->unmaps++;
		return 0;
	}
	return -ENOENT;
}

static int engine_transfer(struct kobox_dma_fixture *fixture, uint64_t iova, void *bytes,
			   size_t length, unsigned int write)
{
	struct dma_mapping *mapping;
	unsigned int permission = write ? KOBOX_DMA_DEVICE_WRITE : KOBOX_DMA_DEVICE_READ;

	if (!fixture->enabled)
		return -EACCES;
	for (mapping = fixture->mappings; mapping; mapping = mapping->next) {
		if (iova < mapping->iova || iova - mapping->iova >= mapping->length ||
		    length > mapping->length - (iova - mapping->iova))
			continue;
		if (!(mapping->protection & permission))
			return -EACCES;
		if (write)
			memcpy((char *)mapping->address + (iova - mapping->iova), bytes, length);
		else
			memcpy(bytes, (char *)mapping->address + (iova - mapping->iova), length);
		fixture->transfers++;
		return 0;
	}
	return -EFAULT;
}

static void fail_map(void *context, unsigned int after)
{
	uint64_t words[MESSAGE_WORDS] = {FAIL_MAP, after};

	if (call(context, words, -1))
		abort();
}

static int map(void *context, uint64_t iova, uint64_t offset,
	       uint64_t length, uint32_t protection)
{
	struct imported_domain *domain = context;
	uint64_t words[MESSAGE_WORDS] = {MAP, iova, offset, length, protection};

	return call_socket(domain->owner, domain->socket, words, -1);
}

static int unmap(void *context, uint64_t iova, uint64_t length)
{
	struct imported_domain *domain = context;
	uint64_t words[MESSAGE_WORDS] = {UNMAP, iova, length};

	return call_socket(domain->owner, domain->socket, words, -1);
}

static int identity(void *object, struct kobox_iommu_identity *out)
{
	struct imported_domain *domain = object;

	*out = (struct kobox_iommu_identity) {
		.generation = domain->owner->generation, .object_id = domain->owner->object_id,
		.device_object_id = domain->owner->device_id,
		.ram_length = domain->owner->ram_size, .aperture_start = 0x40000000,
		.aperture_end = 0x40ffffff, .page_size = 4096, .coherent = 1,
	};
	return 0;
}

static int import_domain(void *context, const kb2_resource_grant_slot_t *slot,
			 const kb2_resource_grant_object_t *object,
			 const struct kobox_resource_native_handle *handles,
			 size_t count, void **out,
			 const struct kobox_resource_interface_operations **ops)
{
	static const struct kobox_iommu_resource_operations operations = {
		.base = {.size = sizeof(operations), .identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER},
		.identity = identity, .set_enabled = enable, .map = map, .unmap = unmap,
	};
	static const uint8_t digest[32] = KB2_IOMMU_DOMAIN_SCHEMA_SHA256_BYTES;
	struct kobox_dma_fixture *fixture = context;
	struct imported_domain *domain;
	struct stat actual;

	*out = NULL;
	*ops = NULL;
	if (slot->slot_id != 2 || slot->resource_type != KB2_CLOSURE_RESOURCE_DEVICE ||
	    memcmp(slot->interface_schema_digest, digest, sizeof(digest)) ||
	    object->object_id != fixture->object_id ||
	    object->granted_rights != KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS ||
	    count != 1 || handles[0].role != KB2_IOMMU_DOMAIN_NATIVE_HANDLE_ROLE_DOMAIN ||
	    fstat(handles[0].handle, &actual) || !S_ISSOCK(actual.st_mode) ||
	    fixture->anchor_device != actual.st_dev || fixture->anchor_inode != actual.st_ino ||
	    (fixture->borrowed_socket && fixture->socket >= 0))
		return -EACCES;
	domain = calloc(1, sizeof(*domain));
	if (!domain)
		return -ENOMEM;
	domain->socket = fcntl(handles[0].handle, F_DUPFD_CLOEXEC, 0);
	if (domain->socket < 0) {
		int error = -errno;

		free(domain);
		return error;
	}
	domain->owner = fixture;
	if (fixture->borrowed_socket)
		fixture->socket = domain->socket;
	fixture->imported++;
	*out = domain;
	*ops = &operations.base;
	return 0;
}

static void release_domain(void *context, void *object)
{
	struct kobox_dma_fixture *fixture = context;
	struct imported_domain *domain = object;

	if (fixture->borrowed_socket)
		fixture->socket = -1;
	close(domain->socket);
	free(domain);
	fixture->released++;
}

static int open_grant(struct kobox_dma_fixture *fixture)
{
	static const uint8_t digest[32] = KB2_IOMMU_DOMAIN_SCHEMA_SHA256_BYTES;
	kb2_closure_manifest_artifact_t artifact = {
		.node_id = 1, .kind = KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
		.flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT | KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX,
		.namespace_name = {.data = "core", .length = 4},
	};
	kb2_closure_manifest_resource_t resource = {
		.slot_id = 2, .type = KB2_CLOSURE_RESOURCE_DEVICE,
		.minimum_count = 1, .maximum_count = 1,
		.required_rights = KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS,
		.maximum_rights = KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS,
		.flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED,
	};
	kb2_closure_manifest_binding_t binding = {.slot_id = 2, .node_id = 1};
	kb2_closure_manifest_source_t manifest_source = {
		.artifacts = &artifact, .artifact_count = 1,
		.resources = &resource, .resource_count = 1,
		.bindings = &binding, .binding_count = 1,
	};
	kb2_resource_grant_slot_source_t slot = {
		.slot_id = 2, .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
		.state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
	};
	kb2_resource_grant_object_source_t object = {
		.slot_id = 2, .object_id = 102, .granted_rights = KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS,
	};
	kb2_resource_grant_handle_binding_t handle = {
		.object_id = 102, .role = KB2_IOMMU_DOMAIN_NATIVE_HANDLE_ROLE_DOMAIN,
		.transfer_handle_index = 0,
	};
	kb2_resource_grant_source_t grant_source = {
		.generation = 17, .slots = &slot, .slot_count = 1,
		.objects = &object, .object_count = 1,
		.handle_bindings = &handle, .handle_binding_count = 1,
	};
	uint8_t manifest_bytes[1024], grant_bytes[1024];
	kb2_closure_manifest_t manifest;
	kb2_resource_grant_t grant;
	struct kobox_posix_resource_config config = {
		.manifest = &manifest, .grant = &grant,
		.native_handles = &fixture->socket, .native_handle_count = 1,
		.import_object = import_domain, .release_object = release_domain,
		.object_context = fixture,
	};
	size_t manifest_size, grant_size;
	struct stat stat;
	void *image;
	int core = open(fixture->core_path, O_RDONLY | O_CLOEXEC);
	int wrong_fd, profile;

	if (core < 0)
		return -errno;
	if (fstat(core, &stat) || stat.st_size <= 0 || (uint64_t)stat.st_size > SIZE_MAX) {
		close(core);
		return -EINVAL;
	}
	image = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, core, 0);
	close(core);
	if (image == MAP_FAILED)
		return -errno;
	artifact.content_size = stat.st_size;
	kb2_sha256(image, stat.st_size, artifact.content_digest);
	munmap(image, stat.st_size);
	memcpy(resource.interface_schema_digest, digest, 32);
	memcpy(slot.interface_schema_digest, digest, 32);
	if (kb2_closure_manifest_encode(manifest_bytes, sizeof(manifest_bytes), &manifest_size,
					&manifest_source) != KB2_PROTOCOL_OK ||
	    kb2_closure_manifest_decode(manifest_bytes, manifest_size, &manifest) != KB2_PROTOCOL_OK)
		return -EPROTO;
	kb2_sha256(manifest_bytes, manifest_size, grant_source.closure_manifest_digest);
	wrong_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (wrong_fd < 0)
		return -errno;
	/* Exercise the serialized grant and real registry, not only a lookup
	 * callback oracle. A stale generation may import, but cannot bind to
	 * this owner's domain; that imported FD must be released as well.
	 */
	for (profile = 0; profile < 6; profile++) {
		enum kobox_resource_runtime_status result;
		struct kobox_linux_resource_port port;
		struct kobox_boot_iommu_resource iommu;

		config.native_handles = profile == 0 ? &wrong_fd : &fixture->socket;
		object.granted_rights = profile == 1 ? 1 : KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS;
		handle.role = profile == 2 ? 1 : KB2_IOMMU_DOMAIN_NATIVE_HANDLE_ROLE_DOMAIN;
		memcpy(slot.interface_schema_digest, digest, sizeof(digest));
		if (profile == 3)
			slot.interface_schema_digest[0] ^= 1;
		grant_source.generation = profile == 4 ? 16 : 17;
		if (kb2_resource_grant_encode(grant_bytes, sizeof(grant_bytes), &grant_size,
					      &grant_source) != KB2_PROTOCOL_OK ||
		    kb2_resource_grant_decode(grant_bytes, grant_size, &grant) != KB2_PROTOCOL_OK)
			goto failed;
		result = kobox_posix_resource_open(&config, &fixture->runtime);
		if (profile < 4) {
			enum kobox_resource_runtime_status expected = profile == 1 || profile == 3 ?
				KOBOX_RESOURCE_RUNTIME_MALFORMED : KOBOX_RESOURCE_RUNTIME_IMPORT_FAILURE;

			if (result != expected || fixture->runtime || fixture->imported || fixture->released)
				goto failed;
		} else {
			if (result)
				goto failed;
			if (profile == 5) {
				close(wrong_fd);
				return 0;
			}
			if (kobox_boot_resource_port(fixture->runtime, &port) ||
			    kobox_boot_iommu_resource(&port, 2, 0, 101, fixture->ram_size, &iommu) != -EPROTO)
				goto failed;
			kobox_resource_runtime_close(&fixture->runtime);
			if (fixture->imported != 1 || fixture->released != 1)
				goto failed;
		}
		fixture->rejected++;
	}
failed:
	close(wrong_fd);
	kobox_resource_runtime_close(&fixture->runtime);
	return -EPROTO;
}

static int transfer(void *context, uint64_t iova, void *bytes,
		    size_t length, unsigned int write)
{
	uint64_t words[MESSAGE_WORDS] = {TRANSFER, iova, length, write};
	uint32_t value;
	int result;

	if (length != sizeof(value) || write > 1)
		return -EINVAL;
	memcpy(&value, bytes, sizeof(value));
	words[4] = value;
	result = call(context, words, -1);
	if (!result && !write) {
		value = words[1];
		memcpy(bytes, &value, sizeof(value));
	}
	return result;
}

static int engine_loop(int socket, int fail_unmap)
{
	struct kobox_dma_fixture fixture = {.ram = -1, .fail_unmap = fail_unmap};
	uint64_t request[MESSAGE_WORDS], response[MESSAGE_WORDS];
	int fd, result;

	for (;;) {
		result = receive_message(socket, request, &fd);
		if (result)
			return 1;
		if (fixture.unmap_failed) {
			fprintf(stderr, "DMA continued after failed invalidation\n");
			if (fd >= 0)
				close(fd);
			return 1;
		}
		memset(response, 0, sizeof(response));
		result = -EINVAL;
		if ((fd >= 0) != (request[0] == PREPARE)) {
			if (fd >= 0)
				close(fd);
			return 1;
		}
		switch (request[0]) {
		case PREPARE: {
			struct stat stat;

			if (fixture.ram < 0 && request[1] && request[1] <= SIZE_MAX &&
			    !fstat(fd, &stat) && (uint64_t)stat.st_size == request[1]) {
				fixture.ram = fd;
				fixture.ram_size = request[1];
				result = 0;
			} else {
				close(fd);
			}
			break;
		}
		case ENABLE:
			if (request[1] <= 1) {
				fixture.enabled = request[1];
				result = 0;
			}
			break;
		case MAP:
			if (request[3] <= SIZE_MAX && request[4] <= UINT32_MAX)
				result = engine_map(&fixture, request[1], request[2], request[3], request[4]);
			break;
		case UNMAP:
			if (request[2] <= SIZE_MAX)
				result = engine_unmap(&fixture, request[1], request[2]);
			break;
		case TRANSFER: {
			uint32_t value = request[4];

			if (request[2] == sizeof(value) && request[3] <= 1) {
				result = engine_transfer(&fixture, request[1], &value, sizeof(value), request[3]);
				response[1] = value;
			}
			break;
		}
		case FAIL_MAP:
			if (request[1] <= UINT32_MAX) {
				fixture.fail_after = request[1];
				result = 0;
			}
			break;
		case STOP:
			result = fixture.mappings || fixture.enabled ? -EBUSY : 0;
			response[1] = fixture.maps;
			response[2] = fixture.unmaps;
			response[3] = fixture.transfers;
			response[4] = fixture.revoked;
			break;
		}
		response[0] = -result;
		if (send_message(socket, response, -1))
			return 1;
		if (request[0] == STOP) {
			close(fixture.ram);
			return result ? 1 : 0;
		}
	}
}

static void close_fixture(void *context)
{
	struct kobox_dma_fixture *fixture = context;
	uint64_t words[MESSAGE_WORDS] = {STOP};
	pid_t waited;
	int status;

	if (call(fixture, words, -1))
		abort();
	kobox_resource_runtime_close(&fixture->runtime);
	close(fixture->socket);
	do {
		waited = waitpid(fixture->engine, &status, 0);
	} while (waited < 0 && errno == EINTR);
	fprintf(stderr, "DMA device process: maps=%llu unmaps=%llu transfers=%llu revoked=%llu\n",
		(unsigned long long)words[1], (unsigned long long)words[2],
		(unsigned long long)words[3], (unsigned long long)words[4]);
	if (waited != fixture->engine || !WIFEXITED(status) || WEXITSTATUS(status) ||
	    words[1] != words[2] || words[2] != words[4] ||
	    fixture->imported != 2 || fixture->released != 2 || fixture->rejected != 5 ||
	    fixture->pci.map_calls != fixture->pci.unmap_calls || fixture->pci.bad_sizing)
		abort();
	pthread_mutex_destroy(&fixture->lock);
	fprintf(stderr, "IOMMU grant: imports=%u releases=%u rejected=%u\n",
		fixture->imported, fixture->released, fixture->rejected);
	close(fixture->pci.backing);
}

static void panic_report(void *context, unsigned int references,
			 unsigned int online_cpus)
{
	char message[96];
	int length = snprintf(message, sizeof(message),
		"DMA panic observed: page_references=%u online_cpus=%u\n",
		references, online_cpus);

	(void)context;
	if (length > 0 && (size_t)length < sizeof(message))
		(void)write(STDERR_FILENO, message, length);
}

int kobox_dma_fixture_start(const char *core_path, struct kobox_dma_fixture **out)
{
	struct kobox_dma_fixture *fixture;
	struct stat anchor;
	int sockets[2], result;
	struct timeval timeout = {.tv_sec = 5};

	if (!core_path || !out)
		return -EINVAL;
	*out = NULL;
	fixture = calloc(1, sizeof(*fixture));
	if (!fixture)
		return -ENOMEM;
	fixture->ram = -1;
	fixture->socket = -1;
	fixture->pci.backing = -1;
	fixture->generation = 17;
	fixture->object_id = 102;
	fixture->device_id = 101;
	fixture->core_path = core_path;
	result = pthread_mutex_init(&fixture->lock, NULL);
	if (result) {
		free(fixture);
		return -result;
	}
	fixture->dma = (struct kobox_linux_dma_test) {
		.context = fixture, .transfer = transfer, .fail_map = fail_map,
		.panic_report = panic_report,
	};
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) {
		result = -errno;
		goto failed;
	}
	fixture->engine = fork();
	if (fixture->engine < 0) {
		result = -errno;
		close(sockets[0]);
		close(sockets[1]);
		goto failed;
	}
	if (!fixture->engine) {
		close(sockets[0]);
		_exit(engine_loop(sockets[1], 0));
	}
	close(sockets[1]);
	fixture->socket = sockets[0];
	if (fstat(fixture->socket, &anchor)) {
		result = -errno;
		goto failed;
	}
	fixture->anchor_device = anchor.st_dev;
	fixture->anchor_inode = anchor.st_ino;
	if (setsockopt(fixture->socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
	    setsockopt(fixture->socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
		result = -errno;
		goto failed;
	}
	kobox_pci_config_fixture_init(&fixture->pci, &fixture->pci_host);
	fixture->pci.backing = memfd_create("dma-pci-conformance", MFD_CLOEXEC);
	if (fixture->pci.backing < 0 || ftruncate(fixture->pci.backing, 0x5000)) {
		result = -errno;
		goto failed;
	}
	*out = fixture;
	return 0;
failed:
	if (fixture->pci.backing >= 0)
		close(fixture->pci.backing);
	if (fixture->socket >= 0)
		close(fixture->socket);
	/* Before PREPARE, closing the only parent socket makes the engine exit. */
	if (fixture->engine > 0)
		while (waitpid(fixture->engine, NULL, 0) < 0 && errno == EINTR)
			;
	pthread_mutex_destroy(&fixture->lock);
	free(fixture);
	return result;
}

int kobox_dma_fixture_prepare_host(void *context, int descriptor, size_t length)
{
	return prepare(context, descriptor, length);
}

void kobox_dma_fixture_stop(void *context)
{
	close_fixture(context);
	free(context);
}

int kobox_dma_fixture_test_main(int argc, char **argv)
{
	struct kobox_dma_fixture *fixture;
	struct kobox_boot_test_resources resources;

	if (argc != 2 || kobox_dma_fixture_start(argv[1], &fixture))
		return 1;
	resources = (struct kobox_boot_test_resources) {
		.port = &fixture->port, .pci = &fixture->pci_host,
		.dma = &fixture->dma, .prepare_dma = kobox_dma_fixture_prepare_host,
		.close = kobox_dma_fixture_stop, .context = fixture,
	};
	return kobox_boot_test_run(argc, argv, &resources);
}

int kobox_dma_fixture_engine(int socket, int fail_unmap)
{
	return engine_loop(socket, fail_unmap);
}

int kobox_dma_fixture_create(uint64_t generation, uint64_t object_id,
			     uint64_t device_id, dev_t device, ino_t inode,
			     struct kobox_dma_fixture **out)
{
	struct kobox_dma_fixture *fixture;

	if (!out || !generation || !object_id || !device_id)
		return -EINVAL;
	*out = NULL;
	fixture = calloc(1, sizeof(*fixture));
	if (!fixture)
		return -ENOMEM;
	if (pthread_mutex_init(&fixture->lock, NULL)) {
		free(fixture);
		return -ENOMEM;
	}
	fixture->generation = generation;
	fixture->object_id = object_id;
	fixture->device_id = device_id;
	fixture->anchor_device = device;
	fixture->anchor_inode = inode;
	fixture->socket = -1;
	fixture->ram = -1;
	fixture->borrowed_socket = 1;
	fixture->dma = (struct kobox_linux_dma_test) {
		.context = fixture, .transfer = transfer, .fail_map = fail_map,
		.panic_report = panic_report,
	};
	*out = fixture;
	return 0;
}

int kobox_dma_fixture_import(void *context, const kb2_resource_grant_slot_t *slot,
	const kb2_resource_grant_object_t *object,
	const struct kobox_resource_native_handle *handles, size_t count, void **out,
	const struct kobox_resource_interface_operations **operations)
{
	return import_domain(context, slot, object, handles, count, out, operations);
}

void kobox_dma_fixture_release(void *context, void *object)
{
	release_domain(context, object);
}

int kobox_dma_fixture_prepare(struct kobox_dma_fixture *fixture,
			      const struct kobox_linux_resource_port *port,
			      int ram_descriptor, size_t ram_length)
{
	uint64_t words[MESSAGE_WORDS] = {PREPARE, ram_length};
	int result;

	if (!fixture || fixture->socket < 0 || fixture->ram_size)
		return -EINVAL;
	result = call(fixture, words, ram_descriptor);
	if (result)
		return result;
	fixture->ram_size = ram_length;
	result = kobox_boot_iommu_resource(port, 2, 0, fixture->device_id,
					  ram_length, &fixture->iommu);
	if (!result)
		fixture->dma.host = fixture->iommu.host;
	return result;
}

const struct kobox_linux_dma_test *kobox_dma_fixture_test(struct kobox_dma_fixture *fixture)
{
	return &fixture->dma;
}

int kobox_dma_fixture_finish(struct kobox_dma_fixture *fixture)
{
	uint64_t words[MESSAGE_WORDS] = {STOP};
	int result = call(fixture, words, -1);

	if (result)
		return result;
	if (!words[1] || words[1] != words[2] || words[2] != words[4] || !words[3])
		return -EIO;
	fprintf(stderr, "Native DMA cleanup: maps=%llu unmaps=%llu revoked=%llu\n",
		(unsigned long long)words[1], (unsigned long long)words[2],
		(unsigned long long)words[4]);
	return 0;
}

void kobox_dma_fixture_destroy(struct kobox_dma_fixture *fixture)
{
	if (!fixture)
		return;
	if (fixture->socket >= 0 || fixture->imported != fixture->released)
		abort();
	pthread_mutex_destroy(&fixture->lock);
	free(fixture);
}
