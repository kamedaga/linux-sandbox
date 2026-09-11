// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "boot_test.h"
#include "virtio_gate.h"
#include "../tests/linux/qemu/qemu_pci.h"
#include "../tests/linux/qemu/qemu_remote.h"
#include "../tests/linux/qemu/qtest.h"
#include "../host/posix/device_channel.h"
#include "../host/posix/vm_service.h"
#include "../task/posix_machine.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const struct {
	const char *name, *path;
} modules[] = {
	{"i2c_core", "drivers/i2c/i2c-core.ko"},
	{"drm_panel_orientation_quirks", "drivers/gpu/drm/drm_panel_orientation_quirks.ko"},
	{"drm", "drivers/gpu/drm/drm.ko"},
	{"drm_shmem_helper", "drivers/gpu/drm/drm_shmem_helper.ko"},
	{"virtio_ring", "drivers/virtio/virtio_ring.ko"},
	{"virtio", "drivers/virtio/virtio.ko"},
	{"virtio_pci_modern_dev", "drivers/virtio/virtio_pci_modern_dev.ko"},
	{"virtio_pci", "drivers/virtio/virtio_pci.ko"},
	{"virtio_dma_buf", "drivers/virtio/virtio_dma_buf.ko"},
	{"drm_kms_helper", "drivers/gpu/drm/drm_kms_helper.ko"},
	{"virtio_gpu", "drivers/gpu/drm/virtio/virtio-gpu.ko"},
	{"dma_consumer_test", "kobox/gem/dma_consumer_test.ko"},
};

struct hardware {
	int argc;
	char **argv;
	struct kobox_qemu_remote *remote;
	struct kobox_posix_vm_service *vm;
	bool fault, dma_death, revoke_failure, virgl;
	struct kobox_posix_proxy_device *remote_device, *remote_consumer;
	const char *executable;
	uint64_t irq_delay;
	struct kobox_qemu_pci *device;
	struct kobox_qemu_pci *consumer;
	unsigned int prime;
	struct kobox_linux_virtio_test test;
	struct kobox_linux_native_module images[sizeof(modules) / sizeof(modules[0])];
	struct kobox_exec_file *client_files;
	size_t client_file_count;
};

static atomic_bool fail_invalidation;

int __real_kobox_qtest_command(struct kobox_qtest *test, char *response, size_t capacity,
			       const char *format, ...);
int __wrap_kobox_qtest_command(struct kobox_qtest *test, char *response, size_t capacity,
			       const char *format, ...)
{
	char command[256];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(command, sizeof(command), format, arguments);
	va_end(arguments);
	if (length < 0 || (size_t)length >= sizeof(command))
		return EOVERFLOW;
	/* Only the explicitly armed failure trial suppresses one actual VT-d
	 * context invalidation. No successful hardware reply is synthesized.
	 */
	if (!strncmp(command, "writeq 0xfed90028 ", 18) &&
	    atomic_exchange_explicit(&fail_invalidation, false, memory_order_acq_rel))
		return EIO;
	return __real_kobox_qtest_command(test, response, capacity, "%s", command);
}

static void arm_invalidation_failure(void)
{
	atomic_store_explicit(&fail_invalidation, true, memory_order_release);
}

static int notify(void *context, unsigned int cpu)
{
	(void)context;
	return kobox_task_posix_operations.cpu_notify(cpu, KOBOX_LINUX_TASK_DEVICE_IRQ);
}

static int death_checkpoint(void *context)
{
	struct hardware *hardware = context;

	if (hardware->test.observation == KOBOX_VIRTIO_OBSERVE_SHARED)
		fprintf(stderr, "Mesa generation death checkpoint: actual-poll=2 pending-driver-fences=2\n");
	kobox_qemu_remote_checkpoint(hardware->remote);
}

static int arm_fault(void *context, uint64_t address)
{
	struct hardware *hardware = context;

	return kobox_posix_vm_service_fault_checkpoint(hardware->vm, address,
		death_checkpoint, hardware);
}

