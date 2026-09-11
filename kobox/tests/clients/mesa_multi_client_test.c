// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "../../boot/exec_gate.h"
#include "mesa_kms_test.h"

#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum { WIDTH = 79, HEIGHT = 61, FRAMES = 32 };

#define GL_FUNCTIONS(X) \
	X(GetString, PFNGLGETSTRINGPROC) \
	X(GetError, PFNGLGETERRORPROC) \
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
	X(GenTextures, PFNGLGENTEXTURESPROC) \
	X(BindTexture, PFNGLBINDTEXTUREPROC) \
	X(DeleteTextures, PFNGLDELETETEXTURESPROC) \
	X(GenFramebuffers, PFNGLGENFRAMEBUFFERSPROC) \
	X(BindFramebuffer, PFNGLBINDFRAMEBUFFERPROC) \
	X(FramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC) \
	X(CheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC) \
	X(DeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC) \
	X(Viewport, PFNGLVIEWPORTPROC) \
	X(Disable, PFNGLDISABLEPROC) \
	X(DrawArrays, PFNGLDRAWARRAYSPROC) \
	X(ReadPixels, PFNGLREADPIXELSPROC) \
	X(Flush, PFNGLFLUSHPROC)

static struct {
#define DECLARE(name, type) type name;
	GL_FUNCTIONS(DECLARE)
#undef DECLARE
	void (*image_target)(GLenum target, void *image);
	PFNEGLCREATESYNCKHRPROC create_sync;
	PFNEGLDESTROYSYNCKHRPROC destroy_sync;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC fence_fd;
} gl;

struct buffer {
	struct gbm_bo *bo;
	EGLImage image;
	GLuint texture, framebuffer;
};

struct client {
	int fd, socket;
	unsigned int role, cpu;
	struct gbm_device *device;
	EGLDisplay display;
	EGLContext context;
	struct buffer own, peer;
	GLuint program, vao;
	GLint code;
};

/* Private AF_UNIX client payload; FDs travel exclusively in SCM_RIGHTS. */
struct message {
	unsigned int phase, role, value;
};

static int failure(unsigned int line)
{
	struct kobox_exec_failure result = {
		.pid = getpid(), .line = line, .error = errno,
	};

	dprintf(2, "Mesa multi failure: pid=%d line=%u errno=%d EGL=%#x GL=%#x\n",
		getpid(), line, errno, eglGetError(),
		gl.GetError ? gl.GetError() : 0);
	return pwrite(19, &result, sizeof(result), KOBOX_EXEC_FAILURE_OFFSET) ==
		sizeof(result) ? 1 : 2;
}

#define CHECK(condition) do { if (!(condition)) return failure(__LINE__); } while (0)

static int ready(int fd)
{
	struct pollfd event = {.fd = fd, .events = POLLIN};
	int result;

	do {
		result = poll(&event, 1, 5000);
	} while (result < 0 && errno == EINTR);
	CHECK(result == 1 && (event.revents & POLLIN) &&
	      !(event.revents & (POLLERR | POLLNVAL)));
	return 0;
}

static int send_message(struct client *client, unsigned int phase,
			unsigned int value, int fd)
{
	struct message data = {phase, client->role, value};
	struct iovec iov = {&data, sizeof(data)};
	union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
	struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};

	if (fd >= 0) {
		struct cmsghdr *header;

		message.msg_control = control.bytes;
		message.msg_controllen = sizeof(control.bytes);
		header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(fd));
		memcpy(CMSG_DATA(header), &fd, sizeof(fd));
	}
	CHECK(sendmsg(client->socket, &message, MSG_NOSIGNAL) == sizeof(data));
	return 0;
}

static int receive_message(struct client *client, unsigned int phase,
			   unsigned int *value, int *fd)
{
	struct message data;
	struct iovec iov = {&data, sizeof(data)};
	union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
	struct msghdr message = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
	};
	struct cmsghdr *header;

	CHECK(!ready(client->socket));
	CHECK(recvmsg(client->socket, &message, MSG_CMSG_CLOEXEC) == sizeof(data));
	CHECK(!(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
	      data.phase == phase && data.role == (client->role ^ 1));
	*value = data.value;
	header = CMSG_FIRSTHDR(&message);
	if (!fd) {
		CHECK(!header);
		return 0;
	}
	CHECK(header && header->cmsg_level == SOL_SOCKET &&
	      header->cmsg_type == SCM_RIGHTS &&
	      header->cmsg_len == CMSG_LEN(sizeof(*fd)) &&
	      !CMSG_NXTHDR(&message, header));
	memcpy(fd, CMSG_DATA(header), sizeof(*fd));
	CHECK(*fd >= 0 && (fcntl(*fd, F_GETFD) & FD_CLOEXEC));
	return 0;
}

