// SPDX-License-Identifier: GPL-2.0-only

#include "../arch/x86_64/host_call.h"

#include <sched.h>
#include <string.h>

static int calls;

static __attribute__((noinline)) int native_call(void)
{
	const unsigned int mxcsr = 0x1f80;
	unsigned int observed;
	unsigned short control;

	calls++;
	asm volatile("stmxcsr %0; fnstcw %1" : "=m"(observed), "=m"(control));
	if (observed != mxcsr || control != 0x37f)
		return -43;
	sched_yield();
	asm volatile("fninit; pxor %%xmm0, %%xmm0; pxor %%xmm8, %%xmm8; ldmxcsr %0"
		     : : "m"(mxcsr) : "memory");
	return -42;
}

int main(void)
{
	unsigned char before[512] __attribute__((aligned(16))) = {0};
	unsigned char after[512] __attribute__((aligned(16))) = {0};
	const unsigned int mxcsr = 0x7f80;
	unsigned int original;
	int result;

	asm volatile("stmxcsr %0" : "=m"(original));
	asm volatile("fld1; movd %0, %%xmm0; movd %0, %%xmm8; ldmxcsr %1"
		     : : "r"(0x12345678), "m"(mxcsr) : "memory");
	asm volatile("fxsaveq %0" : "=m"(before) : : "memory");
	result = kobox_host_call(native_call());
	asm volatile("fxsaveq %0" : "=m"(after) : : "memory");
	asm volatile("fninit; ldmxcsr %0" : : "m"(original) : "memory");
	return result != -42 || calls != 1 || memcmp(before, after, sizeof(before));
}
