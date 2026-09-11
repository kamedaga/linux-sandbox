/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_EXCEPTION_H
#define KOBOX_LINUX_EXCEPTION_H

#include "../task/host.h"

#include "../arch/x86_64/exception_frame.h"

enum kobox_linux_exception_result {
	KOBOX_EXCEPTION_FATAL,
	KOBOX_EXCEPTION_RESUME,
};

typedef enum kobox_linux_exception_result (*kobox_linux_exception_fn)(
	struct kobox_linux_exception_frame *frame);

struct kobox_linux_warning {
	uint32_t cpu, line;
	char file[192];
};

#ifdef __KERNEL__
enum kobox_linux_exception_result kobox_linux_exception_dispatch(
	struct kobox_linux_exception_frame *frame);
unsigned long kobox_linux_exception_warnings(void);
int kobox_linux_exception_warning(unsigned int index, struct kobox_linux_warning *warning);
#endif

#endif