static int context_open(struct client *client)
{
	const EGLint attributes[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE,
	};
	EGLConfig config;
	const char *renderer, *extensions;
	cpu_set_t cpus;

	CPU_ZERO(&cpus);
	CPU_SET(client->cpu, &cpus);
	CHECK(!sched_setaffinity(0, sizeof(cpus), &cpus));
	client->fd = open(client->role ? "/dev/dri/renderD128" : "/dev/dri/card0",
			  O_RDWR | O_CLOEXEC);
	CHECK(client->fd >= 0);
	client->device = gbm_create_device(client->fd);
	CHECK(client->device);
	client->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, client->device, NULL);
	CHECK(client->display != EGL_NO_DISPLAY &&
	      eglInitialize(client->display, NULL, NULL) && eglBindAPI(EGL_OPENGL_API));
	extensions = eglQueryString(client->display, EGL_EXTENSIONS);
	CHECK(extensions && strstr(extensions, "EGL_EXT_image_dma_buf_import ") &&
	      strstr(extensions, "EGL_ANDROID_native_fence_sync "));
	config = kobox_mesa_kms_config(client->display);
	CHECK(config);
	client->context = eglCreateContext(client->display, config, EGL_NO_CONTEXT, attributes);
	CHECK(client->context != EGL_NO_CONTEXT &&
	      eglMakeCurrent(client->display, EGL_NO_SURFACE, EGL_NO_SURFACE, client->context));
#define LOAD(name, type) gl.name = (type)eglGetProcAddress("gl" #name); CHECK(gl.name);
	GL_FUNCTIONS(LOAD)
#undef LOAD
	gl.image_target = (void (*)(GLenum, void *))eglGetProcAddress("glEGLImageTargetTexture2DOES");
	gl.create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
	gl.destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
	gl.fence_fd = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
	CHECK(gl.image_target && gl.create_sync && gl.destroy_sync && gl.fence_fd);
	renderer = (const char *)gl.GetString(GL_RENDERER);
	CHECK(renderer && strstr(renderer, "virgl (") &&
	      !strcasestr(renderer, "llvmpipe") && !strcasestr(renderer, "softpipe") &&
	      !strcasestr(renderer, "software"));
	dprintf(2, "Mesa multi: pid=%d role=%u cpu=%u renderer=%s\n",
		getpid(), client->role, client->cpu, renderer);
	return 0;
}

static int buffer_import(struct client *client, struct buffer *buffer,
			 int fd, unsigned int stride)
{
	const EGLAttrib attributes[] = {
		EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT,
		EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_ARGB8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride, EGL_NONE,
	};

	CHECK(stride >= WIDTH * 4 && stride <= 4096);
	buffer->image = eglCreateImage(client->display, EGL_NO_CONTEXT,
				      EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
	CHECK(buffer->image != EGL_NO_IMAGE);
	gl.GenTextures(1, &buffer->texture);
	gl.BindTexture(GL_TEXTURE_2D, buffer->texture);
	gl.image_target(GL_TEXTURE_2D, buffer->image);
	gl.GenFramebuffers(1, &buffer->framebuffer);
	gl.BindFramebuffer(GL_FRAMEBUFFER, buffer->framebuffer);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			      GL_TEXTURE_2D, buffer->texture, 0);
	CHECK(gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
	      gl.GetError() == GL_NO_ERROR);
	return 0;
}

