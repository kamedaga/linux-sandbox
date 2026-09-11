// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "qemu_remote.h"
#include "../../../host/posix/device_channel.h"
#include "../../../runtime/device_session.h"
#include "../../../host/posix/host.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define OBJECTS 2

struct kobox_qemu_remote {
	struct kobox_posix_device_proxy *proxy;
	int checkpoint;
	size_t expected_retired;
};

int kobox_qemu_remote_open(struct kobox_qemu_remote *remote, uint64_t object,
	int ram_descriptor, size_t ram_size, uint64_t delay,
	struct kobox_posix_proxy_device **out)
{
	return kobox_posix_device_proxy_open(remote->proxy, object, ram_descriptor,
					    ram_size, delay, out);
}

int kobox_qemu_remote_start(struct kobox_qemu_remote *remote,
	int (*notify)(void *, unsigned int), void *context)
{
	return kobox_posix_device_proxy_start(remote->proxy, notify, context);
}

int kobox_qemu_remote_stop(struct kobox_qemu_remote *remote)
{
	size_t retired, delivered;
	int result = kobox_posix_device_proxy_stop(remote->proxy);

	if (result)
		return result;
	result = kobox_posix_device_proxy_statistics(remote->proxy, &retired, &delivered);
	if (result)
		return result;
	if (remote->expected_retired) {
		fprintf(stderr, "Generation IRQ: retired=%zu expected=%zu fresh=%zu\n",
			retired, remote->expected_retired, delivered);
		if (retired != remote->expected_retired || !delivered)
			return EIO;
	}
	return 0;
}

struct owner_lease {
	void *address;
	size_t length;
	uint64_t token;
	struct owner_lease *next;
};

struct owner;

struct owned_device {
	struct owner *owner;
	struct kobox_qemu_pci *hardware;
	struct owner_lease *leases;
	uint64_t object, token;
	bool opened;
	/* Written only by this device's poller, read after its join. */
	struct kobox_device_packet first_irq;
};

struct owner {
	const char *executable;
	struct kobox_device_session session;
	uint64_t requests;
	bool consumer, virgl;
	int request, event, checkpoint;
	struct kobox_qemu_remote_failure *failure;
	unsigned int dma_write, dma_writes;
	uint64_t dma_offset, dma_before;
	void (*before_revoke)(void);
	struct kobox_posix_memory_backing ram;
	struct owned_device devices[OBJECTS];
};

static int owner_notify(void *context, unsigned int cpu)
{
	struct owned_device *device = context;
	struct kobox_device_packet event = {
		.generation = device->owner->session.generation, .object = device->object,
		.operation = KB2_DEVICE_PORT_OP_IRQ_EVENT, .count = 1, .values = {cpu},
	};
	int result = kobox_device_send(device->owner->event, &event, -1, MSG_DONTWAIT);

	if (!result && !device->first_irq.generation)
		device->first_irq = event;

	/* This callback does not consume a pending hardware route. The QEMU
	 * poller retries every pending, unmasked, non-servicing route on every
	 * pass, so EAGAIN defers delivery without losing another CPU's wake.
	 * A dead receiver is handled by the owner's independent pidfd path.
	 */
	return result == EAGAIN || result == EPIPE || result == ECONNREFUSED ? 0 : result;
}

static int register_ram(struct owner *owner, uint64_t length, int *descriptor)
{
	struct stat state;
	int seals, flags;

	if (owner->ram.initialized)
		return *descriptor < 0 && owner->ram.size == length ? 0 : EINVAL;
	if (*descriptor < 0 || length < (16UL << 20) || length > (1UL << 30) || length % 4096)
		return EINVAL;
	if (fstat(*descriptor, &state))
		return errno;
	if (!S_ISREG(state.st_mode) || state.st_size != (off_t)length)
		return EINVAL;
	seals = fcntl(*descriptor, F_GET_SEALS);
	flags = fcntl(*descriptor, F_GETFL);
	if (seals < 0 || (seals & (F_SEAL_GROW | F_SEAL_SEAL)) != (F_SEAL_GROW | F_SEAL_SEAL) ||
	    seals & (F_SEAL_SHRINK | F_SEAL_WRITE) || flags < 0 || (flags & O_ACCMODE) != O_RDWR)
		return EPERM;
	owner->ram = (struct kobox_posix_memory_backing) {
		.descriptor = *descriptor, .size = length, .initialized = true,
	};
	*descriptor = -1;
	return 0;
}