static void vm_ready(void *context, struct kobox_posix_vm_service *service)
{
	struct hardware *hardware = context;

	hardware->vm = service;
}

static int prepare(void *context, int descriptor, size_t size)
{
	struct hardware *hardware = context;
	int result;

	if (hardware->remote) {
		result = kobox_qemu_remote_open(hardware->remote, 1, descriptor, size,
			hardware->irq_delay, &hardware->remote_device);
		if (result)
			return result;
		hardware->test.pci = kobox_posix_device_proxy_pci(hardware->remote_device);
		hardware->test.dma = kobox_posix_device_proxy_dma(hardware->remote_device);
		hardware->test.irq = kobox_posix_device_proxy_irq(hardware->remote_device);
		if (hardware->prime) {
			result = kobox_qemu_remote_open(hardware->remote, 2, descriptor, size, 0,
				&hardware->remote_consumer);
			if (result)
				return result;
			hardware->test.consumer_pci = kobox_posix_device_proxy_pci(hardware->remote_consumer);
			hardware->test.consumer_dma = kobox_posix_device_proxy_dma(hardware->remote_consumer);
		}
		return kobox_qemu_remote_start(hardware->remote, notify, NULL);
	}
	result = kobox_qemu_pci_create(&hardware->device, hardware->executable, descriptor, size,
		hardware->virgl);
	if (result)
		return result;
	result = kobox_qemu_pci_irq_delay(hardware->device, hardware->irq_delay);
	if (result)
		return result;
	hardware->test.pci = kobox_qemu_pci_host(hardware->device);
	hardware->test.dma = kobox_qemu_pci_dma(hardware->device);
	hardware->test.irq = kobox_qemu_pci_irq(hardware->device);
	if (hardware->prime) {
		result = kobox_qemu_pci_create_dma_consumer(&hardware->consumer,
					 hardware->executable, descriptor, size);
		if (result)
			return result;
		hardware->test.consumer_pci = kobox_qemu_pci_host(hardware->consumer);
		hardware->test.consumer_dma = kobox_qemu_pci_dma(hardware->consumer);
		/* The hardware thread advances QEMU's device timers. EDU DMA is
		 * synchronously polled by its native test driver, without IRQs.
		 */
		result = kobox_qemu_pci_irq_start(hardware->consumer, notify, NULL);
		if (result)
			return result;
	}
	return kobox_qemu_pci_irq_start(hardware->device, notify, NULL);
}

static void close_hardware(void *context)
{
	struct hardware *hardware = context;
	size_t index;

	if (hardware->remote) {
		if (kobox_posix_device_proxy_close(hardware->remote_device) ||
		    (hardware->remote_consumer && kobox_posix_device_proxy_close(hardware->remote_consumer)) ||
		    kobox_qemu_remote_stop(hardware->remote))
			abort();
	} else {
		if (kobox_qemu_pci_close(hardware->device))
			abort();
		if (hardware->consumer && kobox_qemu_pci_close(hardware->consumer))
			abort();
	}
	for (index = 0; index < hardware->test.count; index++)
		if (munmap((void *)hardware->images[index].image, hardware->images[index].length))
			abort();
	for (index = 0; index < hardware->client_file_count; index++)
		if (munmap((void *)hardware->client_files[index].data,
			   hardware->client_files[index].length))
			abort();
	free(hardware->client_files);
}

static int run(void *context, struct kobox_qemu_remote *remote)
{
	struct hardware *hardware = context;
	struct kobox_boot_test_resources resources = {
		.virtio = &hardware->test, .prepare_dma = prepare,
		.client_files = hardware->client_files,
		.client_file_count = hardware->client_file_count,
		.vm_ready = vm_ready,
		.close = close_hardware, .context = hardware,
	};
	char **argv = hardware->argv;
	char *boot_argv[5];

	hardware->remote = remote;
	hardware->test.death_context = hardware;
	if (hardware->argc == 4)
		return kobox_boot_test_run(2, argv, &resources);
	boot_argv[0] = argv[0];
	boot_argv[1] = argv[1];
	boot_argv[2] = "--elf-exec";
	boot_argv[3] = argv[4];
	boot_argv[4] = argv[5];
	return kobox_boot_test_run(5, boot_argv, &resources);
}

