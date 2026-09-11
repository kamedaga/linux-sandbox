// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../../boot/exec_gate.h"
#include <drm/virtgpu_drm.h>
#include <linux/virtio_gpu.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

long kobox_sync_interrupt_wait(int fd, unsigned long request, void *argument);

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
	struct kobox_exec_failure failure = {
		.pid = call(SYS_getpid, 0, 0, 0, 0, 0, 0), .line = line, .error = error,
	};

	call(SYS_pwrite64, 19, (uintptr_t)&failure, sizeof(failure),
	     KOBOX_EXEC_FAILURE_OFFSET, 0, 0);
	return 1;
}

#define CHECK(condition) do { if (!(condition)) return fail(__LINE__, 0); } while (0)
#define SUCCESS(expression) do { \
	long error = (expression); \
	if (error) return fail(__LINE__, error); \
} while (0)

static long ioctl_call(int fd, unsigned long request, void *argument)
{
	return call(SYS_ioctl, fd, request, (uintptr_t)argument, 0, 0, 0);
}

static int64_t now(void)
{
	struct timespec time;

	if (call(SYS_clock_gettime, CLOCK_MONOTONIC, (uintptr_t)&time, 0, 0, 0, 0))
		return -1;
	return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static int fence_info(int fd, int expected)
{
	struct sync_fence_info fence = {0};
	struct sync_file_info info = {
		.num_fences = 1, .sync_fence_info = (uintptr_t)&fence,
	};
	const char *driver = expected ? "detached-driver" : "virtio_gpu";
	unsigned int index;

	SUCCESS(ioctl_call(fd, SYNC_IOC_FILE_INFO, &info));
	if (info.num_fences != 1 || info.status != expected || fence.status != expected)
		return fail(__LINE__, ((uint64_t)(uint32_t)expected << 32) |
				     (uint32_t)info.status);
	/* Prove the pending fence's driver identity. Upstream deliberately
	 * hides driver-owned names once completion permits driver teardown.
	 */
	for (index = 0; ; index++) {
		CHECK(fence.driver_name[index] == driver[index]);
		if (!driver[index])
			break;
	}
	return 0;
}

static int transfer_fds(int socket, int files[2], int receive)
{
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(2 * sizeof(int))];
	} control = {0};
	char byte = 'f';
	struct iovec vector = {.iov_base = &byte, .iov_len = 1};
	struct msghdr message = {
		.msg_iov = &vector, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
	};
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);
	int *rights = (void *)CMSG_DATA(header);

	if (!receive) {
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(2 * sizeof(int));
		rights[0] = files[0];
		rights[1] = files[1];
		CHECK(call(SYS_sendmsg, socket, (uintptr_t)&message, 0, 0, 0, 0) == 1);
		return 0;
	}
	CHECK(call(SYS_recvmsg, socket, (uintptr_t)&message,
		   MSG_CMSG_CLOEXEC, 0, 0, 0) == 1);
	CHECK(byte == 'f' && !(message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)));
	header = CMSG_FIRSTHDR(&message);
	CHECK(header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
	CHECK(header->cmsg_len == CMSG_LEN(2 * sizeof(int)) && !CMSG_NXTHDR(&message, header));
	rights = (void *)CMSG_DATA(header);
	files[0] = rights[0];
	files[1] = rights[1];
	CHECK(files[0] >= 0 && files[1] >= 0 && files[0] != files[1]);
	CHECK(call(SYS_fcntl, files[0], F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC);
	CHECK(call(SYS_fcntl, files[1], F_GETFD, 0, 0, 0, 0) == FD_CLOEXEC);
	return 0;
}

static int shared_fences(int socket, int render)
{
	struct drm_syncobj_handle imported = {0};
	struct drm_syncobj_timeline_wait wait = {.count_handles = 1};
	struct drm_syncobj_destroy destroy = {0};
	uint64_t point = 7;
	int files[2];
	char byte = 'r';

	if (transfer_fds(socket, files, 1))
		return 1;
	if (fence_info(files[0], 0))
		return 1;
	imported.fd = files[1];
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imported));
	wait.handles = (uintptr_t)&imported.handle;
	wait.points = (uintptr_t)&point;
	wait.timeout_nsec = now() + 5000000000LL;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait));
	if (fence_info(files[0], 1))
		return 1;
	CHECK(call(SYS_write, socket, (uintptr_t)&byte, 1, 0, 0, 0) == 1);
	CHECK(call(SYS_read, socket, (uintptr_t)&byte, 1, 0, 0, 0) == 1 && byte == 'd');
	/* The exporter reset the shared object and dropped all its references.
	 * The opaque FD observes that reset; the sync_file keeps its snapshot.
	 */
	CHECK(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait) == -EINVAL);
	if (fence_info(files[0], 1))
		return 1;
	destroy.handle = imported.handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy));
	SUCCESS(call(SYS_close, files[0], 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, files[1], 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, render, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, socket, 0, 0, 0, 0, 0));
	return 0;
}

