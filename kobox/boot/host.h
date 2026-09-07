/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_BOOT_HOST_H
#define KOBOX_LINUX_BOOT_HOST_H

#include "../task/host.h"
#include "exception.h"

enum kobox_linux_image_protection {
	KOBOX_IMAGE_READ = 1U << 0,
	KOBOX_IMAGE_WRITE = 1U << 1,
	KOBOX_IMAGE_EXECUTE = 1U << 2,
};

/* Process-local machine bootstrap, not a wire or controller interface. */
struct kobox_linux_boot_layout {
	size_t size;
	struct kobox_linux_task_layout task;
	int (*exceptions_install)(kobox_linux_exception_fn dispatch);
	/* Change owned core-image pages and their direct RAM aliases (NX).
	 * Zero revokes the image view, leaving writable RAM for buddy reuse.
	 * The caller supplies Linux's synchronization, not the host adapter.
	 */
	int (*image_protect)(void *image, size_t offset, size_t length,
			     unsigned int protection);
	void *image;
	const char *command_line;
	void (*console_write)(void *context, const char *text, size_t length);
	void *console_context;
	/* Called in Linux PID 1 after upstream kernel-service initialization. */
	void (*kernel_main)(void *argument);
	void *kernel_argument;
};

#ifdef __KERNEL__
void *kobox_linux_boot_text_alias(void *where, unsigned long size);
#endif

#endif