static int owner_open(struct owned_device *device, const struct kobox_device_packet *request,
		       struct kobox_device_packet *reply, int *descriptor)
{
	struct owner *owner = device->owner;
	const struct kobox_linux_pci_host *pci;
	const struct kobox_linux_dma_host *dma;
	int result;

	if (device->opened)
		return EBUSY;
	if (device->object == 2 && !owner->consumer)
		return EPERM;
	result = register_ram(owner, request->values[0], descriptor);
	if (result)
		return result;
	/* A failed start never lets the peer reopen the same object identity. */
	device->opened = true;
	if (device->object == 1)
		result = kobox_qemu_pci_create(&device->hardware, owner->executable,
			owner->ram.descriptor, owner->ram.size, owner->virgl);
	else
		result = kobox_qemu_pci_create_dma_consumer(&device->hardware, owner->executable,
			owner->ram.descriptor, owner->ram.size);
	if (result)
		return result;
	result = kobox_qemu_pci_irq_delay(device->hardware, request->values[1]);
	if (!result)
		result = kobox_qemu_pci_irq_start(device->hardware, owner_notify, device);
	if (result)
		return result;
	pci = kobox_qemu_pci_host(device->hardware);
	dma = kobox_qemu_pci_dma(device->hardware);
	if (pci->window_count != 1)
		return EOPNOTSUPP;
	reply->count = 9;
	reply->values[0] = pci->segment;
	reply->values[1] = pci->bus;
	reply->values[2] = pci->devfn;
	reply->values[3] = pci->window_count;
	reply->values[4] = pci->windows[0].start;
	reply->values[5] = pci->windows[0].length;
	reply->values[6] = dma->aperture_start;
	reply->values[7] = dma->aperture_end;
	reply->values[8] = dma->coherent;
	return 0;
}