static int advance_timeline(int render, uint32_t binary, uint32_t timeline, int previous)
{
	struct drm_virtgpu_resource_create resource = {
		.target = 2, .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
		.width = 64, .height = 64, .depth = 1, .array_size = 1, .size = 16384,
	};
	struct drm_prime_handle prime = {.flags = DRM_CLOEXEC | DRM_RDWR};
	struct dma_buf_export_sync_file export = {.flags = DMA_BUF_SYNC_READ};
	struct drm_syncobj_handle import = {
		.handle = binary, .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,
	};
	struct drm_syncobj_transfer transfer = {
		.src_handle = binary, .dst_handle = timeline, .dst_point = 11,
	};
	struct drm_syncobj_timeline_wait wait = {.count_handles = 1};
	struct drm_syncobj_timeline_array query = {.count_handles = 1};
	struct drm_gem_close close = {0};
	struct pollfd poll;
	uint64_t point = 7, observed = 0;

	SUCCESS(ioctl_call(render, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &resource));
	prime.handle = resource.bo_handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime));
	SUCCESS(ioctl_call(prime.fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export));
	if (fence_info(export.fd, 0))
		return 1;
	import.fd = export.fd;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import));
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer));
	wait.handles = (uintptr_t)&timeline;
	wait.points = (uintptr_t)&point;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait));
	point = 11;
	CHECK(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait) == -ETIME);
	query.handles = (uintptr_t)&timeline;
	query.points = (uintptr_t)&observed;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_QUERY, &query));
	CHECK(observed == 7);
	query.flags = DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_QUERY, &query));
	CHECK(observed == 11);
	if (fence_info(previous, 1))
		return 1;
	poll = (struct pollfd) {.fd = export.fd, .events = POLLIN};
	CHECK(call(SYS_poll, (uintptr_t)&poll, 1, 5000, 0, 0, 0) == 1);
	CHECK(poll.revents == POLLIN);
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait));
	query.flags = 0;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_QUERY, &query));
	CHECK(observed == 11);
	if (fence_info(export.fd, 1) || fence_info(previous, 1))
		return 1;
	close.handle = resource.bo_handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_GEM_CLOSE, &close));
	SUCCESS(call(SYS_close, prime.fd, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, export.fd, 0, 0, 0, 0, 0));
	return 0;
}

