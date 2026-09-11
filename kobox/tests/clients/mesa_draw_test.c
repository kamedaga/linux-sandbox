// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "mesa_draw_test.h"
#include "../../boot/exec_gate.h"

#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
	WIDTH = 73, HEIGHT = 59, ROW_PIXELS = WIDTH + 7,
	STRIDE = ROW_PIXELS * 4, GUARD = 64,
	BYTES = 2 * GUARD + STRIDE * HEIGHT,
	SLOTS = 3, FRAMES = 128, POISON = 0xa5,
};

/* Entry points belong to the real Mesa EGL context, not a private GPU API. */
#define DRAW_FUNCTIONS(X) \
	X(CreateShader, PFNGLCREATESHADERPROC) \
	X(ShaderSource, PFNGLSHADERSOURCEPROC) \
	X(CompileShader, PFNGLCOMPILESHADERPROC) \
	X(GetShaderiv, PFNGLGETSHADERIVPROC) \
	X(GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC) \
	X(DeleteShader, PFNGLDELETESHADERPROC) \
	X(CreateProgram, PFNGLCREATEPROGRAMPROC) \
	X(AttachShader, PFNGLATTACHSHADERPROC) \
	X(LinkProgram, PFNGLLINKPROGRAMPROC) \
	X(GetProgramiv, PFNGLGETPROGRAMIVPROC) \
	X(GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC) \
	X(DeleteProgram, PFNGLDELETEPROGRAMPROC) \
	X(UseProgram, PFNGLUSEPROGRAMPROC) \
	X(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC) \
	X(Uniform1i, PFNGLUNIFORM1IPROC) \
	X(Uniform1ui, PFNGLUNIFORM1UIPROC) \
	X(GenVertexArrays, PFNGLGENVERTEXARRAYSPROC) \
	X(BindVertexArray, PFNGLBINDVERTEXARRAYPROC) \
	X(DeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC) \
	X(EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC) \
	X(VertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC) \
	X(GenBuffers, PFNGLGENBUFFERSPROC) \
	X(BindBuffer, PFNGLBINDBUFFERPROC) \
	X(BufferData, PFNGLBUFFERDATAPROC) \
	X(MapBufferRange, PFNGLMAPBUFFERRANGEPROC) \
	X(UnmapBuffer, PFNGLUNMAPBUFFERPROC) \
	X(DeleteBuffers, PFNGLDELETEBUFFERSPROC) \
	X(GenTextures, PFNGLGENTEXTURESPROC) \
	X(BindTexture, PFNGLBINDTEXTUREPROC) \
	X(TexImage2D, PFNGLTEXIMAGE2DPROC) \
	X(TexSubImage2D, PFNGLTEXSUBIMAGE2DPROC) \
	X(TexParameteri, PFNGLTEXPARAMETERIPROC) \
	X(ActiveTexture, PFNGLACTIVETEXTUREPROC) \
	X(DeleteTextures, PFNGLDELETETEXTURESPROC) \
	X(GenFramebuffers, PFNGLGENFRAMEBUFFERSPROC) \
	X(BindFramebuffer, PFNGLBINDFRAMEBUFFERPROC) \
	X(FramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC) \
	X(CheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC) \
	X(DeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC) \
	X(PixelStorei, PFNGLPIXELSTOREIPROC) \
	X(Viewport, PFNGLVIEWPORTPROC) \
	X(Disable, PFNGLDISABLEPROC) \
	X(DrawArrays, PFNGLDRAWARRAYSPROC) \
	X(ReadPixels, PFNGLREADPIXELSPROC) \
	X(FenceSync, PFNGLFENCESYNCPROC) \
	X(ClientWaitSync, PFNGLCLIENTWAITSYNCPROC) \
	X(GetSynciv, PFNGLGETSYNCIVPROC) \
	X(DeleteSync, PFNGLDELETESYNCPROC) \
	X(Flush, PFNGLFLUSHPROC) \
	X(GetError, PFNGLGETERRORPROC)

