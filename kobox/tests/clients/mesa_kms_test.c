// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "mesa_kms_test.h"
#include "../../boot/exec_gate.h"

#include <GL/glcorearb.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { FRAMES = 16, MODES = 2 };

struct kms;

struct flip_cookie {
	struct kms *kms;
	unsigned int token;
};

#define KMS_FUNCTIONS(X) \
	X(CreateShader, PFNGLCREATESHADERPROC) \
	X(ShaderSource, PFNGLSHADERSOURCEPROC) \
	X(CompileShader, PFNGLCOMPILESHADERPROC) \
	X(GetShaderiv, PFNGLGETSHADERIVPROC) \
	X(DeleteShader, PFNGLDELETESHADERPROC) \
	X(CreateProgram, PFNGLCREATEPROGRAMPROC) \
	X(AttachShader, PFNGLATTACHSHADERPROC) \
	X(LinkProgram, PFNGLLINKPROGRAMPROC) \
	X(GetProgramiv, PFNGLGETPROGRAMIVPROC) \
	X(DeleteProgram, PFNGLDELETEPROGRAMPROC) \
	X(UseProgram, PFNGLUSEPROGRAMPROC) \
	X(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC) \
	X(Uniform1i, PFNGLUNIFORM1IPROC) \
	X(GenVertexArrays, PFNGLGENVERTEXARRAYSPROC) \
	X(BindVertexArray, PFNGLBINDVERTEXARRAYPROC) \
	X(DeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC) \
	X(Viewport, PFNGLVIEWPORTPROC) \
	X(Disable, PFNGLDISABLEPROC) \
	X(DrawArrays, PFNGLDRAWARRAYSPROC) \
	X(GetError, PFNGLGETERRORPROC)

static struct {
#define DECLARE(name, type) type name;
	KMS_FUNCTIONS(DECLARE)
#undef DECLARE
} gl;

struct kms {
	int fd;
	uint32_t connector, crtc, plane;
	drmModeModeInfo modes[MODES];
	unsigned int events, frame, cpu;
	unsigned int token;
	struct flip_cookie cookies[MODES * FRAMES];
	bool pending, bad_event;
	uint64_t last_time;
	GLuint program, vao;
	GLint code, height;
};

static int failure(unsigned int line)
{
	struct kobox_exec_failure failure = {
		.pid = getpid(), .line = line, .error = errno,
	};

	dprintf(2, "Mesa KMS failure: line=%u errno=%d EGL=%#x GL=%#x\n",
		line, errno, eglGetError(), gl.GetError ? gl.GetError() : 0);
	return pwrite(19, &failure, sizeof(failure), KOBOX_EXEC_FAILURE_OFFSET) ==
		sizeof(failure) ? 1 : 2;
}

#define REQUIRE(condition) do { if (!(condition)) return failure(__LINE__); } while (0)

