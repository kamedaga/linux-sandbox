// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../../boot/exec_gate.h"
#include "../../arch/x86_64/user_layout.h"
#include <drm/virtgpu_drm.h>
#include <linux/virtio_gpu.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#ifdef KOBOX_DRM_SYNC
int kobox_sync_client_test(int render);
#endif
#ifdef KOBOX_DRM_SYNC_REMOVE
int kobox_sync_remove_test(void);
#endif

static long call(unsigned long number, unsigned long a0, unsigned long a1,
		 unsigned long a2, unsigned long a3, unsigned long a4,
		 unsigned long a5)
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
		.pid = call(SYS_getpid, 0, 0, 0, 0, 0, 0),
		.line = line, .error = error,
	};

	call(SYS_pwrite64, 19, (uintptr_t)&failure, sizeof(failure),
	     KOBOX_EXEC_FAILURE_OFFSET, 0, 0);
	return 1;
}

#define CHECK(condition) do { \
	if (!(condition)) return fail(__LINE__, 0); \
} while (0)
#define SUCCESS(expression) do { \
	long error = (expression); \
	if (error) return fail(__LINE__, error); \
} while (0)
#define PASS(expression) do { if (expression) return 1; } while (0)

static long ioctl_call(int fd, unsigned long request, void *argument)
{
	return call(SYS_ioctl, fd, request, (uintptr_t)argument, 0, 0, 0);
}

static int version(int fd)
{
	char name[32] = {0};
	struct drm_version version = {.name = name, .name_len = sizeof(name)};

	SUCCESS(ioctl_call(fd, DRM_IOCTL_VERSION, &version));
	CHECK(version.name_len == 10 && name[0] == 'v' && name[6] == '_');
	version.name = (void *)1;
	CHECK(ioctl_call(fd, DRM_IOCTL_VERSION, &version) == -EFAULT);
	return 0;
}

static int empty_events(int fd)
{
	struct pollfd event = {.fd = fd, .events = POLLIN};
	char bytes[128];

	SUCCESS(call(SYS_poll, (uintptr_t)&event, 1, 0, 0, 0, 0));
	CHECK(!event.revents);
	CHECK(call(SYS_read, fd, (uintptr_t)bytes, sizeof(bytes), 0, 0, 0) == -EAGAIN);
	return 0;
}

static int kms_events(int fd)
{
	uint32_t crtcs[16], connectors[16];
	struct drm_mode_card_res resources = {
		.crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = 16,
		.connector_id_ptr = (uintptr_t)connectors, .count_connectors = 16,
	};
	struct drm_mode_modeinfo modes[32];
	struct drm_mode_get_connector connector = {0};
	struct drm_mode_create_dumb create = {.bpp = 32};
	struct drm_mode_map_dumb map = {0};
	struct drm_mode_destroy_dumb destroy;
	volatile uint32_t *pixels;
	struct drm_mode_fb_cmd fb = {.bpp = 32, .depth = 24};
	struct drm_mode_crtc crtc = {0};
	struct drm_mode_crtc_page_flip flip = {
		.flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = 0x4729136a,
	};
	struct drm_event_vblank event = {0};
	struct pollfd poll = {.fd = fd, .events = POLLIN};

	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources));
	CHECK(resources.count_crtcs && resources.count_crtcs <= 16 &&
	      resources.count_connectors && resources.count_connectors <= 16);
	connector.connector_id = connectors[0];
	/* A zero mode count requests upstream connector probing. */
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector));
	CHECK(connector.count_modes && connector.count_modes <= 32);
	connector.modes_ptr = (uintptr_t)modes;
	connector.count_modes = 32;
	connector.count_props = 0;
	connector.count_encoders = 0;
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector));
	CHECK(connector.count_modes && modes[0].hdisplay && modes[0].vdisplay);
	create.width = modes[0].hdisplay;
	create.height = modes[0].vdisplay;
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create));
	map.handle = create.handle;
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_MAP_DUMB, &map));
	pixels = (void *)call(SYS_mmap, 0, create.size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, fd, map.offset);
	CHECK((uintptr_t)pixels >= KOBOX_X86_USER_START &&
	      (uintptr_t)pixels < KOBOX_X86_USER_END);
	pixels[0] = 0x0013579b;
	fb.width = create.width;
	fb.height = create.height;
	fb.pitch = create.pitch;
	fb.handle = create.handle;
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_ADDFB, &fb));
	crtc.crtc_id = crtcs[0];
	crtc.fb_id = fb.fb_id;
	crtc.set_connectors_ptr = (uintptr_t)connectors;
	crtc.count_connectors = 1;
	crtc.mode_valid = 1;
	crtc.mode = modes[0];
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_SETCRTC, &crtc));
	flip.crtc_id = crtc.crtc_id;
	flip.fb_id = fb.fb_id;
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip));
	CHECK(call(SYS_poll, (uintptr_t)&poll, 1, 5000, 0, 0, 0) == 1);
	CHECK(poll.revents == POLLIN);
	CHECK(call(SYS_read, fd, (uintptr_t)&event, sizeof(event), 0, 0, 0) == sizeof(event));
	CHECK(event.base.type == DRM_EVENT_FLIP_COMPLETE &&
	      event.base.length == sizeof(event) && event.user_data == flip.user_data);
	PASS(empty_events(fd));
	crtc.fb_id = 0;
	crtc.mode_valid = 0;
	crtc.count_connectors = 0;
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_SETCRTC, &crtc));
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id));
	CHECK(pixels[0] == 0x0013579b);
	SUCCESS(call(SYS_munmap, (uintptr_t)pixels, create.size, 0, 0, 0, 0));
	destroy.handle = create.handle;
	SUCCESS(ioctl_call(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy));
	return 0;
}

