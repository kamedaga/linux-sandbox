/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_HOST_CALL_H
#define KOBOX_HOST_CALL_H

/* Linux is compiled without implicit FP/SSE use. Native host services follow
 * a different ABI and may clobber every vector register. Preserve the fixed
 * machine's FP/SSE bank across that boundary, including calls which park and
 * later resume this OS thread. This changes no Linux FPU ownership policy.
 */
#ifdef KOBOX_BOOT_RUNTIME
#define kobox_host_call(expression) ({ \
	unsigned char __host_fp[512] __attribute__((aligned(16))); \
	const unsigned int __host_mxcsr = 0x1f80; \
	asm volatile("fxsaveq %0" : "=m" (__host_fp) : : "memory"); \
	asm volatile("fninit; ldmxcsr %0" : : "m" (__host_mxcsr) : "memory"); \
	__auto_type __host_result = (expression); \
	asm volatile("fxrstorq %0" : : "m" (__host_fp) : "memory"); \
	__host_result; \
})
#else
#define kobox_host_call(expression) (expression)
#endif

#endif