static int discover(struct kms *kms)
{
	drmModeRes *resources = drmModeGetResources(kms->fd);
	drmModeConnector *connector = NULL;
	drmModePlaneRes *planes;
	int index, mode, crtc_index = -1;

	REQUIRE(resources);
	for (index = 0; index < resources->count_connectors; index++) {
		connector = drmModeGetConnector(kms->fd, resources->connectors[index]);
		if (connector && connector->connection == DRM_MODE_CONNECTED &&
		    connector->count_modes)
			break;
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	if (!connector) {
		drmModeFreeResources(resources);
		return failure(__LINE__);
	}
	kms->connector = connector->connector_id;
	for (index = 0; index < connector->count_encoders && crtc_index < 0; index++) {
		drmModeEncoder *encoder = drmModeGetEncoder(kms->fd, connector->encoders[index]);

		if (!encoder)
			continue;
		for (int c = 0; c < resources->count_crtcs && c < 32; c++) {
			if (encoder->possible_crtcs & (1U << c)) {
				crtc_index = c;
				kms->crtc = resources->crtcs[c];
				break;
			}
		}
		drmModeFreeEncoder(encoder);
	}
	for (mode = 0; mode < MODES; mode++) {
		for (index = 0; index < connector->count_modes; index++) {
			drmModeModeInfo *info = &connector->modes[index];

			if (info->hdisplay == (mode ? 800 : 640) &&
			    info->vdisplay == (mode ? 600 : 480)) {
				kms->modes[mode] = *info;
				break;
			}
		}
	}
	drmModeFreeConnector(connector);
	drmModeFreeResources(resources);
	REQUIRE(crtc_index >= 0 && kms->modes[0].clock && kms->modes[1].clock);
	REQUIRE(!drmSetClientCap(kms->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1));
	planes = drmModeGetPlaneResources(kms->fd);
	REQUIRE(planes);
	for (uint32_t p = 0; p < planes->count_planes && !kms->plane; p++) {
		drmModePlane *plane = drmModeGetPlane(kms->fd, planes->planes[p]);
		drmModeObjectProperties *props;
		bool primary = false, format = false;

		if (!plane)
			continue;
		props = drmModeObjectGetProperties(kms->fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
		for (uint32_t f = 0; f < plane->count_formats; f++)
			format |= plane->formats[f] == GBM_FORMAT_XRGB8888;
		for (uint32_t i = 0; props && i < props->count_props; i++) {
			drmModePropertyPtr prop = drmModeGetProperty(kms->fd, props->props[i]);

			if (prop && !strcmp(prop->name, "type"))
				primary = props->prop_values[i] == DRM_PLANE_TYPE_PRIMARY;
			drmModeFreeProperty(prop);
		}
		if (primary && format && (plane->possible_crtcs & (1U << crtc_index)))
			kms->plane = plane->plane_id;
		drmModeFreeObjectProperties(props);
		drmModeFreePlane(plane);
	}
	drmModeFreePlaneResources(planes);
	REQUIRE(kms->plane);
	return 0;
}

static GLuint shader(GLenum type, const char *source)
{
	GLuint shader = gl.CreateShader(type);
	GLint compiled;

	gl.ShaderSource(shader, 1, &source, NULL);
	gl.CompileShader(shader);
	gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		gl.DeleteShader(shader);
		return 0;
	}
	return shader;
}

static int drawing_init(struct kms *kms)
{
	const char *vertex = "#version 330 core\n"
		"void main() { vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
		"gl_Position=vec4(p*2.0-1.0,0,1); }\n";
	const char *fragment = "#version 330 core\n"
		"uniform int code; uniform int height; out vec4 color;"
		"void main() { int x=int(gl_FragCoord.x);"
		"int y=height-1-int(gl_FragCoord.y);"
		"ivec3 c=ivec3(16+3*code,32+48*((x/32)%4),32+48*((y/32)%4));"
#ifdef KOBOX_MESA_KMS_BAD_SHADER
		"if (x>=64 && x<96 && y>=64 && y<96) c.b=255;"
#endif
		"color=vec4(vec3(c)/255.0,1.0); }\n";
	GLuint vs, fs;
	GLint linked;

#define LOAD(name, type) gl.name = (type)eglGetProcAddress("gl" #name); REQUIRE(gl.name);
	KMS_FUNCTIONS(LOAD)
#undef LOAD
	vs = shader(GL_VERTEX_SHADER, vertex);
	fs = shader(GL_FRAGMENT_SHADER, fragment);
	if (!vs || !fs) {
		gl.DeleteShader(vs);
		gl.DeleteShader(fs);
		return failure(__LINE__);
	}
	kms->program = gl.CreateProgram();
	gl.AttachShader(kms->program, vs);
	gl.AttachShader(kms->program, fs);
	gl.LinkProgram(kms->program);
	gl.DeleteShader(vs);
	gl.DeleteShader(fs);
	gl.GetProgramiv(kms->program, GL_LINK_STATUS, &linked);
	REQUIRE(linked);
	gl.UseProgram(kms->program);
	kms->code = gl.GetUniformLocation(kms->program, "code");
	kms->height = gl.GetUniformLocation(kms->program, "height");
	REQUIRE(kms->code >= 0 && kms->height >= 0);
	gl.GenVertexArrays(1, &kms->vao);
	gl.BindVertexArray(kms->vao);
	gl.Disable(GL_DITHER);
	gl.Disable(GL_FRAMEBUFFER_SRGB);
	return 0;
}

EGLConfig kobox_mesa_kms_config(EGLDisplay display)
{
	const EGLint attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE,
	};
	EGLConfig configs[128];
	EGLint count, visual;

	if (!eglChooseConfig(display, attributes, configs, 128, &count))
		return NULL;
	for (int i = 0; i < count; i++) {
		if (eglGetConfigAttrib(display, configs[i], EGL_NATIVE_VISUAL_ID, &visual) &&
		    (uint32_t)visual == GBM_FORMAT_XRGB8888)
			return configs[i];
	}
	return NULL;
}

