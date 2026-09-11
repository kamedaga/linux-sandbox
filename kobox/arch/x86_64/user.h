/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_USER_H
#define KOBOX_X86_USER_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Process-local machine state, not Linux pt_regs, a ptrace structure or wire
 * data. Adapters translate named registers; no native structure is overlaid.
 */
/* Architectural FXSAVE image for the fixed core's legacy FP/SSE machine. */
struct kobox_x86_fp_state {
	uint8_t fxsave[512];
};

struct kobox_x86_user_regs {
	uint64_t ax, bx, cx, dx, si, di, bp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t ip, sp, flags, orig_ax;
	uint64_t fs_base, gs_base;
	uint32_t cs, ss, ds, es, fs, gs;
};

#endif
