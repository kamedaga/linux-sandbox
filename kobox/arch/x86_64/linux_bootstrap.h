/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_LINUX_BOOTSTRAP_H
#define KOBOX_X86_LINUX_BOOTSTRAP_H

#include "exception_frame.h"
#include "linux_call.h"

#include <asm/signal.h>
#include <asm/sigcontext.h>
#include <asm/ucontext.h>

/* The freestanding bootstrap uses native UAPI, not glibc's ucontext_t. */
static inline void kobox_x86_linux_bootstrap_fault(
	struct kobox_linux_exception_frame *frame, const struct ucontext *context)
{
	frame->ip = context->uc_mcontext.rip;
	frame->sp = context->uc_mcontext.rsp;
	frame->flags = context->uc_mcontext.eflags;
	frame->error_code = context->uc_mcontext.err;
}

#endif
