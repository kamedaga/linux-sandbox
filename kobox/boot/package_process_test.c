// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../host/posix/package.h"
#include "boot_test.h"
#include "resource_registry.h"
#include "pci_fixture.h"
#include "lifecycle_fixture.h"

#include <kobox2/sha256.h>
#include <kobox2/pci_function_layout.h>
#include <kobox2_test/management.h>
#include <kobox2_test/bootstrap.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Resource cases use an isolated conformance PCI function, not physical
 * hardware. Artifact-only cases retain the separate fork/exec boot Gate.
 */
#define ARTIFACTS 6
#define DESCRIPTORS (2 + ARTIFACTS + 1)

static int diagnostic_log = -1;

static void check(int condition, unsigned int line)
{
	if (!condition) {
		char output[65536];
		ssize_t length = diagnostic_log < 0 ? -1 :
			pread(diagnostic_log, output, sizeof(output), 0);

		if (length > 0)
			(void)write(STDERR_FILENO, output, length);
		fprintf(stderr, "package process check failed at %u: %s\n",
			line, strerror(errno));
		exit(1);
	}
}

#define CHECK(expression) check(!!(expression), __LINE__)

static int immutable(const void *bytes, size_t size)
{
	int descriptor = memfd_create("native-boot-package", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	CHECK(descriptor >= 0);
	CHECK(write(descriptor, bytes, size) == (ssize_t)size);
	CHECK(!fcntl(descriptor, F_ADD_SEALS,
		     F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK));
	return descriptor;
}

static int artifact(const char *path, kb2_closure_manifest_artifact_t *record,
		    int broken)
{
	struct stat info;
	void *bytes;
	size_t size;
	int descriptor, source = open(path, O_RDONLY | O_CLOEXEC);

	CHECK(source >= 0 && !fstat(source, &info));
	CHECK(S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size < (128L << 20));
	size = info.st_size;
	bytes = mmap(NULL, size, PROT_READ, MAP_PRIVATE, source, 0);
	CHECK(bytes != MAP_FAILED);
	if (broken) {
		Elf64_Ehdr header;

		CHECK(size >= sizeof(header));
		memcpy(&header, bytes, sizeof(header));
		/* Still a digest-valid ET_REL package. The native module loader
		 * must reject the missing section table, after earlier loads.
		 */
		header.e_shoff = 0;
		header.e_shnum = 0;
		header.e_shstrndx = 0;
		descriptor = immutable(&header, sizeof(header));
	} else {
		descriptor = immutable(bytes, size);
	}
	CHECK(!munmap(bytes, size));
	close(source);
	/* Hash the sealed copy, not the original file which could change. */
	CHECK(!fstat(descriptor, &info));
	record->content_size = info.st_size;
	bytes = mmap(NULL, info.st_size, PROT_READ, MAP_PRIVATE, descriptor, 0);
	CHECK(bytes != MAP_FAILED);
	kb2_sha256(bytes, info.st_size, record->content_digest);
	CHECK(!munmap(bytes, info.st_size));
	return descriptor;
}

static void package_metadata(kb2_closure_manifest_artifact_t *artifacts,
			     struct kobox_posix_package_input *input,
			     int descriptors[DESCRIPTORS], uint64_t generation,
			     unsigned int count, int resource_case, int bad_rights)
{
	kb2_closure_manifest_dependency_t dependencies[ARTIFACTS - 1];
	kb2_closure_manifest_source_t source = {
		.artifacts = artifacts, .artifact_count = count,
		.dependencies = dependencies, .dependency_count = count - 1,
	};
	kb2_resource_grant_source_t grant = {.generation = generation};
	kb2_closure_manifest_resource_t resource = {
		.slot_id = 1, .type = KB2_CLOSURE_RESOURCE_DEVICE,
		.minimum_count = 1, .maximum_count = 1,
		.required_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
		.maximum_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
		.flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED | KB2_CLOSURE_RESOURCE_FLAG_RESET_REQUIRED,
		.interface_schema_digest = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES,
	};
	kb2_closure_manifest_binding_t binding = {.slot_id = 1, .node_id = count};
	kb2_resource_grant_slot_source_t slot = {
		.slot_id = 1, .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
		.state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
		.interface_schema_digest = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES,
	};
	kb2_resource_grant_object_source_t object = {
		.slot_id = 1, .object_id = 101, .granted_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
	};
	kb2_resource_grant_handle_binding_t handle = {
		.object_id = 101, .role = KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE, .transfer_handle_index = 0,
	};
	uint8_t *bytes;
	size_t size, written;
	unsigned int index;

	if (resource_case) {
		if (bad_rights)
			object.granted_rights = KB2_CLOSURE_DEVICE_RIGHT_COMMAND;
		source.resources = &resource;
		source.resource_count = 1;
		source.bindings = &binding;
		source.binding_count = 1;
		grant.slots = &slot;
		grant.slot_count = 1;
		grant.objects = &object;
		grant.object_count = 1;
		grant.handle_bindings = &handle;
		grant.handle_binding_count = 1;
	}
	for (index = 1; index < count; index++)
		dependencies[index - 1] = (kb2_closure_manifest_dependency_t) {
			.consumer_node_id = index + 1, .provider_node_id = index,
		};
	CHECK(kb2_closure_manifest_encoded_size(&source, &size) == KB2_PROTOCOL_OK);
	bytes = malloc(size);
	CHECK(bytes);
	CHECK(kb2_closure_manifest_encode(bytes, size, &written, &source) == KB2_PROTOCOL_OK);
	descriptors[0] = immutable(bytes, size);
	kb2_sha256(bytes, size, input->expected_manifest_digest);
	memcpy(grant.closure_manifest_digest, input->expected_manifest_digest, 32);
	free(bytes);
	CHECK(kb2_resource_grant_encoded_size(&grant, &size) == KB2_PROTOCOL_OK);
	bytes = malloc(size);
	CHECK(bytes);
	CHECK(kb2_resource_grant_encode(bytes, size, &written, &grant) == KB2_PROTOCOL_OK);
	descriptors[1] = immutable(bytes, size);
	kb2_sha256(bytes, size, input->expected_grant_digest);
	free(bytes);
}

/* This test uses a private single-packet SCM_RIGHTS channel. The expected
 * identity and FD count are launch-owner state inherited before receiving,
 * not fields trusted from the packet under test.
 */
static void transfer(int socket, int descriptors[DESCRIPTORS], unsigned int count, int sending)
{
	size_t received;

	if (sending) {
		CHECK(kb2_test_send_handles(socket, descriptors, count));
	} else {
		CHECK(kb2_test_receive_handles(socket, descriptors, count, &received));
		CHECK(received == count);
	}
}

struct imported_resources {
	struct kobox_resource_runtime *runtime;
	struct kobox_boot_package *package;
	struct kobox_pci_fixture_owner *owner;
	struct kobox_lifecycle_fixture *lifecycle;
};

static void close_resources(void *context)
{
	struct imported_resources *resources = context;

	if (resources->lifecycle)
		kobox_lifecycle_fixture_close(resources->lifecycle);
	kobox_resource_runtime_close(&resources->runtime);
	kobox_posix_package_close(&resources->package);
	CHECK(resources->owner->imported == 1 && resources->owner->released == 1);
	fprintf(stderr, "Native resource cleanup: imports=1 releases=1\n");
}

static void launch(const char *executable, int socket, int *descriptors,
		   struct kobox_posix_package_input input, int expected_error,
		   unsigned int count, struct kobox_pci_fixture_owner *owner,
		   int bad_resource, int init_failure, int managed)
{
	struct kobox_boot_package *package = NULL;
	char paths[ARTIFACTS][64];
	char *arguments[ARTIFACTS + 3] = {(char *)executable, paths[0], "--modules"};
	unsigned int index;
	int result;
	unsigned int transferred = count + 2 + !!owner;

	for (index = 0; index < transferred; index++)
		close(descriptors[index]);
	transfer(socket, descriptors, transferred, 0);
	if (!managed)
		close(socket);
	input.manifest_descriptor = descriptors[0];
	input.grant_descriptor = descriptors[1];
	input.artifact_descriptors = descriptors + 2;
	input.artifact_count = count;
	result = kobox_posix_package_open(&input, &package);
	for (index = 0; index < count + 2; index++)
		close(descriptors[index]);
	CHECK(result == expected_error);
	if (expected_error) {
		CHECK(!package);
		if (owner)
			close(descriptors[count + 2]);
		_exit(0);
	}
	for (index = 0; index < count; index++) {
		int descriptor = kobox_posix_blob_descriptor(&package->artifacts[index]);

		CHECK(!fcntl(descriptor, F_SETFD, 0));
		CHECK(snprintf(paths[index], sizeof(paths[index]),
			       "/proc/self/fd/%d", descriptor) < (int)sizeof(paths[index]));
		if (index)
			arguments[index + 2] = paths[index];
	}
	if (owner) {
		struct imported_resources resources = {.package = package, .owner = owner};
		struct kobox_lifecycle_fixture lifecycle;
		struct kobox_linux_resource_port port;
		struct kobox_posix_resource_config config = {
			.manifest = &package->manifest, .grant = &package->grant,
			.native_handles = &descriptors[count + 2], .native_handle_count = 1,
			.import_object = kobox_pci_fixture_import,
			.release_object = kobox_pci_fixture_release, .object_context = owner,
		};
		struct kobox_boot_test_resources test = {
			.port = &port, .close = close_resources, .context = &resources,
		};

		CHECK(package->grant.generation == owner->generation);
		result = kobox_posix_resource_open(&config, &resources.runtime);
		close(descriptors[count + 2]);
		if (bad_resource) {
			CHECK(result == KOBOX_RESOURCE_RUNTIME_IMPORT_FAILURE);
			CHECK(!resources.runtime && !owner->imported);
			kobox_posix_package_close(&resources.package);
			_exit(0);
		}
		CHECK(result == KOBOX_RESOURCE_RUNTIME_OK);
		CHECK(!kobox_boot_resource_port(resources.runtime, &port));
		if (managed) {
			CHECK(!kobox_lifecycle_fixture_start(&lifecycle, socket,
							    owner->generation));
			resources.lifecycle = &lifecycle;
			test.lifecycle = &lifecycle.port;
		}
		arguments[2] = init_failure ? "--resource-port-fail" : "--resource-port";
		/* The child has never entered hosted Linux. Only imported FDs
		 * and independently inherited owner identity feed this boot.
		 */
		_exit(kobox_boot_test_run(count + 2, arguments, &test));
	}
	/* Only verified immutable FDs survive exec. The existing real boot
	 * launcher uses dlopen and upstream init_module, not the legacy loader.
	 */
	execv(executable, arguments);
	CHECK(0);
}

static int create_pci(void)
{
	const uint8_t identity[4] = {0xf4, 0x1a, 0x50, 0x10};
	int descriptor = memfd_create("isolated-pci-conformance", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	CHECK(descriptor >= 0 && !ftruncate(descriptor, KOBOX_PCI_FIXTURE_SIZE));
	CHECK(pwrite(descriptor, identity, sizeof(identity), 0) == sizeof(identity));
	CHECK(!fcntl(descriptor, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_GROW | F_SEAL_SHRINK));
	return descriptor;
}

static void management_receive(int socket, uint32_t expected_opcode)
{
	uint8_t packet[KB2_TEST_MESSAGE_SIZE];
	struct pollfd wait = {.fd = socket, .events = POLLIN};
	uint64_t correlation, value;
	uint32_t opcode;

	CHECK(poll(&wait, 1, 30000) == 1);
	CHECK(recv(socket, packet, sizeof(packet), MSG_TRUNC) == sizeof(packet));
	CHECK(kb2_test_message_decode(packet, sizeof(packet),
		KB2_TEST_MESSAGE_FLAG_EVENT, 17, &opcode, &correlation, &value));
	CHECK(opcode == expected_opcode && correlation && !value);
}

static void management_stop(int socket, uint64_t generation)
{
	uint8_t packet[KB2_TEST_MESSAGE_SIZE];

	CHECK(kb2_test_message_encode(packet, sizeof(packet),
		KB2_TEST_REQUEST_QUIESCE, 0, generation, 2, 0));
	CHECK(send(socket, packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet));
}

int main(int argc, char **argv)
{
	static const char * const names[ARTIFACTS] = {
		"core", "i2c_core", "drm_panel_orientation_quirks", "drm", "drm_shmem_helper",
		"resource_test",
	};
	struct kobox_posix_package_input input = {.expected_generation = 17};
	kb2_closure_manifest_artifact_t artifacts[ARTIFACTS] = {0};
	int descriptors[DESCRIPTORS], sockets[2], status, pidfd, log;
	int stale, broken, bad_digest, resource_case, bad_resource, bad_rights, init_failure;
	int managed, stale_stop, disconnect, early_stop;
	int owned_device = -1, outside_device = -1;
	struct kobox_pci_fixture_owner device_owner = {.generation = 17, .object_id = 101};
	struct stat device_info;
	const char *scenario;
	char output[65536];
	struct pollfd wait;
	ssize_t length;
	unsigned int index;
	unsigned int count, transferred;
	pid_t child, owner = getpid();

	CHECK(argc == 8 || argc == 9);
	resource_case = argc == 9;
	count = resource_case ? 6 : 5;
	transferred = count + 2 + resource_case;
	scenario = argv[argc - 1];
	stale = !strcmp(scenario, "stale") || !strcmp(scenario, "resource-stale");
	broken = !strcmp(scenario, "module-failure") || !strcmp(scenario, "resource-module-failure") ||
		!strcmp(scenario, "resource-stop-module-failure");
	bad_digest = !strcmp(scenario, "bad-grant");
	bad_resource = !strcmp(scenario, "resource-bad-fd");
	bad_rights = !strcmp(scenario, "resource-rights");
	init_failure = !strcmp(scenario, "resource-init-failure") ||
		!strcmp(scenario, "resource-stop-init-failure");
	stale_stop = !strcmp(scenario, "resource-stale-stop");
	disconnect = !strcmp(scenario, "resource-disconnect");
	early_stop = !strcmp(scenario, "resource-early-stop");
	managed = !strcmp(scenario, "resource-stop") || stale_stop || disconnect ||
		early_stop || !strcmp(scenario, "resource-stop-module-failure") ||
		!strcmp(scenario, "resource-stop-init-failure");
	CHECK(resource_case ? (!strcmp(scenario, "resource") || bad_resource || broken ||
			      bad_rights || stale || init_failure || managed) :
		(stale || broken || bad_digest || !strcmp(scenario, "normal")));
	for (index = 0; index < count; index++) {
		artifacts[index].node_id = index + 1;
		artifacts[index].kind = index ? KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE :
			KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER;
		artifacts[index].flags = KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX |
			(index == count - 1 ? KB2_CLOSURE_ARTIFACT_FLAG_ROOT : 0);
		artifacts[index].namespace_name = (kb2_closure_string_t) {
			.data = names[index], .length = strlen(names[index]),
		};
		descriptors[index + 2] = artifact(argv[index + 2], &artifacts[index],
						broken && index == 3);
	}
	package_metadata(artifacts, &input, descriptors, stale ? 16 : 17, count, resource_case, bad_rights);
	if (resource_case) {
		owned_device = create_pci();
		outside_device = create_pci();
		CHECK(!fstat(owned_device, &device_info));
		device_owner.device = device_info.st_dev;
		device_owner.inode = device_info.st_ino;
		descriptors[count + 2] = fcntl(bad_resource ? outside_device : owned_device, F_DUPFD_CLOEXEC, 0);
		CHECK(descriptors[count + 2] >= 0);
	}
	if (bad_digest)
		input.expected_grant_digest[0] ^= 1;
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets));
	log = memfd_create("package-boot-output", MFD_CLOEXEC);
	CHECK(log >= 0);
	diagnostic_log = log;
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(!prctl(PR_SET_PDEATHSIG, SIGKILL));
		CHECK(getppid() == owner);
		CHECK(dup2(log, STDERR_FILENO) == STDERR_FILENO);
		close(log);
		diagnostic_log = -1;
		close(sockets[0]);
		if (resource_case) {
			close(owned_device);
			close(outside_device);
		}
		launch(argv[1], sockets[1], descriptors, input,
		       stale ? -ESTALE : bad_digest || bad_rights ? -EBADMSG : 0,
		       count, resource_case ? &device_owner : NULL, bad_resource,
		       init_failure, managed);
		_exit(125);
	}
	close(sockets[1]);
	pidfd = syscall(SYS_pidfd_open, child, 0);
	CHECK(pidfd >= 0);
	transfer(sockets[0], descriptors, transferred, 1);
	if (early_stop)
		management_stop(sockets[0], 17);
	if (managed && !broken && !init_failure) {
		uint32_t marker, before, after;
		struct pollfd alive = {.fd = pidfd, .events = POLLIN};

		management_receive(sockets[0], KB2_TEST_EVENT_READY);
		if (stale_stop) {
			management_stop(sockets[0], 16);
			management_receive(sockets[0], KB2_TEST_EVENT_FAULT);
		}
		/* Ready must represent a live native module and mapped BAR,
		 * not an event emitted after the original auto-unload fixture.
		 */
		if (!early_stop) {
			CHECK(poll(&alive, 1, 0) == 0);
			CHECK(pread(owned_device, &marker, sizeof(marker), 4096) == sizeof(marker));
			CHECK(marker == 0x72657331U);
			CHECK(pread(owned_device, &before, sizeof(before), 4104) == sizeof(before));
			for (index = 0; index < 30; index++) {
				CHECK(poll(&alive, 1, 100) == 0);
				CHECK(pread(owned_device, &after, sizeof(after), 4104) == sizeof(after));
				if (after > before)
					break;
			}
			CHECK(after > before);
		}
		if (!disconnect && !early_stop)
			management_stop(sockets[0], 17);
	}
	close(sockets[0]);
	for (index = 0; index < transferred; index++)
		close(descriptors[index]);
	wait = (struct pollfd) {.fd = pidfd, .events = POLLIN};
	do {
		status = poll(&wait, 1, 45000);
	} while (status < 0 && errno == EINTR);
	if (status != 1)
		CHECK(!kill(child, SIGKILL));
	CHECK(waitpid(child, &status, 0) == child);
	close(pidfd);
	length = pread(log, output, sizeof(output) - 1, 0);
	CHECK(length >= 0 && length < (ssize_t)sizeof(output) - 1);
	output[length] = '\0';
	close(log);
	diagnostic_log = -1;
	fputs(output, stderr);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) ==
	      (broken || init_failure || disconnect ? 1 : 0));
	if (!stale && !bad_digest && !bad_resource && !bad_rights) {
		CHECK(strstr(output, "Native module lifecycle:"));
		CHECK(strstr(output, broken ? "loaded=2 unloaded=2 warnings=0" :
			     init_failure ? "loaded=4 unloaded=4 warnings=0" :
			     resource_case ? "loaded=5 unloaded=5 warnings=0" :
			     "loaded=4 unloaded=4 warnings=0"));
	}
	if (resource_case) {
		uint32_t marker;
		uint8_t reset[KOBOX_PCI_FIXTURE_SIZE] = {0xf4, 0x1a, 0x50, 0x10};
		uint8_t outside[KOBOX_PCI_FIXTURE_SIZE];

		CHECK(pread(owned_device, &marker, sizeof(marker), 4096) == sizeof(marker));
		CHECK(marker == (!bad_resource && !broken && !bad_rights && !stale ?
			(init_failure ? 0x72657331U : 0x72657332U) : 0));
		CHECK(pread(owned_device, &marker, sizeof(marker), 4100) == sizeof(marker));
		CHECK(marker == (!bad_resource && !broken && !bad_rights && !stale ? 0x62617234U : 0));
		CHECK(pread(owned_device, &marker, sizeof(marker), 0x40) == sizeof(marker));
		CHECK(marker == (!bad_resource && !broken && !bad_rights && !stale ? 0x12345678U : 0));
		if (!bad_resource && !bad_rights && !stale) {
			CHECK(strstr(output, "Native resource cleanup: imports=1 releases=1"));
		}
		/* Only the parent owns reset; process death/reaping above proves
		 * imported mappings are gone. This isolated backend's reset domain
		 * is exactly one owned memfd/function, never the adjacent device.
		 */
		CHECK(pwrite(owned_device, reset, sizeof(reset), 0) == sizeof(reset));
		CHECK(pread(outside_device, outside, sizeof(outside), 0) == sizeof(outside));
		CHECK(!memcmp(outside, reset, sizeof(reset)));
		close(owned_device);
		close(outside_device);
	}
	printf("native boot transfer: %s passed\n", scenario);
	return 0;
}