static int owner_map(struct owned_device *device, const uint64_t *values, uint64_t *token)
{
	const struct kobox_linux_pci_host *pci = kobox_qemu_pci_host(device->hardware);
	void *address;
	struct owner_lease *lease;
	int result;

	if (!values[1] || values[1] > (1UL << 30) || (values[0] | values[1]) % 4096)
		return EINVAL;
	if (values[3] != KOBOX_MMIO_UC && values[3] != KOBOX_MMIO_UC_MINUS)
		return EOPNOTSUPP;
	if (device->token == UINT64_MAX)
		return ENOSPC;
	lease = calloc(1, sizeof(*lease));
	if (!lease)
		return ENOMEM;
	address = mmap(NULL, values[1], PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (address == MAP_FAILED) {
		result = errno;
		free(lease);
		return result;
	}
	result = -pci->memory_map(pci->context, address, values[0], values[1], values[2], values[3]);
	if (result) {
		if (munmap(address, values[1]))
			abort();
		free(lease);
		return result;
	}
	*token = ++device->token;
	*lease = (struct owner_lease) {address, values[1], *token, device->leases};
	device->leases = lease;
	return 0;
}

static int owner_unmap(struct owned_device *device, uint64_t token, uint64_t length)
{
	const struct kobox_linux_pci_host *pci = kobox_qemu_pci_host(device->hardware);
	struct owner_lease **position;

	for (position = &device->leases; *position; position = &(*position)->next) {
		struct owner_lease *lease = *position;
		int result;

		if (!token || lease->token != token || lease->length != length)
			continue;
		result = -pci->memory_unmap(pci->context, lease->address, lease->length);
		if (result)
			return result;
		if (munmap(lease->address, lease->length))
			abort();
		*position = lease->next;
		free(lease);
		return 0;
	}
	return ESTALE;
}

static int dispatch(struct owner *owner, const struct kobox_device_packet *request,
		     struct kobox_device_packet *reply, int *descriptor)
{
	const struct kobox_linux_pci_host *pci;
	const struct kobox_linux_dma_host *dma;
	const struct kobox_linux_irq_host *irq;
	struct kobox_linux_irq_route routes[KB2_DEVICE_PORT_MAX_ROUTES];
	struct kobox_linux_irq_event event;
	struct owned_device *device;
	const uint64_t *v = request->values;
	unsigned int i, active;
	uint32_t value;
	int result;

	if (kobox_device_session_accept(&owner->session, request, OBJECTS) !=
	    KOBOX_DEVICE_SESSION_OK)
		return ESTALE;
	owner->requests++;
	result = kobox_device_error(kobox_device_request_arguments(request));
	if (result)
		return result;
	device = &owner->devices[request->object - 1];
	if (request->operation == KB2_DEVICE_PORT_OP_OPEN)
		return owner_open(device, request, reply, descriptor);
	if (*descriptor >= 0)
		return EINVAL;
	if (!device->hardware)
		return ESTALE;
	pci = kobox_qemu_pci_host(device->hardware);
	dma = kobox_qemu_pci_dma(device->hardware);
	irq = kobox_qemu_pci_irq(device->hardware);
	switch (request->operation) {
	case KB2_DEVICE_PORT_OP_CLOSE:
		result = kobox_qemu_pci_close(device->hardware);
		if (!result)
			device->hardware = NULL;
		return result;
	case KB2_DEVICE_PORT_OP_CONFIG_READ:
		result = -pci->config_read(pci->context, v[0], v[1], &value);
		if (!result) {
			reply->count = 1;
			reply->values[0] = value;
		}
		return result;
	case KB2_DEVICE_PORT_OP_CONFIG_WRITE:
		return -pci->config_write(pci->context, v[0], v[1], v[2]);
	case KB2_DEVICE_PORT_OP_MMIO_MAP:
		reply->count = 1;
		return owner_map(device, v, reply->values);
	case KB2_DEVICE_PORT_OP_MMIO_UNMAP:
		return owner_unmap(device, v[0], v[1]);
	case KB2_DEVICE_PORT_OP_MMIO_READ:
		reply->count = 1;
		return -pci->memory_read(pci->context, v[0], v[1], reply->values);
	case KB2_DEVICE_PORT_OP_MMIO_WRITE:
		if (owner->dma_write && device->object == 2 &&
		    v[0] == pci->windows[0].start + 0x98 && v[1] == 8 && v[2] == 3 &&
		    ++owner->dma_writes == owner->dma_write) {
			result = kobox_qemu_pci_clock_hold(device->hardware);
			if (!result)
				result = -pci->memory_write(pci->context, v[0], v[1], v[2]);
			if (!result)
				result = kobox_qemu_pci_dma_checkpoint(device->hardware,
					&owner->dma_offset, &owner->dma_before);
			if (!result)
				owner->failure->dma_pending = true;
			return result;
		}
		return -pci->memory_write(pci->context, v[0], v[1], v[2]);
	case KB2_DEVICE_PORT_OP_DMA_ENABLE:
		return -dma->enable(dma->context, v[0]);
	case KB2_DEVICE_PORT_OP_DMA_MAP:
		return -dma->map(dma->context, v[0], v[1], v[2], v[3]);
	case KB2_DEVICE_PORT_OP_DMA_UNMAP:
		return -dma->unmap(dma->context, v[0], v[1]);
	case KB2_DEVICE_PORT_OP_IRQ_ALLOCATE:
		if (!v[2] || v[2] > KB2_DEVICE_PORT_MAX_ROUTES || v[0] > KOBOX_IRQ_MSIX)
			return EINVAL;
		result = -irq->allocate(irq->context, v[0], v[1], v[2], v[3], routes);
		if (result)
			return result;
		reply->count = v[2] * 4;
		for (i = 0; i < v[2]; i++) {
			reply->values[i * 4] = routes[i].hwirq;
			reply->values[i * 4 + 1] = routes[i].cookie;
			reply->values[i * 4 + 2] = routes[i].address;
			reply->values[i * 4 + 3] = routes[i].data;
		}
		return 0;
	case KB2_DEVICE_PORT_OP_IRQ_RELEASE:
		return -irq->release(irq->context, v[0], v[1]);
	case KB2_DEVICE_PORT_OP_IRQ_MASK:
		return -irq->mask(irq->context, v[0], v[1], v[2]);
	case KB2_DEVICE_PORT_OP_IRQ_ACK:
		return -irq->ack(irq->context, v[0], v[1]);
	case KB2_DEVICE_PORT_OP_IRQ_QUIESCE:
		return -irq->quiesce(irq->context, v[0], v[1]);
	case KB2_DEVICE_PORT_OP_IRQ_ACTIVE:
		result = -irq->active(irq->context, v[0], v[1], &active);
		if (!result) {
			reply->count = 1;
			reply->values[0] = active;
		}
		return result;
	case KB2_DEVICE_PORT_OP_IRQ_AFFINITY:
		return -irq->affinity(irq->context, v[0], v[1], v[2]);
	case KB2_DEVICE_PORT_OP_IRQ_RETRIGGER:
		return -irq->retrigger(irq->context, v[0], v[1]);
	case KB2_DEVICE_PORT_OP_IRQ_NEXT:
		result = -irq->next(irq->context, v[0], &event);
		if (!result) {
			reply->count = 2;
			reply->values[0] = event.hwirq;
			reply->values[1] = event.cookie;
		}
		return result;
	default:
		return EOPNOTSUPP;
	}
}

static int owner_drain(struct owner *owner)
{
	unsigned int i;
	int result, first = 0;

	/* The caller has reaped the sandbox. No Linux cleanup is needed here.
	 * Every hardware process must be stopped before RAM truncation can
	 * release a single page, including RAM shared with the second consumer.
	 */
	for (i = 0; i < OBJECTS; i++) {
		struct owned_device *device = &owner->devices[i];
		bool terminated = false;

		if (!device->hardware)
			continue;
		result = kobox_qemu_pci_revoke(device->hardware);
		if (result) {
			result = kobox_qemu_pci_terminate(device->hardware);
			terminated = !result;
			if (terminated && owner->failure)
				owner->failure->hardware_terminated++;
		}
		if (result) {
			if (!first)
				first = result;
			continue;
		}
		if (i == 1 && owner->failure && owner->failure->dma_pending && !terminated) {
			uint64_t value;

			/* Let the queued real DMA timer expire after VT-d drain but
			 * before hardware termination or CPU backing invalidation.
			 */
			result = kobox_qemu_pci_clock_step(device->hardware, 2000000000);
			if (!result)
				result = kobox_qemu_pci_ram_read(device->hardware, owner->dma_offset, &value);
			if (!result && value != owner->dma_before)
				result = EIO;
			if (!result)
				owner->failure->dma_blocked = true;
			else if (!first)
				first = result;
		}
		while (device->leases) {
			result = owner_unmap(device, device->leases->token, device->leases->length);
			if (result)
				break;
		}
		if (!result)
			result = kobox_qemu_pci_close(device->hardware);
		if (result) {
			if (!first)
				first = result;
		} else {
			device->hardware = NULL;
		}
	}
	/* Even on failure, every device's notification producer was joined.
	 * Never return with another producer using this owner's stack context.
	 */
	if (first)
		return first;
	if (owner->failure)
		owner->failure->retired_irq = owner->devices[0].first_irq;
	if (owner->ram.initialized) {
		result = kobox_posix_memory_backing_revoke(&owner->ram);
		if (result)
			return result;
		return kobox_posix_memory_backing_destroy(&owner->ram);
	}
	return 0;
}

static int kill_checkpoint(struct owner *owner, int pidfd)
{
	struct kobox_qemu_remote_failure *failure = owner->failure;

	if (!failure || !owner->ram.initialized || !owner->devices[0].hardware)
		return EPROTO;
	failure->checkpoint = true;
	failure->backing = fcntl(owner->ram.descriptor, F_DUPFD_CLOEXEC, 0);
	failure->length = owner->ram.size;
	failure->shared_alias = mmap(NULL, owner->ram.size, PROT_READ,
		MAP_SHARED, owner->ram.descriptor, 0);
	failure->private_alias = mmap(NULL, owner->ram.size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE, owner->ram.descriptor, 0);
	if (failure->backing < 0 || failure->shared_alias == MAP_FAILED ||
	    failure->private_alias == MAP_FAILED)
		return errno;
	/* These real aliases belong to the surviving owner process.
	 * Materialize a COW page without changing the sandbox's RAM.
	 */
	*(volatile unsigned char *)failure->private_alias =
		*(volatile unsigned char *)failure->shared_alias;
	if (syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, NULL, 0))
		return errno;
	failure->killed = true;
	return 0;
}