int kobox_sync_client_test(int render)
{
	struct drm_virtgpu_resource_create resource = {
		.target = 2, .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
		.width = 64, .height = 64, .depth = 1, .array_size = 1, .size = 16384,
	};
	struct drm_prime_handle prime = {.flags = DRM_CLOEXEC | DRM_RDWR};
	struct dma_buf_export_sync_file export = {.flags = DMA_BUF_SYNC_READ};
	struct drm_syncobj_create binary = {0}, timeline = {0};
	struct drm_syncobj_handle import = {0}, snapshot = {0}, opaque = {0};
	struct drm_syncobj_transfer transfer = {.dst_point = 7};
	struct drm_syncobj_wait wait = {.count_handles = 1};
	struct drm_syncobj_timeline_wait timeline_wait = {.count_handles = 1};
	struct drm_syncobj_destroy destroy = {0};
	struct drm_syncobj_array reset = {.count_handles = 1};
	struct drm_gem_close close;
	struct pollfd poll;
	uint64_t point = 7;
	int64_t start;
	int sockets[2], files[2], status = -1;
	long child;
	char byte;

	SUCCESS(ioctl_call(render, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &resource));
	prime.handle = resource.bo_handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime));
	SUCCESS(ioctl_call(prime.fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export));
	if (fence_info(export.fd, 0))
		return 1;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_CREATE, &binary));
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_CREATE, &timeline));
	import.handle = binary.handle;
	import.fd = export.fd;
	import.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import));
	transfer.src_handle = binary.handle;
	transfer.dst_handle = timeline.handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer));
	snapshot.handle = timeline.handle;
	snapshot.point = point;
	snapshot.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE |
			 DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &snapshot));
	if (fence_info(snapshot.fd, 0))
		return 1;
	opaque.handle = timeline.handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &opaque));
	SUCCESS(call(SYS_socketpair, AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0,
		     (uintptr_t)sockets, 0, 0));
	child = call(SYS_fork, 0, 0, 0, 0, 0, 0);
	CHECK(child >= 0);
	if (!child) {
		int result;
		long peer;

		/* Remove every inherited fence FD and the exporter's DRM file.
		 * Only SCM_RIGHTS may give this independent DRM client access.
		 */
		SUCCESS(call(SYS_close, sockets[0], 0, 0, 0, 0, 0));
		SUCCESS(call(SYS_close, render, 0, 0, 0, 0, 0));
		SUCCESS(call(SYS_close, prime.fd, 0, 0, 0, 0, 0));
		SUCCESS(call(SYS_close, export.fd, 0, 0, 0, 0, 0));
		SUCCESS(call(SYS_close, snapshot.fd, 0, 0, 0, 0, 0));
		SUCCESS(call(SYS_close, opaque.fd, 0, 0, 0, 0, 0));
		peer = call(SYS_openat, AT_FDCWD, (uintptr_t)"/renderD128",
			    O_RDWR | O_CLOEXEC, 0, 0, 0);
		CHECK(peer >= 0);
		result = shared_fences(sockets[1], peer);
		call(SYS_exit_group, result, 0, 0, 0, 0, 0);
		__builtin_trap();
	}
	SUCCESS(call(SYS_close, sockets[1], 0, 0, 0, 0, 0));
	files[0] = snapshot.fd;
	files[1] = opaque.fd;
	if (transfer_fds(sockets[0], files, 0))
		return 1;
	poll = (struct pollfd) {.fd = export.fd, .events = POLLIN};
	SUCCESS(call(SYS_poll, (uintptr_t)&poll, 1, 0, 0, 0, 0));
	start = now();
	CHECK(start > 0);
	wait.handles = (uintptr_t)&binary.handle;
	wait.timeout_nsec = start + 10000000;
	CHECK(ioctl_call(render, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == -ETIME);
	CHECK(now() >= wait.timeout_nsec);
	timeline_wait.handles = (uintptr_t)&timeline.handle;
	timeline_wait.points = (uintptr_t)&point;
	timeline_wait.timeout_nsec = now() + 10000000;
	CHECK(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &timeline_wait) == -ETIME);
	CHECK(now() >= timeline_wait.timeout_nsec);
	wait.timeout_nsec = now() + 5000000000LL;
	SUCCESS(kobox_sync_interrupt_wait(render, DRM_IOCTL_SYNCOBJ_WAIT, &wait));
	timeline_wait.timeout_nsec = now() + 5000000000LL;
	SUCCESS(kobox_sync_interrupt_wait(render, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT,
					&timeline_wait));
	CHECK(call(SYS_poll, (uintptr_t)&poll, 1, 5000, 0, 0, 0) == 1);
	CHECK(poll.revents == POLLIN);
	if (fence_info(export.fd, 1) || fence_info(snapshot.fd, 1))
		return 1;
	/* Completion before either wait: an expired deadline must still succeed. */
	wait.timeout_nsec = 0;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_WAIT, &wait));
	timeline_wait.timeout_nsec = 0;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &timeline_wait));
	CHECK(call(SYS_read, sockets[0], (uintptr_t)&byte, 1, 0, 0, 0) == 1 && byte == 'r');
	if (advance_timeline(render, binary.handle, timeline.handle, snapshot.fd))
		return 1;
	reset.handles = (uintptr_t)&timeline.handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_RESET, &reset));
	destroy.handle = binary.handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy));
	destroy.handle = timeline.handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy));
	close.handle = resource.bo_handle;
	close.pad = 0;
	SUCCESS(ioctl_call(render, DRM_IOCTL_GEM_CLOSE, &close));
	SUCCESS(call(SYS_close, prime.fd, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, export.fd, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, snapshot.fd, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, opaque.fd, 0, 0, 0, 0, 0));
	byte = 'd';
	CHECK(call(SYS_write, sockets[0], (uintptr_t)&byte, 1, 0, 0, 0) == 1);
	CHECK(call(SYS_wait4, child, (uintptr_t)&status, 0, 0, 0, 0) == child);
	if (status == 256)
		return 1; /* Keep the child's exact failure record. */
	if (status)
		return fail(__LINE__, status);
	SUCCESS(call(SYS_close, sockets[0], 0, 0, 0, 0, 0));
	return 0;
}