static int buffers_exchange(struct client *client)
{
	unsigned int stride;
	int fd, result;

	client->own.bo = gbm_bo_create(client->device, WIDTH, HEIGHT,
		GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	CHECK(client->own.bo && gbm_bo_get_plane_count(client->own.bo) == 1 &&
	      gbm_bo_get_offset(client->own.bo, 0) == 0 &&
	      gbm_bo_get_modifier(client->own.bo) == DRM_FORMAT_MOD_LINEAR);
	stride = gbm_bo_get_stride(client->own.bo);
	fd = gbm_bo_get_fd(client->own.bo);
	CHECK(fd >= 0);
	result = buffer_import(client, &client->own, fd, stride);
	if (!result)
		result = send_message(client, 1, stride, fd);
	CHECK(!close(fd) && !result);
	CHECK(!receive_message(client, 1, &stride, &fd));
	result = buffer_import(client, &client->peer, fd, stride);
	CHECK(!close(fd) && !result);
	return 0;
}

static GLuint compile_shader(GLenum type, const char *source)
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

static int program_create(struct client *client)
{
	const char *vertex = "#version 330 core\n"
		"void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
		"gl_Position=vec4(p*2.0-1.0,0,1);}";
	const char *fragment = "#version 330 core\n"
		"uniform int code;out vec4 color;void main(){"
		"int x=int(gl_FragCoord.x),y=int(gl_FragCoord.y);"
		"ivec3 c=ivec3((x*3+code*7)%256,(y*5+code*11)%256,"
		"(x+y+code*13)%256);"
#ifdef KOBOX_MESA_MULTI_BAD_SHADER
		"if(code==32 && x==17 && y==23)c.b^=1;"
#endif
		"color=vec4(vec3(c)/255.0,1);}";
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment);
	GLint linked;

	CHECK(vs && fs);
	client->program = gl.CreateProgram();
	gl.AttachShader(client->program, vs);
	gl.AttachShader(client->program, fs);
	gl.LinkProgram(client->program);
	gl.DeleteShader(vs);
	gl.DeleteShader(fs);
	gl.GetProgramiv(client->program, GL_LINK_STATUS, &linked);
	CHECK(linked);
	client->code = gl.GetUniformLocation(client->program, "code");
	CHECK(client->code >= 0);
	gl.GenVertexArrays(1, &client->vao);
	gl.BindVertexArray(client->vao);
	gl.UseProgram(client->program);
	gl.Disable(GL_DITHER);
	gl.Disable(GL_FRAMEBUFFER_SRGB);
	gl.Viewport(0, 0, WIDTH, HEIGHT);
	return 0;
}

static int render(struct client *client, unsigned int frame)
{
	EGLSyncKHR sync;
	int fd, result;

	gl.BindFramebuffer(GL_FRAMEBUFFER, client->own.framebuffer);
	gl.Uniform1i(client->code, client->role * FRAMES + frame);
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
	sync = gl.create_sync(client->display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
	CHECK(sync != EGL_NO_SYNC_KHR);
	gl.Flush();
	fd = gl.fence_fd(client->display, sync);
	CHECK(gl.destroy_sync(client->display, sync) && fd >= 0);
	result = send_message(client, 2, frame, fd);
	CHECK(!close(fd) && !result && gl.GetError() == GL_NO_ERROR);
	return 0;
}

static int pixels(struct buffer *buffer, unsigned int role, unsigned int frame)
{
	unsigned char data[HEIGHT][WIDTH][4];
	unsigned int x, y, code = role * FRAMES + frame;

	gl.BindFramebuffer(GL_FRAMEBUFFER, buffer->framebuffer);
	gl.ReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, data);
	CHECK(gl.GetError() == GL_NO_ERROR);
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			unsigned char expected[] = {
				(x * 3 + code * 7) % 256, (y * 5 + code * 11) % 256,
				(x + y + code * 13) % 256, 255,
			};

			if (memcmp(data[y][x], expected, 4)) {
				dprintf(2, "Mesa shared pixel mismatch: role=%u frame=%u x=%u y=%u\n",
					role, frame, x, y);
				return failure(__LINE__);
			}
		}
	}
	return 0;
}

