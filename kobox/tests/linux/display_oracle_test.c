// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

/* Observe the actual SDL/X11 output, without access to guest buffers or GL.
 * QEMU's GL scanout bypasses the pixman surface used by QMP screendump.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stopping;
static int xerror;

static void stop(int signal)
{
	(void)signal;
	stopping = 1;
}

static int error(Display *display, XErrorEvent *event)
{
	(void)display;
	xerror = event->error_code;
	return 0;
}

static Window find_window(Display *display, Window parent, const char *name,
			  unsigned int depth)
{
	Window root, owner, *children = NULL, found = None;
	unsigned int count;
	char *title = NULL;

	if (XFetchName(display, parent, &title) && title) {
		if (strstr(title, name))
			found = parent;
		XFree(title);
	}
	if (found || depth == 6)
		return found;
	if (!XQueryTree(display, parent, &root, &owner, &children, &count))
		return None;
	for (unsigned int i = 0; i < count && !found; i++)
		found = find_window(display, children[i], name, depth + 1);
	XFree(children);
	return found;
}

static bool matches(XImage *image, unsigned int code)
{
	if (image->red_mask != 0xff0000 || image->green_mask != 0xff00 ||
	    image->blue_mask != 0xff)
		return false;
	for (int y = 0; y < image->height; y++) {
		for (int x = 0; x < image->width; x++) {
			unsigned long expected = ((16 + 3 * code) << 16) |
				((32 + 48 * ((x / 32) % 4)) << 8) |
				(32 + 48 * ((y / 32) % 4));

			if ((XGetPixel(image, x, y) & 0xffffff) != expected)
				return false;
		}
	}
	return true;
}

static int save(XImage *image, const char *directory, unsigned int code)
{
	char path[4096];
	FILE *file;
	int length = snprintf(path, sizeof(path), "%s/display-%02u.ppm", directory, code);

	if (length < 0 || (size_t)length >= sizeof(path))
		return 1;
	file = fopen(path, "wbx");
	if (!file)
		return 1;
	fprintf(file, "P6\n%d %d\n255\n", image->width, image->height);
	for (int y = 0; y < image->height; y++) {
		for (int x = 0; x < image->width; x++) {
			unsigned long pixel = XGetPixel(image, x, y);
			unsigned char rgb[] = {pixel >> 16, pixel >> 8, pixel};

			if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) {
				fclose(file);
				return 1;
			}
		}
	}
	return fclose(file) != 0;
}

int main(int argc, char **argv)
{
	struct sigaction action = {.sa_handler = stop};
	Display *display;
	Window window = None;
	uint64_t seen = 0, mismatched = 0, pixels = 0;
	unsigned int samples = 0;
	bool failed = false;

	if (argc != 3 || !(display = XOpenDisplay(NULL)))
		return 2;
	XSetErrorHandler(error);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (unsigned int attempt = 0; !stopping && seen != UINT64_MAX && attempt < 6000; attempt++) {
		struct timespec interval = {.tv_nsec = 10000000};
		XWindowAttributes attributes;
		XImage *image;
		unsigned int red, code;
		bool valid;

		nanosleep(&interval, NULL);
		if (!window)
			window = find_window(display, DefaultRootWindow(display), argv[1], 0);
		xerror = 0;
		if (!window || !XGetWindowAttributes(display, window, &attributes) ||
		    xerror || attributes.map_state != IsViewable) {
			window = None;
			continue;
		}
		if (!((attributes.width == 640 && attributes.height == 480) ||
		      (attributes.width == 800 && attributes.height == 600)))
			continue;
		image = XGetImage(display, window, 0, 0, attributes.width,
			attributes.height, AllPlanes, ZPixmap);
		if (!image || xerror) {
			if (image)
				XDestroyImage(image);
			window = None;
			continue;
		}
		samples++;
		red = (XGetPixel(image, 0, 0) >> 16) & 255;
		if (red < 16 || red > 205 || (red - 16) % 3) {
			XDestroyImage(image);
			continue;
		}
		code = (red - 16) / 3;
		valid = attributes.width == ((code / 16) % 2 ? 800 : 640) && matches(image, code);
		if (valid && !(seen & (UINT64_C(1) << code))) {
			seen |= UINT64_C(1) << code;
			pixels += (uint64_t)image->width * image->height;
			if ((code % 16 == 0 || code % 16 == 15) && save(image, argv[2], code))
				failed = true;
			printf("KMS display: code=%u size=%dx%d pixels=ok\n",
				code, image->width, image->height);
		} else if (!valid) {
			mismatched |= UINT64_C(1) << code;
		}
		XDestroyImage(image);
	}
	XCloseDisplay(display);
	printf("KMS display oracle: seen=%016llx mismatched=%016llx pixels=%llu samples=%u saved=%u\n",
		(unsigned long long)seen, (unsigned long long)mismatched,
		(unsigned long long)pixels, samples, !failed);
	return failed || seen != UINT64_MAX;
}