#ifdef KOBOX_DRM_SYNC_REMOVE
static int phase_at(unsigned int offset, uint32_t phase, int write)
{
	int64_t deadline = now() + 10000000000LL;
	const struct timespec interval = {.tv_nsec = 1000000};
	uint32_t observed;
	long size;

	if (write) {
		CHECK(call(SYS_pwrite64, 19, (uintptr_t)&phase, sizeof(phase),
			   offset, 0, 0) == sizeof(phase));
		return 0;
	}
	do {
		size = call(SYS_pread64, 19, (uintptr_t)&observed, sizeof(observed),
			    offset, 0, 0);
		CHECK(size >= 0);
		if (size == sizeof(observed) && observed == phase)
			return 0;
		SUCCESS(call(SYS_nanosleep, (uintptr_t)&interval, 0, 0, 0, 0, 0));
	} while (now() < deadline);
	return fail(__LINE__, -ETIMEDOUT);
}

static int device_phase(uint32_t phase, int write)
{
	return phase_at(KOBOX_EXEC_DEVICE_OFFSET, phase, write);
}

static int removal_waiter(int render, uint32_t handle, int timeline, int sync_file)
{
	struct drm_syncobj_wait binary_wait = {.count_handles = 1};
	struct drm_syncobj_timeline_wait timeline_wait = {.count_handles = 1};
	uint64_t point = 7;

	if (phase_at(KOBOX_EXEC_DEVICE_OFFSET + 4 * (timeline + 1), 1, 1))
		return 1;
	if (timeline) {
		timeline_wait.handles = (uintptr_t)&handle;
		timeline_wait.points = (uintptr_t)&point;
		timeline_wait.timeout_nsec = now() + 5000000000LL;
		SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &timeline_wait));
	} else {
		binary_wait.handles = (uintptr_t)&handle;
		binary_wait.timeout_nsec = now() + 5000000000LL;
		SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_WAIT, &binary_wait));
	}
	/* Upstream syncobj WAIT reports completion, not the fence's error.
	 * A zero result also proves the ioctl entered before DRM unplugged:
	 * an ioctl that only enters after unplug must return ENODEV instead.
	 */
	return fence_info(sync_file, -ENODEV);
}