static struct {
#define DECLARE(name, type) type name;
	DRAW_FUNCTIONS(DECLARE)
#undef DECLARE
} gl;

struct slot {
	GLuint source, target, framebuffer, upload, download;
	GLsync fence;
	unsigned int frame;
};

static int fail(unsigned int line)
{
	struct kobox_exec_failure failure = {
		.pid = getpid(), .line = line,
		.error = gl.GetError ? (int64_t)gl.GetError() : -1,
	};

	dprintf(2, "Mesa draw failure: line=%u GL=%#llx errno=%d\n",
		line, (unsigned long long)failure.error, errno);
	return pwrite(19, &failure, sizeof(failure), KOBOX_EXEC_FAILURE_OFFSET) ==
		sizeof(failure) ? 1 : 2;
}

#define CHECK(condition) do { if (!(condition)) return fail(__LINE__); } while (0)

static int shader(GLenum type, const char *source, GLuint *out)
{
	GLint compiled;
	char log[512];

	*out = gl.CreateShader(type);
	CHECK(*out);
	gl.ShaderSource(*out, 1, &source, NULL);
	gl.CompileShader(*out);
	gl.GetShaderiv(*out, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		gl.GetShaderInfoLog(*out, sizeof(log), NULL, log);
		dprintf(2, "Mesa shader: %s\n", log);
		return fail(__LINE__);
	}
	return 0;
}

static int program(GLuint *out, GLint *frame_location)
{
	const char *vertex =
		"#version 330 core\n"
		"layout(location=0) in vec2 position;\n"
		"void main() { gl_Position=vec4(position,0,1); }\n";
	const char *fragment =
		"#version 330 core\n"
		"uniform usampler2D image; uniform uint frame;\n"
		"layout(location=0) out uvec4 color;\n"
		"void main() {\n"
		" ivec2 sz=textureSize(image,0);\n"
		" ivec2 p=(ivec2(gl_FragCoord.xy)+ivec2(frame*3u,frame*5u))%sz;\n"
		" uvec4 c=texelFetch(image,p,0);\n"
		" color=uvec4(c.b^(frame&255u),(c.r+frame)&255u,255u-c.g,255u);\n"
#ifdef KOBOX_MESA_DRAW_BAD_SHADER
		" if (int(gl_FragCoord.x)==17 && int(gl_FragCoord.y)==23) color.r^=1u;\n"
#endif
		"}\n";
	GLuint vs, fs;
	GLint linked, image_location;
	char log[512];

	if (shader(GL_VERTEX_SHADER, vertex, &vs) || shader(GL_FRAGMENT_SHADER, fragment, &fs))
		return 1;
	*out = gl.CreateProgram();
	CHECK(*out);
	gl.AttachShader(*out, vs);
	gl.AttachShader(*out, fs);
	gl.LinkProgram(*out);
	gl.GetProgramiv(*out, GL_LINK_STATUS, &linked);
	gl.DeleteShader(vs);
	gl.DeleteShader(fs);
	if (!linked) {
		gl.GetProgramInfoLog(*out, sizeof(log), NULL, log);
		dprintf(2, "Mesa program: %s\n", log);
		return fail(__LINE__);
	}
	gl.UseProgram(*out);
	*frame_location = gl.GetUniformLocation(*out, "frame");
	image_location = gl.GetUniformLocation(*out, "image");
	CHECK(*frame_location >= 0 && image_location >= 0);
	gl.Uniform1i(image_location, 0);
	return 0;
}

static void source_pixel(unsigned int x, unsigned int y, unsigned int frame,
			 unsigned char pixel[4])
{
	pixel[0] = (3 * x + 5 * y + 7 * frame) & 255;
	pixel[1] = (x ^ y ^ frame) & 255;
	pixel[2] = (11 * x + 13 * y + 17 * frame) & 255;
	pixel[3] = 255;
}