#define VERIFY(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "generation revoke check failed at %u: %s\n", __LINE__, #condition); \
		return 1; \
	} \
} while (0)

static void *expected_fault;

static void alias_bus(int signal, siginfo_t *info, void *context)
{
	(void)context;
	_exit(signal == SIGBUS && info->si_code == BUS_ADRERR &&
	      info->si_addr == expected_fault ? 0 : 1);
}

static int alias_faults(void *address)
{
	int status;
	pid_t child, waited;

	expected_fault = address;
	child = fork();
	VERIFY(child >= 0);
	if (!child) {
		struct rlimit limit = {0};
		struct sigaction action = {.sa_sigaction = alias_bus, .sa_flags = SA_SIGINFO};

		if (setrlimit(RLIMIT_CORE, &limit) || sigemptyset(&action.sa_mask) ||
		    sigaction(SIGBUS, &action, NULL))
			_exit(1);
		(void)*(volatile unsigned char *)address;
		_exit(1);
	}
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	VERIFY(waited == child && WIFEXITED(status) && !WEXITSTATUS(status));
	return 0;
}

static int old_generation(struct kobox_qemu_remote_failure *failure,
			   uint64_t generation, uint64_t target)
{
	struct kobox_device_packet packet = {
		.generation = target, .object = 1, .sequence = 1,
		.operation = KB2_DEVICE_PORT_OP_CONFIG_READ, .count = 2, .values = {0, 4},
	};
	struct stat state;
	void *cold;
	unsigned int events = 0;
	int result, descriptor = -1;

	VERIFY(failure->checkpoint && failure->killed && failure->reaped && failure->revoked);
	VERIFY(failure->backing >= 0 && failure->request >= 0 && failure->event >= 0);
	VERIFY(!fstat(failure->backing, &state) && !state.st_size);
	VERIFY(ftruncate(failure->backing, (off_t)failure->length) == -1 && errno == EPERM);
	VERIFY(!alias_faults(failure->shared_alias));
	VERIFY(!alias_faults((unsigned char *)failure->shared_alias + failure->length - 1));
	VERIFY(!alias_faults(failure->private_alias));
	cold = mmap(NULL, 4096, PROT_READ, MAP_SHARED, failure->backing, 0);
	VERIFY(cold != MAP_FAILED && !alias_faults(cold));
	VERIFY(!munmap(cold, 4096));
	result = kobox_device_send(failure->request, &packet, -1, MSG_DONTWAIT);
	VERIFY(result == EPIPE || result == ECONNRESET);
	VERIFY(!fcntl(failure->event, F_SETFL, O_NONBLOCK));
	while (!(result = kobox_device_receive(failure->event, &packet, &descriptor))) {
		VERIFY(descriptor < 0 && packet.generation == generation && !packet.sequence &&
			packet.operation == KB2_DEVICE_PORT_OP_IRQ_EVENT && ++events < 4096);
	}
	VERIFY(result == EAGAIN || result == EPIPE);
	return 0;
}