static int peer(volatile uint32_t *mapping, uint32_t handle, unsigned int cpu)
{
	struct drm_mode_map_dumb map = {.handle = handle};
	unsigned long mask = 1UL << (cpu ^ 1);
	long primary, render;

	SUCCESS(call(SYS_sched_setaffinity, 0, sizeof(mask), (uintptr_t)&mask, 0, 0, 0));
	SUCCESS(call(SYS_setresuid, 1000, 1000, 1000, 0, 0, 0));
	primary = call(SYS_openat, AT_FDCWD, (uintptr_t)"/card0",
		       O_RDWR | O_NONBLOCK, 0, 0, 0);
	render = call(SYS_openat, AT_FDCWD, (uintptr_t)"/renderD128",
		      O_RDWR | O_NONBLOCK, 0, 0, 0);
	CHECK(primary >= 0 && render >= 0);
	PASS(version(primary));
	PASS(version(render));
	CHECK(ioctl_call(primary, DRM_IOCTL_SET_MASTER, NULL) == -EACCES);
	CHECK(ioctl_call(render, DRM_IOCTL_SET_MASTER, NULL) == -EACCES);
	CHECK(ioctl_call(primary, DRM_IOCTL_MODE_MAP_DUMB, &map) == -ENOENT);
	CHECK(mapping[0] == 0x6a291347);
	mapping[1] = 0x4731296a;
	PASS(empty_events(render));
	SUCCESS(call(SYS_close, primary, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, render, 0, 0, 0, 0, 0));
	return 0;
}

int kobox_drm_client_test(unsigned long *stack)
{
	struct drm_virtgpu_resource_create create = {
		.target = 2, .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
		.width = 64, .height = 64, .depth = 1, .array_size = 1,
		.size = 16384, .stride = 256,
	};
	struct drm_virtgpu_map map = {0};
	struct drm_virtgpu_3d_wait wait = {0};
	struct drm_gem_close close = {0};
	struct kobox_exec_result record = {.phase = 2};
	volatile uint32_t *first, *alias;
	char **argv = (void *)(stack + 1);
	unsigned int cpu;
	long primary, render, child;
	int status = -1;

	CHECK(stack[0] == 3);
	cpu = argv[2][0] - '0';
	CHECK(cpu < 2);
	primary = call(SYS_openat, AT_FDCWD, (uintptr_t)"/card0",
		       O_RDWR | O_NONBLOCK, 0, 0, 0);
	render = call(SYS_openat, AT_FDCWD, (uintptr_t)"/renderD128",
		      O_RDWR | O_NONBLOCK, 0, 0, 0);
	CHECK(primary >= 0 && render >= 0);
	PASS(version(primary));
	PASS(version(render));
	PASS(empty_events(primary));
	PASS(empty_events(render));
	PASS(kms_events(primary));
#ifdef KOBOX_DRM_SYNC
	PASS(kobox_sync_client_test(render));
#endif
	SUCCESS(ioctl_call(render, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &create));
	CHECK(create.bo_handle && create.res_handle);
	wait.handle = create.bo_handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_VIRTGPU_WAIT, &wait));
	map.handle = create.bo_handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_VIRTGPU_MAP, &map));
	first = (void *)call(SYS_mmap, 0, create.size, PROT_READ | PROT_WRITE,
			    MAP_SHARED, render, map.offset);
	alias = (void *)call(SYS_mmap, 0, create.size, PROT_READ | PROT_WRITE,
			    MAP_SHARED, render, map.offset);
	CHECK((uintptr_t)first >= KOBOX_X86_USER_START &&
	      (uintptr_t)first < KOBOX_X86_USER_END &&
	      (uintptr_t)alias >= KOBOX_X86_USER_START &&
	      (uintptr_t)alias < KOBOX_X86_USER_END && first != alias);
	first[0] = 0x6a291347;
	CHECK(alias[0] == first[0]);
	child = call(SYS_fork, 0, 0, 0, 0, 0, 0);
	CHECK(child >= 0);
	if (!child)
		return peer(alias, create.bo_handle, cpu);
	CHECK(call(SYS_wait4, child, (uintptr_t)&status, 0, 0, 0, 0) == child);
	/* Preserve a failing peer's more specific record at FD 19. */
	if (status)
		return 1;
	CHECK(first[1] == 0x4731296a);
	close.handle = create.bo_handle;
	SUCCESS(ioctl_call(render, DRM_IOCTL_GEM_CLOSE, &close));
	CHECK(ioctl_call(render, DRM_IOCTL_VIRTGPU_MAP, &map) == -ENOENT);
	SUCCESS(call(SYS_close, primary, 0, 0, 0, 0, 0));
	SUCCESS(call(SYS_close, render, 0, 0, 0, 0, 0));
	alias[2] = 0x719af062;
	CHECK(first[2] == alias[2]);
	SUCCESS(call(SYS_munmap, (uintptr_t)first, create.size, 0, 0, 0, 0));
	CHECK(alias[0] == 0x6a291347);
	SUCCESS(call(SYS_munmap, (uintptr_t)alias, create.size, 0, 0, 0, 0));
#ifdef KOBOX_DRM_SYNC_REMOVE
	PASS(kobox_sync_remove_test());
#endif
	record.pid = call(SYS_getpid, 0, 0, 0, 0, 0, 0);
	record.cpu = cpu;
	CHECK(call(SYS_pwrite64, 17, (uintptr_t)&record, sizeof(record), 0, 0, 0) == sizeof(record));
	return 0;
}