#ifdef KOBOX_DRM_SYNC_REVOKE
static int fault_waiter(void)
{
	const struct timespec interval = {.tv_nsec = 1000000};
	int64_t deadline = now() + 10000000000LL;
	uint32_t phase;
	long size;

	if (phase_at(KOBOX_EXEC_DEVICE_OFFSET + 12, 1, 1))
		return 1;
	do {
		size = call(SYS_pread64, 19, (uintptr_t)&phase, sizeof(phase),
			KOBOX_EXEC_DEVICE_OFFSET, 0, 0);
		CHECK(size == sizeof(phase));
		if (phase == 7 || phase == 2) {
			/* A genuine instruction load, not a direct fault-handler call.
			 * Phase 2 also exercises the surviving VMA after normal unplug.
			 */
			(void)*(volatile unsigned char *)KOBOX_EXEC_REVOKE_ADDRESS;
			return 0;
		}
		SUCCESS(call(SYS_nanosleep, (uintptr_t)&interval, 0, 0, 0, 0, 0));
	} while (now() < deadline);
	return fail(__LINE__, -ETIMEDOUT);
}
#endif

static int completed_resource(int render)
{
	struct drm_virtgpu_resource_create resource = {
		.target = 2, .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
		.width = 64, .height = 64, .depth = 1, .array_size = 1, .size = 16384,
	};
	struct drm_prime_handle prime = {.flags = DRM_CLOEXEC | DRM_RDWR};
	struct dma_buf_export_sync_file export = {.flags = DMA_BUF_SYNC_READ};
	struct drm_gem_close release = {0};
	struct pollfd poll;

	/* Establish a genuine completion IRQ before the later IRQ mask.
	 * The owner retains that actual notification for generation replay.
	 */
	SUCCESS(ioctl_call(render, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &resource));
	prime.handle = resource.bo_handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime));
	SUCCESS(ioctl_call(prime.fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export));
	poll = (struct pollfd) {.fd = export.fd, .events = POLLIN};
	CHECK(call(SYS_poll, (uintptr_t)&poll, 1, 5000, 0, 0, 0) == 1);
	CHECK(poll.revents == POLLIN);
	if (fence_info(export.fd, 1))
		return 1;
	SUCCESS(call(SYS_close, export.fd, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, prime.fd, 0, 0, 0, 0, 0));
	release.handle = resource.bo_handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_GEM_CLOSE, &release));
	return 0;
}

int kobox_sync_remove_test(void)
{
	struct drm_virtgpu_resource_create resource = {
		.target = 2, .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
		.width = 64, .height = 64, .depth = 1, .array_size = 1, .size = 16384,
	};
	struct drm_prime_handle prime = {.flags = DRM_CLOEXEC | DRM_RDWR};
	struct dma_buf_export_sync_file export = {.flags = DMA_BUF_SYNC_READ};
	struct drm_syncobj_create binary = {0}, timeline = {0};
	struct drm_syncobj_handle import = {0}, snapshot = {0};
	struct drm_syncobj_transfer transfer = {.dst_point = 7};
	struct pollfd poll;
	struct drm_syncobj_wait wait = {.count_handles = 1};
	const struct timespec delay = {.tv_nsec = 20000000};
	long render, fresh, children[3];
	int count = 2;
	int index, status;

	render = call(SYS_openat, AT_FDCWD, (uintptr_t)"/renderD128", O_RDWR, 0, 0, 0);
	CHECK(render >= 0);
	if (completed_resource(render))
		return 1;
	if (device_phase(5, 1) || device_phase(6, 0))
		return 1;
	SUCCESS(ioctl_call(render, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &resource));
	prime.handle = resource.bo_handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime));
	SUCCESS(ioctl_call(prime.fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export));
	if (fence_info(export.fd, 0))
		return 1;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_CREATE, &binary));
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_CREATE, &timeline));
	import.handle = binary.handle;
	import.fd = export.fd;
	import.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import));
	transfer.src_handle = binary.handle;
	transfer.dst_handle = timeline.handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer));
	snapshot.handle = timeline.handle;
	snapshot.point = 7;
	snapshot.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE |
			 DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE;
	SUCCESS(ioctl_call(render, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &snapshot));
	if (fence_info(snapshot.fd, 0))
		return 1;