static int reaped_clients(unsigned int expected)
{
	unsigned int killed = 0;
	int status;
	pid_t child;

	/* This executable owns its entire process tree. PTRACE_O_EXITKILL
	 * ends native client tasks independently of the dead Linux runtime.
	 */
	for (;;) {
		child = waitpid(-1, &status, __WALL);
		if (child < 0 && errno == EINTR)
			continue;
		if (child < 0)
			break;
		VERIFY(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		killed++;
	}
	fprintf(stderr, "Generation death: native clients reaped=%u\n", killed);
	VERIFY(errno == ECHILD && killed == expected);
	return 0;
}

static int revoke_wait(struct hardware *hardware)
{
	struct kobox_qemu_remote_failure failures[2];
	struct kobox_device_packet retired[2];
	struct kobox_qemu_remote_trial replay = {.events = retired, .count = 2};
	bool shared = hardware->test.observation == KOBOX_VIRTIO_OBSERVE_SHARED;
	unsigned int cpu, old, trials = shared ? 1 : 2;
	int result;

	VERIFY(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
	hardware->test.death_checkpoint = hardware->dma_death ? NULL : death_checkpoint;
	hardware->test.arm_fault = hardware->fault ? arm_fault : NULL;
	replay.count = trials;
	for (cpu = 0; cpu < trials; cpu++) {
		struct kobox_qemu_remote_trial trial = {
			.dma_write = hardware->dma_death ? 1 + 4 * cpu : 0,
			.before_revoke = hardware->revoke_failure ? arm_invalidation_failure : NULL,
		};

		hardware->test.death_cpu = cpu;
		result = kobox_qemu_remote_run(hardware->executable, cpu + 1, hardware->prime,
			hardware->virgl, run, hardware, &failures[cpu], &trial);
		VERIFY(!result);
		if (hardware->dma_death)
			VERIFY(failures[cpu].dma_pending && failures[cpu].dma_blocked);
		VERIFY(failures[cpu].hardware_terminated == (unsigned int)hardware->revoke_failure);
		VERIFY(!atomic_load_explicit(&fail_invalidation, memory_order_acquire));
		retired[cpu] = failures[cpu].retired_irq;
		VERIFY(retired[cpu].generation == cpu + 1);
		/* CPU 0's native context is already closed before the CPU 1 case. */
		VERIFY(!reaped_clients(4 + hardware->fault - hardware->dma_death - cpu - shared));
		for (old = 0; old <= cpu; old++)
			VERIFY(!old_generation(&failures[old], old + 1, cpu + 2));
	}
	/* Keep both generations' FDs and aliases alive during the fresh boot. */
	hardware->test.death_checkpoint = NULL;
	hardware->test.arm_fault = NULL;
	VERIFY(!kobox_qemu_remote_run(hardware->executable, trials + 1, hardware->prime,
		hardware->virgl, run, hardware, NULL, &replay));
	for (old = 0; old < trials; old++) {
		struct kobox_qemu_remote_failure *failure = &failures[old];

		VERIFY(!old_generation(failure, old + 1, trials + 1));
		VERIFY(!munmap(failure->shared_alias, failure->length));
		VERIFY(!munmap(failure->private_alias, failure->length));
		VERIFY(!close(failure->backing) && !close(failure->request) && !close(failure->event));
	}
	printf("Generation revoke: %s%s; host-only DMA/IRQ/RAM revoke, "
	       "stale FD/alias isolation, generation %u restart passed\n",
	       shared ? "concurrent Mesa/PRIME poll waits killed" :
	       hardware->dma_death ? "native consumer DMA and ioctl killed on both CPUs" :
	       "real binary/timeline ioctl waits killed on both CPUs",
	       hardware->fault ? " and pending GEM fault" : "", trials + 1);
	return 0;
}

static int client_files(struct hardware *hardware, int *argc, char **argv)
{
	size_t count, index;

	if (*argc < 9 || strcmp(argv[6], "--client-file"))
		return 0;
	if ((*argc - 6) % 3)
		return EINVAL;
	count = (*argc - 6) / 3;
	hardware->client_files = calloc(count, sizeof(*hardware->client_files));
	if (!hardware->client_files)
		return ENOMEM;
	for (index = 0; index < count; index++) {
		struct kobox_exec_file *file = &hardware->client_files[index];
		struct stat state;
		int descriptor, result;

		if (strcmp(argv[6 + 3 * index], "--client-file"))
			return EINVAL;
		file->path = argv[7 + 3 * index];
		descriptor = open(argv[8 + 3 * index], O_RDONLY | O_CLOEXEC);
		if (descriptor < 0)
			return errno;
		result = fstat(descriptor, &state) ? errno : 0;
		if (!result && (!S_ISREG(state.st_mode) || state.st_size <= 0 ||
		    (uint64_t)state.st_size > SIZE_MAX))
			result = EINVAL;
		if (!result) {
			file->data = mmap(NULL, state.st_size, PROT_READ,
					  MAP_PRIVATE, descriptor, 0);
			if (file->data == MAP_FAILED)
				result = errno;
		}
		close(descriptor);
		if (result)
			return result;
		file->length = state.st_size;
		hardware->client_file_count++;
	}
	*argc = 6;
	return 0;
}

int main(int argc, char **argv)
{
	struct hardware hardware = {0};
	bool shared_death = argc > 1 && !strcmp(argv[argc - 1], "--virgl-revoke");
	bool shared = shared_death || (argc > 1 && !strcmp(argv[argc - 1], "--virgl-shared"));
	bool virgl = shared || (argc > 1 && !strcmp(argv[argc - 1], "--virgl"));
	bool fault = argc > 1 && !strcmp(argv[argc - 1], "--revoke-fault");
	bool revoke_failure = argc > 1 && !strcmp(argv[argc - 1], "--revoke-dma-failure");
	bool dma = revoke_failure || (argc > 1 && !strcmp(argv[argc - 1], "--revoke-dma"));
	bool failure = shared_death || dma || fault || (argc > 1 && !strcmp(argv[argc - 1], "--revoke-wait"));
	bool remote = virgl || failure || (argc > 1 && !strcmp(argv[argc - 1], "--remote"));
	size_t index;

	if (remote)
		argc--;
	if (client_files(&hardware, &argc, argv))
		return 1;
	hardware.argc = argc;
	hardware.argv = argv;
	hardware.fault = fault;
	hardware.virgl = virgl;
	hardware.dma_death = dma;
	hardware.revoke_failure = revoke_failure;
	if (argc != 4 && argc != 6 && argc != 7)
		return 1;
	if (argc == 7) {
		if (strcmp(argv[6], "--delayed-irq") && strcmp(argv[6], "--remove-irq") &&
		    strcmp(argv[6], "--prime"))
			return 1;
		hardware.irq_delay = 500000000;
		hardware.test.observation = !strcmp(argv[6], "--remove-irq") ? KOBOX_VIRTIO_OBSERVE_REMOVE : 0;
		hardware.prime = !strcmp(argv[6], "--prime");
	}
	if (shared)
		hardware.test.observation = KOBOX_VIRTIO_OBSERVE_SHARED;
	if ((dma && !hardware.prime) || (failure && !dma && !hardware.test.observation))
		return 1;
	hardware.executable = argv[2];
	hardware.test.size = sizeof(hardware.test);
	hardware.test.modules = hardware.images;
	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		struct kobox_linux_native_module *image = &hardware.images[index];
		struct stat status;
		char path[4096];
		int length, descriptor;

		if (!hardware.prime && index == sizeof(modules) / sizeof(modules[0]) - 1)
			break;
		length = snprintf(path, sizeof(path), "%s/%s", argv[3], modules[index].path);
		if (length < 0 || (size_t)length >= sizeof(path))
			return 1;
		descriptor = open(path, O_RDONLY | O_CLOEXEC);
		if (descriptor < 0 || fstat(descriptor, &status) || status.st_size <= 0)
			return 1;
		image->image = mmap(NULL, status.st_size, PROT_READ, MAP_PRIVATE, descriptor, 0);
		close(descriptor);
		if (image->image == MAP_FAILED)
			return 1;
		image->length = status.st_size;
		image->name = modules[index].name;
		hardware.test.count++;
	}
	if (failure)
		return revoke_wait(&hardware);
	if (remote)
		return kobox_qemu_remote_run(hardware.executable, 1, hardware.prime,
			hardware.virgl, run, &hardware, NULL, NULL);
	return run(&hardware, NULL);
}
