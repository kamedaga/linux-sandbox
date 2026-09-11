// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../../boot/exec_gate.h"
#include "../../arch/x86_64/user_layout.h"
#include <drm/virtgpu_drm.h>
#include <linux/virtio_gpu.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#ifdef KOBOX_PRIME_CONSUMER
#include "../../gem/dma_consumer_test.h"
#endif

#define BUFFER_SIZE 16384
#define WORDS (BUFFER_SIZE / sizeof(uint32_t))

static long call(unsigned long number, unsigned long a0, unsigned long a1,
		 unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
	register unsigned long r10 __asm__("r10") = a3;
	register unsigned long r8 __asm__("r8") = a4;
	register unsigned long r9 __asm__("r9") = a5;

	__asm__ volatile("syscall" : "+a"(number) :
		"D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9) :
		"rcx", "r11", "memory");
	return number;
}

static int fail(unsigned int line, long error)
{
	struct kobox_exec_failure previous;
	struct kobox_exec_failure failure = {
		.pid = call(SYS_getpid, 0, 0, 0, 0, 0, 0), .line = line, .error = error,
	};

	/* A peer records its failure before exiting/closing the handshake.
	 * Do not replace that cause with the parent's ensuing EOF assertion.
	 */
	if (call(SYS_pread64, 19, (uintptr_t)&previous, sizeof(previous),
		 KOBOX_EXEC_FAILURE_OFFSET, 0, 0) == sizeof(previous) && previous.line)
		return 1;
	call(SYS_pwrite64, 19, (uintptr_t)&failure, sizeof(failure),
	     KOBOX_EXEC_FAILURE_OFFSET, 0, 0);
	return 1;
}

#define CHECK(condition) do { if (!(condition)) return fail(__LINE__, 0); } while (0)
#define SUCCESS(expression) do { \
	long error = (expression); \
	if (error) return fail(__LINE__, error); \
} while (0)
#define PASS(expression) do { if (expression) return 1; } while (0)

static long ioctl_call(int fd, unsigned long request, void *argument)
{
	return call(SYS_ioctl, fd, request, (uintptr_t)argument, 0, 0, 0);
}

static int poll_buffer(int fd, short events, int ready)
{
	struct pollfd poll = {.fd = fd, .events = events};

	CHECK(call(SYS_poll, (uintptr_t)&poll, 1, 0, 0, 0, 0) == !!ready);
	CHECK(poll.revents == (ready ? events : 0));
	return 0;
}

static int wait_buffer(int fd, short events)
{
	struct pollfd poll = {.fd = fd, .events = events};

	CHECK(call(SYS_poll, (uintptr_t)&poll, 1, 5000, 0, 0, 0) == 1);
	CHECK(poll.revents == events);
	return 0;
}

