/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_HOST_CALL_H
#define KOBOX_HOST_CALL_H

/* Linux is compiled without implicit FP/SSE use. Native host services follow
 * a different ABI and may clobber every vector register. Preserve the fixed
 * machine's FP/SSE bank across that boundary, including calls which park and
 * later resume this OS thread. This changes no Linux FPU ownership policy.
 */
#ifdef KOBOX_BOOT_RUNTIME
/* Architectural legacy FXSAVE image, not a Linux structure layout: masked
 * x87 exceptions, empty tags, default MXCSR and zero vector registers. An
 * immutable image avoids serial FNINIT on every native boundary; callers'
 * original bank is still saved and restored, including across task switches.
 */
static const unsigned char kobox_host_fp_initial[512]
	__attribute__((aligned(16))) = {
	[0] = 0x7f, [1] = 0x03,
	[24] = 0x80, [25] = 0x1f,
};

#define kobox_host_call(expression) ({ \
	unsigned char __host_fp[512] __attribute__((aligned(16))); \
	asm volatile("fxsaveq %0" : "=m" (__host_fp) : : "memory"); \
	asm volatile("fxrstorq %0" : : "m" (kobox_host_fp_initial) : "memory"); \
	__auto_type __host_result = (expression); \
	asm volatile("fxrstorq %0" : : "m" (__host_fp) : "memory"); \
	__host_result; \
})
#else
#define kobox_host_call(expression) (expression)
#endif

#endif
