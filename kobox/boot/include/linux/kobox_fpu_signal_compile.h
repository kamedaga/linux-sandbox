/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_FPU_SIGNAL_COMPILE_H
#define KOBOX_FPU_SIGNAL_COMPILE_H

/*
 * Compile overlay for arch/x86/kernel/fpu/signal.c only.
 *
 * The hosted machine cannot execute FXSAVE/FXRSTOR directly against a guest
 * virtual address: that address belongs to Linux's software page tables, not
 * to the runtime process. Load the unchanged upstream private helpers first,
 * then replace only their signal-frame operands with the hosted uaccess port.
 * Signal layout, retry, validation and delivery remain upstream code.
 */
#include <linux/compat.h>
#include <linux/cpu.h>
#include <linux/pagemap.h>

#include <asm/fpu/signal.h>
#include <asm/fpu/regset.h>
#include <asm/fpu/xstate.h>
#include <asm/sigframe.h>
#include <asm/trapnr.h>
#include <asm/trace/fpu.h>

#include "../../../../arch/x86/kernel/fpu/context.h"
#include "../../../../arch/x86/kernel/fpu/internal.h"
#include "../../../../arch/x86/kernel/fpu/legacy.h"
#include "../../../../arch/x86/kernel/fpu/xstate.h"
#include <linux/kobox_fpu_user.h>

#endif