static void expected_pixel(unsigned int x, unsigned int y, unsigned int frame,
			   unsigned char pixel[4])
{
	unsigned char source[4];

	source_pixel((x + 3 * frame) % WIDTH, (y + 5 * frame) % HEIGHT, frame, source);
	pixel[0] = source[2] ^ (frame & 255);
	pixel[1] = (source[0] + frame) & 255;
	pixel[2] = 255 - source[1];
	pixel[3] = 255;
}

static int create_slot(struct slot *slot)
{
	GLuint textures[2];
	unsigned int index;

	gl.GenTextures(2, textures);
	for (index = 0; index < 2; index++) {
		gl.BindTexture(GL_TEXTURE_2D, textures[index]);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, WIDTH, HEIGHT, 0,
			      GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, NULL);
	}
	slot->source = textures[0];
	slot->target = textures[1];
	gl.GenFramebuffers(1, &slot->framebuffer);
	gl.BindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			      GL_TEXTURE_2D, slot->target, 0);
	CHECK(gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	gl.GenBuffers(1, &slot->upload);
	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, slot->upload);
	gl.BufferData(GL_PIXEL_UNPACK_BUFFER, BYTES, NULL, GL_STREAM_DRAW);
	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	gl.GenBuffers(1, &slot->download);
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, slot->download);
	gl.BufferData(GL_PIXEL_PACK_BUFFER, BYTES, NULL, GL_STREAM_READ);
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	CHECK(gl.GetError() == GL_NO_ERROR);
	return 0;
}

static int submit(struct slot *slot, unsigned int frame, GLint frame_location)
{
	unsigned char *data;
	unsigned int x, y;

	CHECK(!slot->fence);
	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, slot->upload);
	data = gl.MapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, BYTES, GL_MAP_WRITE_BIT);
	CHECK(data);
	memset(data, POISON, BYTES);
	for (y = 0; y < HEIGHT; y++)
		for (x = 0; x < WIDTH; x++)
			source_pixel(x, y, frame, data + GUARD + y * STRIDE + x * 4);
	CHECK(gl.UnmapBuffer(GL_PIXEL_UNPACK_BUFFER));
	gl.BindTexture(GL_TEXTURE_2D, slot->source);
	gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WIDTH, HEIGHT,
			 GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, (void *)(uintptr_t)GUARD);
	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, slot->download);
	data = gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, BYTES, GL_MAP_WRITE_BIT);
	CHECK(data);
	memset(data, POISON, BYTES);
	CHECK(gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER));
	gl.BindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
	gl.Uniform1ui(frame_location, frame);
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
	slot->fence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	CHECK(slot->fence);
	slot->frame = frame;
	gl.Flush();
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	CHECK(gl.GetError() == GL_NO_ERROR);
	return 0;
}

static int wait_fence(GLsync fence, unsigned int *waits)
{
	unsigned int attempt;
	GLenum result = GL_TIMEOUT_EXPIRED;
	GLint status;

	CHECK(fence);
	for (attempt = 0; result == GL_TIMEOUT_EXPIRED && attempt < 5; attempt++) {
		result = gl.ClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
		(*waits)++;
	}
	CHECK(result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED);
	gl.GetSynciv(fence, GL_SYNC_STATUS, 1, NULL, &status);
	CHECK(status == GL_SIGNALED);
	gl.DeleteSync(fence);
	return 0;
}

static int verify_bytes(const unsigned char *data, unsigned int frame)
{
	unsigned char expected[4];
	unsigned int x, y, offset;

	for (offset = 0; offset < BYTES; offset++) {
		if (offset >= GUARD && offset < GUARD + HEIGHT * STRIDE &&
		    (offset - GUARD) % STRIDE < WIDTH * 4)
			continue;
		CHECK(data[offset] == POISON);
	}
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			const unsigned char *actual = data + GUARD + y * STRIDE + 4 * x;

			expected_pixel(x, y, frame, expected);
			if (memcmp(actual, expected, 4)) {
				dprintf(2, "Mesa pixel mismatch: frame=%u x=%u y=%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
					frame, x, y, actual[0], actual[1], actual[2], actual[3],
					expected[0], expected[1], expected[2], expected[3]);
				return fail(__LINE__);
			}
		}
	}
	return 0;
}