static int shared_draw(struct client *client)
{
	unsigned int frame, value;
	int fd, result;

	CHECK(!buffers_exchange(client) && !program_create(client));
	for (frame = 0; frame < FRAMES; frame++) {
		if (frame == FRAMES / 2) {
			if (!client->role) {
				uint32_t phase = 5;
				unsigned int attempt;

				CHECK(pwrite(19, &phase, sizeof(phase), KOBOX_EXEC_DEVICE_OFFSET) == sizeof(phase));
				for (attempt = 0; attempt < 5000; attempt++) {
					CHECK(pread(19, &phase, sizeof(phase), KOBOX_EXEC_DEVICE_OFFSET) == sizeof(phase));
					if (phase == 6)
						break;
					usleep(1000);
				}
				CHECK(phase == 6 && !send_message(client, 4, frame, -1));
			} else {
				CHECK(!receive_message(client, 4, &value, NULL) && value == frame);
			}
		}
		/* Both clients submit before receiving the peer's real sync_file.
		 * The acknowledgement prevents reuse while either reader is active.
		 */
		CHECK(!render(client, frame));
		CHECK(!receive_message(client, 2, &value, &fd));
		if (frame == FRAMES / 2) {
			struct kobox_exec_fence_wait waiter = {.pid = getpid(), .fd = fd};
			uint32_t phase = 1;

			CHECK(pwrite(19, &waiter, sizeof(waiter), KOBOX_EXEC_FENCE_WAIT_OFFSET +
				     client->role * sizeof(waiter)) == sizeof(waiter));
			if (!client->role)
				CHECK(pwrite(19, &phase, sizeof(phase), KOBOX_EXEC_DEVICE_OFFSET) == sizeof(phase));
		}
		result = ready(fd);
		CHECK(!close(fd) && !result && value == frame);
		CHECK(!pixels(&client->own, client->role, frame) &&
		      !pixels(&client->peer, client->role ^ 1, frame));
		CHECK(!send_message(client, 3, frame, -1));
		CHECK(!receive_message(client, 3, &value, NULL) && value == frame);
	}
	dprintf(2, "Mesa shared: role=%u frames=32 pixels=%u sync_file=32 peer=ok\n",
		client->role, 2 * FRAMES * WIDTH * HEIGHT);
	return 0;
}

static int buffer_close(struct client *client, struct buffer *buffer)
{
	gl.DeleteFramebuffers(1, &buffer->framebuffer);
	gl.DeleteTextures(1, &buffer->texture);
	CHECK(eglDestroyImage(client->display, buffer->image));
	if (buffer->bo)
		gbm_bo_destroy(buffer->bo);
	return 0;
}

static int survivor(struct client *client, pid_t child)
{
	int status;
	pid_t waited;

	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	CHECK(waited == child && WIFEXITED(status) && !WEXITSTATUS(status));
	/* The exporter exited without eglTerminate/GBM teardown. Only the
	 * surviving import's upstream GEM/dma-buf references keep its data alive.
	 */
	CHECK(!pixels(&client->peer, 1, FRAMES - 1));
	dprintf(2, "Mesa shared lifetime: exporter-exited=1 retained-pixels=%u\n",
		WIDTH * HEIGHT);
	CHECK(!buffer_close(client, &client->peer) && !buffer_close(client, &client->own));
	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.DeleteVertexArrays(1, &client->vao);
	gl.DeleteProgram(client->program);
	CHECK(!kobox_mesa_kms_test(client->fd, client->device, client->display,
				 client->context, client->cpu));
	CHECK(eglMakeCurrent(client->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
	CHECK(eglDestroyContext(client->display, client->context) && eglTerminate(client->display));
	eglReleaseThread();
	gbm_device_destroy(client->device);
	CHECK(!close(client->fd));
	return 0;
}

int main(int argc, char **argv)
{
	struct kobox_exec_result report = {.pid = getpid(), .phase = 2};
	struct client client = {0};
	int sockets[2], result;
	pid_t child;

	CHECK(argc == 3 && (!strcmp(argv[2], "0") || !strcmp(argv[2], "1")));
	report.cpu = argv[2][0] - '0';
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets));
	/* Neither process inherits a Mesa context, worker thread or DRM file. */
	child = fork();
	CHECK(child >= 0);
	client.role = !child;
	client.cpu = report.cpu ^ client.role;
	client.socket = sockets[client.role];
	CHECK(!close(sockets[client.role ^ 1]));
	result = context_open(&client);
	if (!result)
		result = shared_draw(&client);
	if (!child)
		_exit(result);
	if (result) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return result;
	}
	CHECK(!survivor(&client, child) && !close(client.socket));
	CHECK(pwrite(19, &report, sizeof(report), 0) == sizeof(report));
	return 0;
}
