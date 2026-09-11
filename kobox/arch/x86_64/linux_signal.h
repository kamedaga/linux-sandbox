/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_LINUX_SIGNAL_H
#define KOBOX_X86_LINUX_SIGNAL_H

#include "exception_frame.h"

/* Linux native frame translation; never included by common machine state. */
void kobox_x86_linux_exception_import(struct kobox_linux_exception_frame *frame,
				     void *native_context, uintptr_t fault_address);
void kobox_x86_linux_exception_export(
	const struct kobox_linux_exception_frame *frame, void *native_context);

#endif