static int verify(struct slot *slot, unsigned int *waits)
{
	const unsigned char *data;
	GLsync transfer;
	int result;

	/* Wait on rendering before readback can introduce any implicit stall. */
	if (wait_fence(slot->fence, waits))
		return 1;
	slot->fence = NULL;
	gl.BindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, slot->download);
	gl.ReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
		      (void *)(uintptr_t)GUARD);
	transfer = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	gl.Flush();
	if (wait_fence(transfer, waits))
		return 1;
	/* CPU inspection requires both rendering and transfer fence completion. */
	data = gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, BYTES, GL_MAP_READ_BIT);
	CHECK(data);
	result = verify_bytes(data, slot->frame);
	CHECK(gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER));
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	CHECK(gl.GetError() == GL_NO_ERROR);
	return result;
}

int kobox_mesa_draw_test(void)
{
	const GLfloat vertices[] = {-1, -1, 3, -1, -1, 3};
	struct slot slots[SLOTS] = {0};
	GLuint executable = 0, vao, vbo;
	GLint frame_location = -1;
	unsigned int frame, index, verified = 0, waits = 0;
	int result;

#define LOAD(name, type) gl.name = (type)eglGetProcAddress("gl" #name); CHECK(gl.name);
	DRAW_FUNCTIONS(LOAD)
#undef LOAD
	if (program(&executable, &frame_location))
		return 1;
	gl.GenVertexArrays(1, &vao);
	gl.BindVertexArray(vao);
	gl.GenBuffers(1, &vbo);
	gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
	gl.BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	gl.EnableVertexAttribArray(0);
	gl.Viewport(0, 0, WIDTH, HEIGHT);
	gl.Disable(GL_DITHER);
	gl.Disable(GL_BLEND);
	gl.Disable(GL_DEPTH_TEST);
	gl.Disable(GL_SCISSOR_TEST);
	gl.Disable(GL_FRAMEBUFFER_SRGB);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, ROW_PIXELS);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, ROW_PIXELS);
	for (index = 0; index < SLOTS; index++) {
		result = create_slot(&slots[index]);
		if (result)
			goto release;
	}
	for (frame = 0; frame < FRAMES; frame++) {
		struct slot *slot = &slots[frame % SLOTS];

		if (slot->fence) {
			result = verify(slot, &waits);
			if (result)
				goto release;
			verified++;
		}
		result = submit(slot, frame, frame_location);
		if (result)
			goto release;
	}
	for (index = 0; index < SLOTS; index++) {
		struct slot *slot = &slots[index];

		result = verify(slot, &waits);
		if (result)
			goto release;
		verified++;
	}
release:
	for (index = 0; index < SLOTS; index++) {
		struct slot *slot = &slots[index];

		if (slot->fence) {
			if (wait_fence(slot->fence, &waits))
				result = 1;
			slot->fence = NULL;
		}
		gl.DeleteFramebuffers(1, &slot->framebuffer);
		gl.DeleteTextures(1, &slot->source);
		gl.DeleteTextures(1, &slot->target);
		gl.DeleteBuffers(1, &slot->upload);
		gl.DeleteBuffers(1, &slot->download);
	}
	gl.UseProgram(0);
	gl.DeleteProgram(executable);
	gl.DeleteVertexArrays(1, &vao);
	gl.DeleteBuffers(1, &vbo);
	if (result) {
		CHECK(gl.GetError() == GL_NO_ERROR);
		return result;
	}
	CHECK(verified == FRAMES && waits >= 2 * FRAMES && gl.GetError() == GL_NO_ERROR);
	dprintf(2, "Mesa draw: frames=%u pixels=%u fences=%u waits=%u reused=%u slots=%u uploads=%u readbacks=%u guards=ok\n",
		verified, verified * WIDTH * HEIGHT, 2 * verified, waits,
		FRAMES - SLOTS, SLOTS, FRAMES, verified);
	return 0;
}
