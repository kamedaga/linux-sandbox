// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../../boot/exec_gate.h"
#ifdef KOBOX_MESA_DRAW
#include "mesa_draw_test.h"
#endif
#ifdef KOBOX_MESA_KMS
#include "mesa_kms_test.h"
#endif

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <gbm.h>
#include <xf86drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(unsigned int line)
{
	struct kobox_exec_failure failure = {
		.pid = getpid(), .line = line, .error = errno,
	};

	dprintf(2, "Mesa client failure: line=%u errno=%d EGL=%#x\n",
		line, errno, eglGetError());
	return pwrite(19, &failure, sizeof(failure), KOBOX_EXEC_FAILURE_OFFSET) ==
		sizeof(failure) ? 1 : 2;
}

#define CHECK(condition) do { if (!(condition)) return fail(__LINE__); } while (0)

#define GL_FUNCTIONS(X) \
	X(GetString, PFNGLGETSTRINGPROC) \
	X(GenTextures, PFNGLGENTEXTURESPROC) \
	X(BindTexture, PFNGLBINDTEXTUREPROC) \
	X(TexImage2D, PFNGLTEXIMAGE2DPROC) \
	X(GenFramebuffers, PFNGLGENFRAMEBUFFERSPROC) \
	X(BindFramebuffer, PFNGLBINDFRAMEBUFFERPROC) \
	X(FramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC) \
	X(CheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC) \
	X(ClearColor, PFNGLCLEARCOLORPROC) \
	X(Clear, PFNGLCLEARPROC) \
	X(Finish, PFNGLFINISHPROC) \
	X(ReadPixels, PFNGLREADPIXELSPROC) \
	X(GetError, PFNGLGETERRORPROC) \
	X(DeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC) \
	X(DeleteTextures, PFNGLDELETETEXTURESPROC)

struct gl_api {
#define DECLARE(name, type) type name;
	GL_FUNCTIONS(DECLARE)
#undef DECLARE
};

int main(int argc, char **argv)
{
#ifndef KOBOX_MESA_KMS
	const EGLint attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE,
	};
#endif
	const EGLint context_attributes[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE,
	};
	struct gl_api gl;
	struct kobox_exec_result result = {.pid = getpid(), .phase = 2};
	struct gbm_device *device;
	drmDevicePtr pci;
	drmVersionPtr version;
	EGLDisplay display;
	EGLContext context;
	EGLConfig config;
	EGLint major, minor;
#ifndef KOBOX_MESA_KMS
	EGLint count;
#endif
	const char *renderer;
#if !defined(KOBOX_MESA_DRAW) && !defined(KOBOX_MESA_KMS)
	GLuint texture, framebuffer;
	unsigned char pixel[4] = {0};
#endif
	int fd, draw_result = 0;

	CHECK(argc == 3 && (!strcmp(argv[2], "0") || !strcmp(argv[2], "1")));
	result.cpu = argv[2][0] - '0';
	dprintf(2, "Mesa client: libc/loader entered on CPU %llu\n",
		(unsigned long long)result.cpu);
#ifdef KOBOX_MESA_KMS
	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
#else
	fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
#endif
	CHECK(fd >= 0);
	version = drmGetVersion(fd);
	CHECK(version && !strcmp(version->name, "virtio_gpu"));
	drmFreeVersion(version);
	CHECK(!drmGetDevice2(fd, 0, &pci));
	CHECK(pci->bustype == DRM_BUS_PCI &&
	      pci->deviceinfo.pci->vendor_id == 0x1af4 &&
	      pci->deviceinfo.pci->device_id == 0x1050);
	drmFreeDevice(&pci);
	dprintf(2, "Mesa client: upstream PCI/DRM sysfs discovery passed\n");
	device = gbm_create_device(fd);
	CHECK(device);
	display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, device, NULL);
	CHECK(display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor));
	CHECK(eglBindAPI(EGL_OPENGL_API));
#ifdef KOBOX_MESA_KMS
	config = kobox_mesa_kms_config(display);
	CHECK(config);
#else
	CHECK(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
#endif
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
	CHECK(context != EGL_NO_CONTEXT);
	CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
#define LOAD(name, type) gl.name = (type)eglGetProcAddress("gl" #name); CHECK(gl.name);
	GL_FUNCTIONS(LOAD)
#undef LOAD
	renderer = (const char *)gl.GetString(GL_RENDERER);
	dprintf(2, "Mesa GL renderer=%s version=%s EGL=%d.%d\n",
		renderer ? renderer : "(null)", gl.GetString(GL_VERSION), major, minor);
	CHECK(renderer && strstr(renderer, "virgl (") && !strcasestr(renderer, "llvmpipe") &&
	      !strcasestr(renderer, "softpipe") && !strcasestr(renderer, "software"));
#ifdef KOBOX_MESA_KMS
	draw_result = kobox_mesa_kms_test(fd, device, display, context, result.cpu);
#elif defined(KOBOX_MESA_DRAW)
	draw_result = kobox_mesa_draw_test();
#else
	/* Force real resource/context creation and submission, not lazy EGL setup. */
	gl.GenTextures(1, &texture);
	gl.BindTexture(GL_TEXTURE_2D, texture);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	gl.GenFramebuffers(1, &framebuffer);
	gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			      GL_TEXTURE_2D, texture, 0);
	CHECK(gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	gl.ClearColor(1, 0, 0, 1);
	gl.Clear(GL_COLOR_BUFFER_BIT);
	gl.Finish();
	gl.ReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	CHECK(gl.GetError() == GL_NO_ERROR);
	CHECK(pixel[0] == 255 && !pixel[1] && !pixel[2] && pixel[3] == 255);
	gl.DeleteFramebuffers(1, &framebuffer);
	gl.DeleteTextures(1, &texture);
#endif
	CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
	CHECK(eglDestroyContext(display, context) && eglTerminate(display));
	eglReleaseThread();
	gbm_device_destroy(device);
	CHECK(!close(fd));
	if (draw_result) {
		dprintf(2, "Mesa failed draw: GL resources and context released\n");
		return draw_result;
	}
	dprintf(2, "Mesa context and backend round-trip passed\n");
	CHECK(pwrite(19, &result, sizeof(result), 0) == sizeof(result));
	return 0;
}