static int serve(struct owner *owner, pid_t child, int pidfd)
{
	struct pollfd events[] = {{owner->request, POLLIN, 0}, {pidfd, POLLIN, 0},
		{owner->checkpoint, POLLIN, 0}};
	int result = 0, status = 0, waited;

	for (;;) {
		struct kobox_device_packet request, reply;
		int descriptor = -1, operation_result;

		do {
			waited = poll(events, 3, -1);
		} while (waited < 0 && errno == EINTR);
		if (waited < 0) {
			result = errno;
			break;
		}
		if (events[1].revents)
			break;
		if (events[2].revents) {
			char checkpoint;

			if (!owner->failure || owner->dma_write ||
			    read(owner->checkpoint, &checkpoint, 1) != 1 || checkpoint != 'K') {
				result = EPROTO;
				break;
			}
			result = kill_checkpoint(owner, pidfd);
			break;
		}
		result = kobox_device_receive(owner->request, &request, &descriptor);
		if (result)
			break;
		reply = (struct kobox_device_packet) {
			.generation = request.generation, .object = request.object,
			.sequence = request.sequence, .operation = request.operation,
		};
		operation_result = dispatch(owner, &request, &reply, &descriptor);
		if (descriptor >= 0)
			close(descriptor);
		if (!operation_result && owner->failure && owner->failure->dma_pending) {
			fprintf(stderr, "Generation DMA: native consumer write=%u pending, RAM offset=%llu\n",
				owner->dma_writes, (unsigned long long)owner->dma_offset);
			/* The real native ioctl is still inside its MMIO RPC. No
			 * response or Linux cleanup can precede this forced death.
			 */
			result = kill_checkpoint(owner, pidfd);
			break;
		}
		reply.status = kobox_device_status(operation_result);
		if (operation_result) {
			reply.count = 0;
			memset(reply.values, 0, sizeof(reply.values));
		}
		/* A peer that stops receiving cannot block death observation. */
		result = kobox_device_send(owner->request, &reply, -1, MSG_DONTWAIT);
		if (result)
			break;
	}
	/* A socket closure alone is not process-exit proof. */
	events[1].revents = 0;
	do {
		waited = poll(&events[1], 1, result ? 5000 : -1);
	} while (waited < 0 && errno == EINTR);
	if (waited <= 0) {
		if (kill(child, SIGKILL) && errno != ESRCH)
			return errno;
		if (!result)
			result = waited < 0 ? errno : ETIMEDOUT;
	}
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != child)
		return ECHILD;
	if (result == EPIPE && WIFEXITED(status))
		result = 0;
	if (owner->failure) {
		owner->failure->reaped = true;
		if (!owner->failure->killed || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
			result = ECHILD;
	} else if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		result = ECHILD;
	}
	{
		int drained;

		if (owner->before_revoke)
			owner->before_revoke();
		drained = owner_drain(owner);

		fprintf(stderr, "Remote hardware owner: generation=%llu requests=%llu child-reaped=1 revoked=%u\n",
			(unsigned long long)owner->session.generation, (unsigned long long)owner->requests, !drained);
		if (drained)
			result = drained;
		if (owner->failure)
			owner->failure->revoked = !drained;
	}
	return result;
}