static void flipped(int fd, unsigned int sequence, unsigned int seconds,
		    unsigned int microseconds, void *data)
{
	struct flip_cookie *cookie = data;
	struct kms *kms = cookie->kms;
	uint64_t time = (uint64_t)seconds * 1000000 + microseconds;

	(void)sequence; /* virtio-gpu has no hardware vblank counter. */
	if (fd != kms->fd || !kms->pending || cookie->token != kms->token ||
	    microseconds >= 1000000 || time < kms->last_time)
		kms->bad_event = true;
	kms->last_time = time;
	kms->pending = false;
	kms->events++;
}

static int wait_flip(struct kms *kms)
{
	drmEventContext context = {.version = 2, .page_flip_handler = flipped};
	struct pollfd pollfd = {.fd = kms->fd, .events = POLLIN};
	int ready;

	do {
		ready = poll(&pollfd, 1, 5000);
	} while (ready < 0 && errno == EINTR);
	REQUIRE(ready == 1 && pollfd.revents == POLLIN);
	REQUIRE(!drmHandleEvent(kms->fd, &context));
	REQUIRE(!kms->pending && !kms->bad_event);
	return 0;
}

static int check_crtc(struct kms *kms, uint32_t framebuffer, drmModeModeInfo *mode)
{
	drmModeCrtc *crtc = drmModeGetCrtc(kms->fd, kms->crtc);
	bool valid;

	REQUIRE(crtc);
	valid = crtc->buffer_id == framebuffer && crtc->mode_valid &&
		crtc->mode.hdisplay == mode->hdisplay && crtc->mode.vdisplay == mode->vdisplay;
	drmModeFreeCrtc(crtc);
	REQUIRE(valid);
	return 0;
}

static int framebuffer(struct kms *kms, struct gbm_bo *bo, uint32_t *fb)
{
	uint32_t handles[4] = {gbm_bo_get_handle(bo).u32};
	uint32_t strides[4] = {gbm_bo_get_stride(bo)};
	uint32_t offsets[4] = {gbm_bo_get_offset(bo, 0)};
	uint64_t modifier = gbm_bo_get_modifier(bo), modifiers[4] = {modifier};
	uint64_t cap;

	REQUIRE(gbm_bo_get_format(bo) == GBM_FORMAT_XRGB8888 &&
		gbm_bo_get_plane_count(bo) == 1 && handles[0] && strides[0]);
	REQUIRE(!drmGetCap(kms->fd, DRM_CAP_ADDFB2_MODIFIERS, &cap));
	/* This upstream virtio-gpu uses implicit scanout layout. Accept an
	 * implicitly allocated GBM BO reporting LINEAR too, but never discard a
	 * non-linear modifier. INVALID itself does not assert linear layout.
	 */
	if (modifier == DRM_FORMAT_MOD_INVALID || !cap) {
		REQUIRE(modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR);
		REQUIRE(!drmModeAddFB2(kms->fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
				GBM_FORMAT_XRGB8888, handles, strides, offsets, fb, 0));
	} else {
		REQUIRE(!drmModeAddFB2WithModifiers(kms->fd, gbm_bo_get_width(bo),
			gbm_bo_get_height(bo), GBM_FORMAT_XRGB8888, handles, strides,
			offsets, modifiers, fb, DRM_MODE_FB_MODIFIERS));
	}
	if (!kms->frame)
		dprintf(2, "Mesa KMS buffer: format=%#x modifier=%#llx stride=%u explicit=%llu\n",
				GBM_FORMAT_XRGB8888, (unsigned long long)modifier,
				strides[0], (unsigned long long)cap);
	return 0;
}

