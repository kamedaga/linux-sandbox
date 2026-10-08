// SPDX-License-Identifier: GPL-2.0-only
#define KOBOX_BOOT_RUNTIME 1
#include "irq_transaction.h"
#include "../arch/x86_64/host_call.h"
#include "../arch/x86_64/fp_entry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned int cpu;
static unsigned int mask;
static unsigned int disabled;
static unsigned int enables;
static unsigned int disables;
static unsigned int reentries;
static unsigned int restore_reentry;
static char calls[64];
static unsigned int call_count;
static const struct kobox_linux_task_host_operations host;
static unsigned int current_cpu(void);

static void clobber_fp(void)
{
	unsigned int mxcsr = 0x1f80;

	/* This test TU uses general registers only, like the Linux runtime. These
	 * explicit instructions simulate an arbitrary native ABI callee's FP use.
	 */
	asm volatile("fninit\n\t"
		     "fldz\n\t"
		     "pxor %%xmm0, %%xmm0\n\t"
		     "pcmpeqd %%xmm1, %%xmm1\n\t"
		     "pcmpeqd %%xmm2, %%xmm2\n\t"
		     "pcmpeqd %%xmm3, %%xmm3\n\t"
		     "pcmpeqd %%xmm4, %%xmm4\n\t"
		     "pcmpeqd %%xmm5, %%xmm5\n\t"
		     "pcmpeqd %%xmm6, %%xmm6\n\t"
		     "pcmpeqd %%xmm7, %%xmm7\n\t"
		     "pcmpeqd %%xmm8, %%xmm8\n\t"
		     "pcmpeqd %%xmm9, %%xmm9\n\t"
		     "pcmpeqd %%xmm10, %%xmm10\n\t"
		     "pcmpeqd %%xmm11, %%xmm11\n\t"
		     "pcmpeqd %%xmm12, %%xmm12\n\t"
		     "pcmpeqd %%xmm13, %%xmm13\n\t"
		     "pcmpeqd %%xmm14, %%xmm14\n\t"
		     "pcmpeqd %%xmm15, %%xmm15\n\t"
		     "ldmxcsr %0"
		     : : "m"(mxcsr) : "memory");
}

static void record(char call)
{
	assert(call_count < sizeof(calls) - 1);
	calls[call_count++] = call;
	calls[call_count] = 0;
	clobber_fp();
}

static __attribute__((noinline, used)) int save_mask_body(uint64_t *previous)
{
	unsigned char fp[512] __attribute__((aligned(16))) = {};

	asm volatile("fxsaveq %0" : "=m"(fp) : : "memory");
	assert(fp[0] == 0x7f && fp[1] == 0x03);
	assert(!fp[2] && !fp[3] && !fp[4]);
	assert(fp[24] == 0x80 && fp[25] == 0x1f);
	for (unsigned int i = 160; i < 416; i++)
		assert(!fp[i]);
	record('S');
	*previous = mask;
	mask = 1;
	return 0;
}

static __attribute__((noinline, used)) int restore_mask_body(uint64_t previous)
{
	record('R');
	assert(mask == 1 && previous <= 1);
	mask = previous;
	if (!mask && !disabled && restore_reentry) {
		restore_reentry = 0;
		(void)kobox_irq_transaction(&host, current_cpu, KOBOX_IRQ_QUERY);
		cpu ^= 1;
		reentries++;
	}
	return 0;
}

/* Every table callback obeys its preserving contract while its body still
 * clobbers FP. The transaction itself now has no blanket FP save. */
static __attribute__((naked)) int save_mask(uint64_t *previous __attribute__((unused)))
{
	KOBOX_FP_ENTRY_BODY(save_mask_body);
}

static __attribute__((naked)) int restore_mask(uint64_t previous __attribute__((unused)))
{
	KOBOX_FP_ENTRY_BODY(restore_mask_body);
}

static unsigned int current_cpu(void)
{
	/* Lookup is a leaf, not a host call. It must follow notification save. */
	assert(mask == 1);
	calls[call_count++] = 'C';
	calls[call_count] = 0;
	return cpu;
}

static __attribute__((noinline, used)) uint8_t irq_disabled_body(uint32_t index)
{
	assert(mask == 1 && index == cpu);
	record('Q');
	return disabled;
}

static __attribute__((noinline, used)) int irq_disable_body(uint32_t index)
{
	assert(mask == 1 && index == cpu && !disabled);
	record('D');
	disabled = 1;
	disables++;
	return 0;
}

static __attribute__((noinline, used)) int irq_enable_body(uint32_t index)
{
	assert(mask == 1 && index == cpu && disabled);
	record('E');
	disabled = 0;
	enables++;
	/* Guest reentry can make nested host calls and resume on another CPU.
	 * No stale CPU lookup is permitted after this callback boundary.
	 */
	(void)kobox_host_call((clobber_fp(), 0));
	cpu ^= 1;
	reentries++;
	return 0;
}