static int cpu_access(int fd, uint64_t flags)
{
	struct dma_buf_sync sync = {.flags = flags};
	long result;

	do {
		result = ioctl_call(fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (result == -EINTR || result == -EAGAIN);
	SUCCESS(result);
	return 0;
}

static volatile uint32_t *map_buffer(int fd, uint64_t offset)
{
	return (void *)call(SYS_mmap, 0, BUFFER_SIZE, PROT_READ | PROT_WRITE,
			    MAP_SHARED, fd, offset);
}

static int valid_mapping(const volatile uint32_t *mapping)
{
	return (uintptr_t)mapping >= KOBOX_X86_USER_START &&
	       (uintptr_t)mapping < KOBOX_X86_USER_END;
}

static int create_buffer(int render, struct drm_prime_handle *prime)
{
	struct drm_virtgpu_resource_create resource = {
		.target = 2, .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
		.width = 64, .height = 64, .depth = 1, .array_size = 1,
		.size = BUFFER_SIZE, .stride = 256,
	};

	SUCCESS(ioctl_call(render, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &resource));
	CHECK(resource.bo_handle && resource.res_handle);
	prime->handle = resource.bo_handle;
	prime->flags = DRM_CLOEXEC | DRM_RDWR;
	SUCCESS(ioctl_call(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, prime));
	CHECK(prime->fd >= 0);
	CHECK(call(SYS_fcntl, prime->fd, F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC);
	return 0;
}

static int close_buffer(int render, const struct drm_prime_handle *prime)
{
	struct drm_gem_close close = {.handle = prime->handle};

	SUCCESS(ioctl_call(render, DRM_IOCTL_GEM_CLOSE, &close));
	SUCCESS(call(SYS_close, prime->fd, 0, 0, 0, 0, 0));
	return 0;
}

static int pending_fence(int render, struct drm_prime_handle *source,
			 struct dma_buf_export_sync_file *export)
{
	struct sync_fence_info fence = {0};
	struct sync_file_info info = {
		.num_fences = 1, .sync_fence_info = (uintptr_t)&fence,
	};
	const char *driver = "virtio_gpu";
	unsigned int index;

	PASS(create_buffer(render, source));
	export->flags = DMA_BUF_SYNC_READ;
	SUCCESS(ioctl_call(source->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, export));
	SUCCESS(ioctl_call(export->fd, SYNC_IOC_FILE_INFO, &info));
	CHECK(info.num_fences == 1 && info.status == 0 && fence.status == 0);
	for (index = 0; ; index++) {
		CHECK(fence.driver_name[index] == driver[index]);
		if (!driver[index])
			break;
	}
	return 0;
}

/* Import a real, pending driver fence as a reader or writer. The two poll
 * classes and exported snapshots must follow dma_resv's usage ordering.
 */
static int reservation_sync(int render, int buffer, uint32_t usage)
{
	struct drm_prime_handle source = {0};
	struct dma_buf_export_sync_file export = {0};
	struct dma_buf_export_sync_file read = {.flags = DMA_BUF_SYNC_READ};
	struct dma_buf_export_sync_file write = {.flags = DMA_BUF_SYNC_WRITE};
	struct dma_buf_import_sync_file import = {.flags = usage};

	PASS(pending_fence(render, &source, &export));
	import.fd = export.fd;
	SUCCESS(ioctl_call(buffer, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import));
	PASS(poll_buffer(buffer, POLLIN, usage == DMA_BUF_SYNC_READ));
	PASS(poll_buffer(buffer, POLLOUT, 0));
	SUCCESS(ioctl_call(buffer, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &read));
	SUCCESS(ioctl_call(buffer, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &write));
	PASS(poll_buffer(read.fd, POLLIN, usage == DMA_BUF_SYNC_READ));
	PASS(poll_buffer(write.fd, POLLIN, 0));
	PASS(wait_buffer(buffer, POLLOUT));
	PASS(poll_buffer(buffer, POLLIN | POLLOUT, 1));
	PASS(poll_buffer(read.fd, POLLIN, 1));
	PASS(poll_buffer(write.fd, POLLIN, 1));
	SUCCESS(call(SYS_close, export.fd, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, read.fd, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, write.fd, 0, 0, 0, 0, 0));
	PASS(close_buffer(render, &source));
	return 0;
}

static int transfer_fd(int socket, int *fd, int receive)
{
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control = {0};
	char byte = 'b';
	struct iovec vector = {.iov_base = &byte, .iov_len = 1};
	struct msghdr message = {
		.msg_iov = &vector, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
	};
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);

	if (!receive) {
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(int));
		*(int *)CMSG_DATA(header) = *fd;
		CHECK(call(SYS_sendmsg, socket, (uintptr_t)&message, 0, 0, 0, 0) == 1);
		return 0;
	}
	CHECK(call(SYS_recvmsg, socket, (uintptr_t)&message,
		   MSG_CMSG_CLOEXEC, 0, 0, 0) == 1);
	CHECK(byte == 'b' && !(message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)));
	header = CMSG_FIRSTHDR(&message);
	CHECK(header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
	CHECK(header->cmsg_len == CMSG_LEN(sizeof(int)) && !CMSG_NXTHDR(&message, header));
	*fd = *(int *)CMSG_DATA(header);
	CHECK(*fd >= 0 && call(SYS_fcntl, *fd, F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC);
	return 0;
}

static int handshake(int socket, char expected, int receive)
{
	char byte = expected;

	CHECK(call(receive ? SYS_read : SYS_write, socket,
		   (uintptr_t)&byte, 1, 0, 0, 0) == 1);
	CHECK(byte == expected);
	return 0;
}

static uint32_t pattern(unsigned int index)
{
	return 0x639a12bfU ^ (index * 7919U);
}

#ifdef KOBOX_PRIME_CONSUMER
static int consumer_attach(int device, int buffer)
{
	struct kobox_dma_consumer_attach request = {.fd = -1};
	struct kobox_dma_consumer_attach *readonly;

	CHECK(ioctl_call(device, KOBOX_DMA_CONSUMER_TRANSFER, NULL) == -ENXIO);
	CHECK(ioctl_call(device, KOBOX_DMA_CONSUMER_ATTACH, &request) == -EBADF);
	readonly = (void *)call(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(valid_mapping((void *)readonly));
	readonly->fd = buffer;
	readonly->segments = 0;
	SUCCESS(call(SYS_mprotect, (uintptr_t)readonly, 4096, PROT_READ, 0, 0, 0));
	/* Failing copyout occurs after attach/map, and must roll both back. */
	CHECK(ioctl_call(device, KOBOX_DMA_CONSUMER_ATTACH, readonly) == -EFAULT);
	SUCCESS(call(SYS_munmap, (uintptr_t)readonly, 4096, 0, 0, 0, 0));
	request.fd = buffer;
	SUCCESS(ioctl_call(device, KOBOX_DMA_CONSUMER_ATTACH, &request));
	if (!request.segments || request.segments > 4)
		return fail(__LINE__, request.segments);
	CHECK(ioctl_call(device, KOBOX_DMA_CONSUMER_ATTACH, &request) == -EBUSY);
	return 0;
}

static int consumer_transfer(int device, int render, int buffer,
			     volatile uint32_t *shared, volatile uint32_t *gem)
{
	struct drm_prime_handle source = {0};
	struct dma_buf_export_sync_file export = {0};
	struct dma_buf_import_sync_file import = {.flags = DMA_BUF_SYNC_WRITE};
	unsigned int index;

	PASS(pending_fence(render, &source, &export));
	import.fd = export.fd;
	SUCCESS(ioctl_call(buffer, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import));
	PASS(poll_buffer(buffer, POLLIN | POLLOUT, 0));
	/* The consumer's upstream begin_cpu_access must wait for this actual
	 * GPU completion before inspecting the pages and starting EDU DMA.
	 */
	SUCCESS(ioctl_call(device, KOBOX_DMA_CONSUMER_TRANSFER, NULL));
	PASS(poll_buffer(export.fd, POLLIN, 1));
	PASS(poll_buffer(buffer, POLLIN | POLLOUT, 1));
	SUCCESS(call(SYS_close, export.fd, 0, 0, 0, 0, 0));
	PASS(close_buffer(render, &source));
	PASS(cpu_access(buffer, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ));
	for (index = 0; index < WORDS; index++) {
		unsigned int in_page = index % 1024;
		unsigned int source = in_page >= 32 && in_page < 48 ? index - 32 : index;

		CHECK(shared[index] == pattern(source) + 1 && gem[index] == shared[index]);
	}
	PASS(cpu_access(buffer, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ));
	return 0;
}
#endif

static int peer(int socket, unsigned int cpu)
{
	struct drm_prime_handle import = {0}, repeat = {0}, export = {0};
	struct drm_virtgpu_map map = {0};
	struct drm_gem_close close = {0};
	volatile uint32_t *shared, *gem;
	unsigned long mask = 1UL << (cpu ^ 1);
	unsigned int index;
	int buffer = -1;
	long render;
#ifdef KOBOX_PRIME_CONSUMER
	long consumer;
#endif

	SUCCESS(call(SYS_sched_setaffinity, 0, sizeof(mask), (uintptr_t)&mask, 0, 0, 0));
	PASS(transfer_fd(socket, &buffer, 1));
#ifdef KOBOX_PRIME_CONSUMER
	consumer = call(SYS_openat, AT_FDCWD, (uintptr_t)"/dma-consumer", O_RDWR, 0, 0, 0);
	CHECK(consumer >= 0);
	PASS(consumer_attach(consumer, buffer));
#endif
	render = call(SYS_openat, AT_FDCWD, (uintptr_t)"/renderD128", O_RDWR, 0, 0, 0);
	CHECK(render >= 0);
	import.fd = buffer;
	SUCCESS(ioctl_call(render, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import));
	repeat.fd = buffer;
	SUCCESS(ioctl_call(render, DRM_IOCTL_PRIME_FD_TO_HANDLE, &repeat));
	CHECK(import.handle && repeat.handle == import.handle);
	map.handle = import.handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_VIRTGPU_MAP, &map));
	shared = map_buffer(buffer, 0);
	gem = map_buffer(render, map.offset);
	CHECK(valid_mapping(shared) && valid_mapping(gem) && shared != gem);
	PASS(cpu_access(buffer, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW));
	for (index = 0; index < WORDS; index++) {
		CHECK(shared[index] == pattern(index) && gem[index] == shared[index]);
		gem[index] = ~pattern(index);
		CHECK(shared[index] == gem[index]);
	}
	PASS(cpu_access(buffer, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW));
	PASS(handshake(socket, 'm', 0));
	PASS(handshake(socket, 'c', 1));
	/* No inherited exporter FD or VMA exists: fork preceded allocation.
	 * The exporter has now closed its handle, dma-buf FD, DRM file and VMA.
	 */
	export.handle = import.handle;
	export.flags = DRM_CLOEXEC | DRM_RDWR;
	SUCCESS(ioctl_call(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export));
	CHECK(export.fd >= 0 && export.fd != buffer);
	PASS(cpu_access(export.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW));
	for (index = 0; index < WORDS; index++) {
		CHECK(gem[index] == ~pattern(index));
		shared[index] = pattern(index) + 1;
		CHECK(gem[index] == shared[index]);
	}
	PASS(cpu_access(export.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW));
#ifdef KOBOX_PRIME_CONSUMER
	PASS(consumer_transfer(consumer, render, buffer, shared, gem));
	SUCCESS(ioctl_call(consumer, KOBOX_DMA_CONSUMER_DETACH, NULL));
	CHECK(ioctl_call(consumer, KOBOX_DMA_CONSUMER_TRANSFER, NULL) == -ENXIO);
	/* Closing the file, as opposed to the detach ioctl, must also drain
	 * the attachment and all IOVA/page references.
	 */
	{
		long transient = call(SYS_openat, AT_FDCWD, (uintptr_t)"/dma-consumer",
				      O_RDWR, 0, 0, 0);

		CHECK(transient >= 0);
		PASS(consumer_attach(transient, buffer));
		SUCCESS(call(SYS_close, transient, 0, 0, 0, 0, 0));
	}
	PASS(cpu_access(buffer, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE));
	for (index = 0; index < WORDS; index++)
		shared[index] = pattern(index) + 1;
	PASS(cpu_access(buffer, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE));
#endif
	close.handle = import.handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_GEM_CLOSE, &close));
	CHECK(ioctl_call(render, DRM_IOCTL_GEM_CLOSE, &close) == -EINVAL);
	SUCCESS(call(SYS_close, render, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, buffer, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, export.fd, 0, 0, 0, 0, 0));
	/* Only the two VMAs retain the object. No device access remains. */
	for (index = 0; index < WORDS; index++)
		CHECK(shared[index] == pattern(index) + 1 && gem[index] == shared[index]);
	SUCCESS(call(SYS_munmap, (uintptr_t)gem, BUFFER_SIZE, 0, 0, 0, 0));
	CHECK(shared[WORDS - 1] == pattern(WORDS - 1) + 1);
	SUCCESS(call(SYS_munmap, (uintptr_t)shared, BUFFER_SIZE, 0, 0, 0, 0));