#ifdef KOBOX_DRM_SYNC_REVOKE
	CHECK(call(SYS_mmap, KOBOX_EXEC_REVOKE_ADDRESS, resource.size,
		PROT_READ, MAP_SHARED | MAP_FIXED_NOREPLACE, prime.fd, 0) == KOBOX_EXEC_REVOKE_ADDRESS);
	count++;
#endif
	for (index = 0; index < count; index++) {
		children[index] = call(SYS_fork, 0, 0, 0, 0, 0, 0);
		CHECK(children[index] >= 0);
		if (!children[index]) {
			int result;

#ifdef KOBOX_DRM_SYNC_REVOKE
			if (index == 2)
				result = fault_waiter();
			else
#endif
				result = removal_waiter(render, index ? timeline.handle : binary.handle,
							index, export.fd);

			call(SYS_exit_group, result, 0, 0, 0, 0, 0);
			__builtin_trap();
		}
	}
	for (index = 0; index < count; index++)
		if (phase_at(KOBOX_EXEC_DEVICE_OFFSET + 4 * (index + 1), 1, 0))
			return 1;
	SUCCESS(call(SYS_nanosleep, (uintptr_t)&delay, 0, 0, 0, 0, 0));
	if (device_phase(1, 1))
		return 1;
	poll = (struct pollfd) {.fd = export.fd, .events = POLLIN};
	CHECK(call(SYS_poll, (uintptr_t)&poll, 1, 5000, 0, 0, 0) == 1);
	CHECK(poll.revents == POLLIN);
	if (fence_info(export.fd, -ENODEV) || fence_info(snapshot.fd, -ENODEV) ||
	    device_phase(2, 0))
		return 1;
	for (index = 0; index < count; index++) {
		status = -1;
		CHECK(call(SYS_wait4, children[index], (uintptr_t)&status, 0, 0, 0, 0) ==
		      children[index]);
		if (status == 256)
			return 1;
		if (status)
			return fail(__LINE__, status);
	}
#ifdef KOBOX_DRM_SYNC_REVOKE
	SUCCESS(call(SYS_munmap, KOBOX_EXEC_REVOKE_ADDRESS, resource.size, 0, 0, 0, 0));
#endif
	/* DRM rejects new ioctls after unplug. Existing anonymous sync_file
	 * descriptors must still expose the driver's terminal error.
	 */
	wait.handles = (uintptr_t)&binary.handle;
	CHECK(ioctl_call(render, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == -ENODEV);
	fresh = call(SYS_openat, AT_FDCWD, (uintptr_t)"/renderD128", O_RDWR, 0, 0, 0);
	CHECK(fresh >= 0 && fresh != render);
	if (completed_resource(fresh))
		return 1;
	/* The replacement can complete real work while the retained old FD
	 * and its exported fences remain terminal, even under the same path.
	 */
	CHECK(ioctl_call(render, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == -ENODEV);
	if (fence_info(export.fd, -ENODEV) || fence_info(snapshot.fd, -ENODEV))
		return 1;
	SUCCESS(call(SYS_close, fresh, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, render, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, prime.fd, 0, 0, 0, 0, 0));
	if (fence_info(export.fd, -ENODEV) || fence_info(snapshot.fd, -ENODEV))
		return 1;
	SUCCESS(call(SYS_close, export.fd, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, snapshot.fd, 0, 0, 0, 0, 0));
	if (device_phase(3, 1) || device_phase(4, 0))
		return 1;
	return 0;
}
#endif /* KOBOX_DRM_SYNC_REMOVE */