static int run_mode(struct kms *kms, struct gbm_device *device,
		    EGLDisplay display, EGLContext context, unsigned int mode)
{
	drmModeModeInfo *info = &kms->modes[mode];
	struct gbm_surface *surface;
	struct gbm_bo *current = NULL, *next = NULL;
	uint32_t current_fb = 0, next_fb = 0;
	EGLSurface window;
	EGLConfig config = kobox_mesa_kms_config(display);
	int result = 0;

	REQUIRE(config && gbm_device_is_format_supported(device, GBM_FORMAT_XRGB8888,
		GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING));
	surface = gbm_surface_create(device, info->hdisplay, info->vdisplay,
		GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	REQUIRE(surface);
	window = eglCreateWindowSurface(display, config, (EGLNativeWindowType)surface, NULL);
	if (window == EGL_NO_SURFACE || !eglMakeCurrent(display, window, window, context)) {
		result = failure(__LINE__);
		goto destroy;
	}
	for (kms->frame = 0; kms->frame < FRAMES; kms->frame++) {
		struct timespec dwell = {.tv_nsec = 200000000};
		unsigned int code = kms->cpu * MODES * FRAMES + mode * FRAMES + kms->frame;

		gl.Viewport(0, 0, info->hdisplay, info->vdisplay);
		gl.Uniform1i(kms->code, code);
		gl.Uniform1i(kms->height, info->vdisplay);
		gl.DrawArrays(GL_TRIANGLES, 0, 3);
		if (gl.GetError() || !eglSwapBuffers(display, window)) {
			result = failure(__LINE__);
			break;
		}
		next = gbm_surface_lock_front_buffer(surface);
		if (!next || next == current || framebuffer(kms, next, &next_fb)) {
			result = failure(__LINE__);
			break;
		}
		if (!current)
			result = drmModeSetCrtc(kms->fd, kms->crtc, next_fb, 0, 0,
				&kms->connector, 1, info);
		else {
			kms->token = mode * FRAMES + kms->frame;
			kms->cookies[kms->token] = (struct flip_cookie){kms, kms->token};
			kms->pending = true;
			result = drmModePageFlip(kms->fd, kms->crtc, next_fb,
				DRM_MODE_PAGE_FLIP_EVENT, &kms->cookies[kms->token]);
			if (!result)
				result = wait_flip(kms);
		}
		if (!result)
			result = check_crtc(kms, next_fb, info);
		if (result) {
			result = failure(__LINE__);
			break;
		}
		if (current) {
			if (drmModeRmFB(kms->fd, current_fb)) {
				result = failure(__LINE__);
				break;
			}
			gbm_surface_release_buffer(surface, current);
		}
		current = next;
		current_fb = next_fb;
		next = NULL;
		next_fb = 0;
		/* Keep each completed frame visible for the external display oracle.
		 * This is test pacing, never a substitute for a DRM flip event.
		 */
		while (nanosleep(&dwell, &dwell)) {
			if (errno != EINTR) {
				result = failure(__LINE__);
				break;
			}
		}
		if (result)
			break;
	}
	if (drmModeSetCrtc(kms->fd, kms->crtc, 0, 0, 0, NULL, 0, NULL))
		result = failure(__LINE__);
	if (next_fb && drmModeRmFB(kms->fd, next_fb))
		result = failure(__LINE__);
	if (current_fb && drmModeRmFB(kms->fd, current_fb))
		result = failure(__LINE__);
	if (next)
		gbm_surface_release_buffer(surface, next);
	if (current)
		gbm_surface_release_buffer(surface, current);
	if (!result) {
		struct pollfd pollfd = {.fd = kms->fd, .events = POLLIN};

		if (poll(&pollfd, 1, 0) != 0)
			result = failure(__LINE__);
	}
destroy:
	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
		result = failure(__LINE__);
	if (window != EGL_NO_SURFACE && !eglDestroySurface(display, window))
		result = failure(__LINE__);
	gbm_surface_destroy(surface);
	return result;
}

int kobox_mesa_kms_test(int fd, struct gbm_device *device, EGLDisplay display,
			EGLContext context, unsigned int cpu)
{
	struct kms kms = {.fd = fd, .cpu = cpu};
	int result;

	REQUIRE(!drmSetMaster(fd) && drmIsMaster(fd));
	result = discover(&kms);
	if (!result)
		result = drawing_init(&kms);
	for (unsigned int mode = 0; !result && mode < MODES; mode++)
		result = run_mode(&kms, device, display, context, mode);
	if (kms.vao)
		gl.DeleteVertexArrays(1, &kms.vao);
	if (kms.program)
		gl.DeleteProgram(kms.program);
	if (drmDropMaster(fd))
		result = failure(__LINE__);
	if (!result && kms.events != MODES * (FRAMES - 1))
		result = failure(__LINE__);
	if (!result)
		dprintf(2, "Mesa KMS: cpu=%u modes=2 frames=32 flips=30 events=%u released=1\n",
			cpu, kms.events);
	return result;
}