#ifdef KOBOX_PRIME_CONSUMER
	{
		struct kobox_dma_consumer_reclaim reclaim = {0};
		long error = ioctl_call(consumer, KOBOX_DMA_CONSUMER_RECLAIM, &reclaim);

		if (error)
			return fail(__LINE__, ((uint64_t)reclaim.references[0] << 32) |
				    ((uint64_t)reclaim.mappings[0] << 16) | reclaim.flags[0]);
		CHECK(reclaim.objects == 1 && reclaim.pages == 15);
	}
	SUCCESS(call(SYS_close, consumer, 0, 0, 0, 0, 0));
#endif
	SUCCESS(call(SYS_close, socket, 0, 0, 0, 0, 0));
	return 0;
}

int kobox_drm_client_test(unsigned long *stack)
{
	struct kobox_exec_result record = {.phase = 2};
	struct drm_prime_handle prime = {0};
	struct dma_buf_sync invalid = {0};
	volatile uint32_t *mapping;
	char **argv = (void *)(stack + 1);
	unsigned int cpu, index;
	int sockets[2], status = -1;
	long child, render;

	CHECK(stack[0] == 3);
	cpu = argv[2][0] - '0';
	CHECK(cpu < 2);
	SUCCESS(call(SYS_socketpair, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC,
		     0, (uintptr_t)sockets, 0, 0));
	child = call(SYS_fork, 0, 0, 0, 0, 0, 0);
	CHECK(child >= 0);
	if (!child) {
		SUCCESS(call(SYS_close, sockets[0], 0, 0, 0, 0, 0));
		return peer(sockets[1], cpu);
	}
	SUCCESS(call(SYS_close, sockets[1], 0, 0, 0, 0, 0));
	render = call(SYS_openat, AT_FDCWD, (uintptr_t)"/renderD128", O_RDWR, 0, 0, 0);
	CHECK(render >= 0);
	PASS(create_buffer(render, &prime));
	PASS(wait_buffer(prime.fd, POLLOUT));
	PASS(reservation_sync(render, prime.fd, DMA_BUF_SYNC_READ));
	PASS(reservation_sync(render, prime.fd, DMA_BUF_SYNC_WRITE));
	CHECK(ioctl_call(prime.fd, DMA_BUF_IOCTL_SYNC, &invalid) == -EINVAL);
	invalid.flags = DMA_BUF_SYNC_RW | (1ULL << 63);
	CHECK(ioctl_call(prime.fd, DMA_BUF_IOCTL_SYNC, &invalid) == -EINVAL);
	mapping = map_buffer(prime.fd, 0);
	CHECK(valid_mapping(mapping));
	CHECK(call(SYS_mmap, 0, BUFFER_SIZE + 4096, PROT_READ,
		   MAP_SHARED, prime.fd, 0) == -EINVAL);
	PASS(cpu_access(prime.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE));
	for (index = 0; index < WORDS; index++)
		mapping[index] = pattern(index);
	PASS(cpu_access(prime.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE));
	PASS(transfer_fd(sockets[0], &prime.fd, 0));
	PASS(handshake(sockets[0], 'm', 1));
	PASS(cpu_access(prime.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ));
	for (index = 0; index < WORDS; index++)
		CHECK(mapping[index] == ~pattern(index));
	PASS(cpu_access(prime.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ));
	PASS(close_buffer(render, &prime));
	SUCCESS(call(SYS_close, render, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_munmap, (uintptr_t)mapping, BUFFER_SIZE, 0, 0, 0, 0));
	PASS(handshake(sockets[0], 'c', 0));
	CHECK(call(SYS_wait4, child, (uintptr_t)&status, 0, 0, 0, 0) == child);
	if (status)
		return 1;
	SUCCESS(call(SYS_close, sockets[0], 0, 0, 0, 0, 0));
	record.pid = call(SYS_getpid, 0, 0, 0, 0, 0, 0);
	record.cpu = cpu;
	CHECK(call(SYS_pwrite64, 17, (uintptr_t)&record, sizeof(record), 0, 0, 0) == sizeof(record));
	return 0;
}