static __attribute__((naked)) uint8_t irq_disabled(uint32_t index __attribute__((unused)))
{
	KOBOX_FP_ENTRY_BODY(irq_disabled_body);
}

static __attribute__((naked)) int irq_disable(uint32_t index __attribute__((unused)))
{
	KOBOX_FP_ENTRY_BODY(irq_disable_body);
}

static __attribute__((naked)) int irq_enable(uint32_t index __attribute__((unused)))
{
	KOBOX_FP_ENTRY_BODY(irq_enable_body);
}

static const struct kobox_linux_task_host_operations host = {
	.notifications_save = save_mask,
	.notifications_restore = restore_mask,
	.cpu_irq_disabled = irq_disabled,
	.cpu_irq_disable = irq_disable,
	.cpu_irq_enable = irq_enable,
};

static void reset(unsigned int initial_mask, unsigned int initial_irq)
{
	cpu = 0;
	mask = initial_mask;
	disabled = initial_irq;
	enables = 0;
	disables = 0;
	reentries = 0;
	call_count = 0;
	restore_reentry = 0;
}

static unsigned long transaction(enum kobox_irq_operation operation)
{
	unsigned char before[512] __attribute__((aligned(16))) = {};
	unsigned char after[512] __attribute__((aligned(16))) = {};
	unsigned int mxcsr = 0x5f80;
	unsigned short control = 0x077f;
	unsigned long flags;
	unsigned int i;

	asm volatile("fninit\n\t"
		     "fldcw %1\n\t"
		     "fld1\n\t"
		     "fldpi\n\t"
		     "ldmxcsr %2\n\t"
		     "pcmpeqd %%xmm0, %%xmm0\n\t"
		     "fxsaveq %0"
		     : "=m"(before) : "m"(control), "m"(mxcsr) : "memory");
	flags = kobox_irq_transaction(&host, current_cpu, operation);
	asm volatile("fxsaveq %0" : "=m"(after) : : "memory");
	/* Compare defined x87 control/tag, MXCSR, x87 registers and every XMM
	 * register. Reserved FXSAVE bytes need not have deterministic values.
	 */
	assert(!memcmp(before, after, 5));
	assert(!memcmp(before + 24, after + 24, 4));
	for (i = 0; i < 8; i++)
		assert(!memcmp(before + 32 + 16 * i, after + 32 + 16 * i, 10));
	assert(!memcmp(before + 160, after + 160, 256));
	mxcsr = 0x1f80;
	asm volatile("fninit; ldmxcsr %0" : : "m"(mxcsr) : "memory");
	return flags;
}

int main(void)
{
	unsigned int initial_mask;
	unsigned int initial_irq;
	unsigned int operation;

	for (initial_mask = 0; initial_mask < 2; initial_mask++)
		for (initial_irq = 0; initial_irq < 2; initial_irq++)
			for (operation = KOBOX_IRQ_QUERY;
			     operation <= KOBOX_IRQ_ENABLE; operation++) {
				reset(initial_mask, initial_irq);
				assert(transaction(operation) == initial_irq);
				assert(mask == initial_mask);
				if (operation == KOBOX_IRQ_ENABLE && initial_irq) {
					assert(!disabled && enables == 1 && reentries == 1 && cpu == 1);
					assert(!strcmp(calls, "SCQER"));
				} else if (!initial_irq && (operation == KOBOX_IRQ_SAVE ||
						   operation == KOBOX_IRQ_DISABLE)) {
					assert(disabled && disables == 1);
					assert(!strcmp(calls, "SCQDR"));
				} else {
					assert(disabled == initial_irq && !enables && !disables);
					assert(!strcmp(calls, "SCQR"));
				}
			}
	reset(0, 0);
	assert(transaction(KOBOX_IRQ_SAVE) == 0);
	assert(transaction(KOBOX_IRQ_SAVE) == 1);
	(void)transaction(KOBOX_IRQ_DISABLE);
	assert(disabled && mask == 0);
	(void)transaction(KOBOX_IRQ_ENABLE);
	assert(!disabled && mask == 0);
	reset(0, 0);
	restore_reentry = 1;
	assert(transaction(KOBOX_IRQ_QUERY) == 0);
	assert(cpu == 1 && mask == 0 && reentries == 1 && !restore_reentry);
	assert(!strcmp(calls, "SCQRSCQR"));
	puts("KOBOX_IRQ_TRANSACTION_TEST_OK fp_bank=preserved nested_irq=ok migration=ok");
	return 0;
}