int kobox_qemu_remote_run(const char *executable, uint64_t generation,
			  bool consumer, bool virgl,
			  int (*entry)(void *, struct kobox_qemu_remote *), void *context,
			  struct kobox_qemu_remote_failure *failure,
			  const struct kobox_qemu_remote_trial *trial)
{
	struct owner owner = {.executable = executable, .session = {.generation = generation},
		.consumer = consumer, .virgl = virgl, .checkpoint = -1, .failure = failure};
	int requests[2], events[2], pidfd, result, status;
	int checkpoint[2] = {-1, -1};
	unsigned int i;
	pid_t child;

	if (!executable || !generation || !entry)
		return EINVAL;
	if (trial && ((trial->dma_write && (!consumer || !failure)) ||
	    (trial->before_revoke && !failure)))
		return EINVAL;
	owner.dma_write = trial ? trial->dma_write : 0;
	owner.before_revoke = trial ? trial->before_revoke : NULL;
	if (failure)
		*failure = (struct kobox_qemu_remote_failure) {.backing = -1, .request = -1, .event = -1};
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, requests))
		return errno;
	if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, events)) {
		result = errno;
		close(requests[0]);
		close(requests[1]);
		return result;
	}
	if (failure && pipe2(checkpoint, O_CLOEXEC)) {
		result = errno;
		goto close_all;
	}
	/* Replay captured hardware notifications through the real channel.
	 * Queue them before boot so scheduling cannot hide a stale delivery.
	 */
	if (trial && trial->count) {
		if (!trial->events || trial->count > 2) {
			result = EINVAL;
			goto close_all;
		}
		for (i = 0; i < trial->count; i++) {
			const struct kobox_device_packet *event = &trial->events[i];

			if (!event->generation || event->generation >= generation || event->object != 1 ||
			    event->operation != KB2_DEVICE_PORT_OP_IRQ_EVENT || event->status ||
			    event->sequence || event->count != 1 || event->values[0] >= 2) {
				result = EINVAL;
				goto close_all;
			}
			result = kobox_device_send(events[0], event, -1, MSG_DONTWAIT);
			if (result)
				goto close_all;
		}
	}
	child = fork();
	if (child < 0) {
		result = errno;
		goto close_all;
	}
	if (!child) {
		struct kobox_qemu_remote remote = {
			.checkpoint = checkpoint[1],
			.expected_retired = trial ? trial->count : 0,
		};

		close(requests[0]);
		close(events[0]);
		if (checkpoint[0] >= 0)
			close(checkpoint[0]);
		if (kobox_posix_device_proxy_create(requests[1], events[1], generation,
			OBJECTS, 2, &remote.proxy))
			_exit(1);
		result = entry(context, &remote);
		if (kobox_posix_device_proxy_destroy(&remote.proxy))
			result = 1;
		fflush(NULL);
		_exit(result ? 1 : 0);
	}
	if (failure) {
		failure->request = fcntl(requests[1], F_DUPFD_CLOEXEC, 0);
		failure->event = fcntl(events[1], F_DUPFD_CLOEXEC, 0);
		if (failure->request < 0 || failure->event < 0) {
			result = errno;
			if (kill(child, SIGKILL) && errno != ESRCH)
				abort();
			while (waitpid(child, &status, 0) < 0 && errno == EINTR)
				;
			goto close_all;
		}
	}
	if (checkpoint[1] >= 0) {
		close(checkpoint[1]);
		checkpoint[1] = -1;
	}
	close(requests[1]);
	close(events[1]);
	requests[1] = events[1] = -1;
	pidfd = syscall(SYS_pidfd_open, child, 0);
	if (pidfd < 0) {
		result = errno;
		if (kill(child, SIGKILL) && errno != ESRCH)
			abort();
		while (waitpid(child, &status, 0) < 0 && errno == EINTR)
			;
		goto close_all;
	}
	owner.request = requests[0];
	owner.event = events[0];
	owner.checkpoint = checkpoint[0];
	for (i = 0; i < OBJECTS; i++) {
		owner.devices[i].owner = &owner;
		owner.devices[i].object = i + 1;
	}
	result = serve(&owner, child, pidfd);
	close(pidfd);
close_all:
	for (i = 0; i < 2; i++) {
		if (requests[i] >= 0)
			close(requests[i]);
		if (events[i] >= 0)
			close(events[i]);
		if (checkpoint[i] >= 0)
			close(checkpoint[i]);
	}
	return result;
}

_Noreturn void kobox_qemu_remote_checkpoint(struct kobox_qemu_remote *remote)
{
	char checkpoint = 'K';
	ssize_t result;

	if (!remote || remote->checkpoint < 0)
		_exit(1);
	do {
		result = write(remote->checkpoint, &checkpoint, 1);
	} while (result < 0 && errno == EINTR);
	if (result != 1)
		_exit(1);
	/* Freeze only this test observer. No guest cleanup may race ahead of
	 * the external owner's SIGKILL and manufacture a clean shutdown.
	 */
	for (;;)
		pause();
}
