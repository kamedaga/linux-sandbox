/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_ASM_PERCPU_H
#define KOBOX_PROVIDER_ASM_PERCPU_H

/* KOBOX_PERCPU_SEGMENT: upstream x86 per-CPU operations. The host gives
 * each thread a GS base at its logical CPU's area (runtime/host.h), so every
 * this_cpu_*() is again one uninterruptible %gs-relative instruction and
 * needs no host notification mask. Otherwise the generic fallback with a
 * mask around each operation. */
#if defined(__ASSEMBLY__)
#include_next <asm/percpu.h>
#elif defined(KOBOX_PERCPU_SEGMENT)
#include_next <asm/percpu.h>
/* Hosted objects are large-model PIC, so a per-CPU variable's address is not
 * an immediate operand as upstream's stable read requires. A memory operand
 * is still one %gs-relative load (the address is formed in a register first,
 * which is not a per-CPU access), and without volatile or a memory clobber
 * the compiler may still reuse the value like upstream's. */
#undef __raw_cpu_read_stable
#define __raw_cpu_read_stable(size, _var)				\
({									\
	__pcpu_type_##size pfo_val__;					\
									\
	asm(__pcpu_op_##size("mov")					\
	    __force_percpu_arg([var]) ", %[val]"			\
	    : [val] __pcpu_reg_##size("=", pfo_val__)			\
	    : [var] "m" (__my_cpu_var(_var)));				\
									\
	(typeof(_var))(unsigned long) pfo_val__;			\
})

/* Hosted CPUs report no CX16 (cpu_port.c keeps them at the common x86-64
 * baseline), so upstream's 16-byte forms would call this_cpu_cmpxchg16b_emu,
 * whose CLI faults outside ring 0. The host notification mask gives the same
 * exclusion as that CLI: no interrupt callback and no move off this logical
 * CPU between comparing and storing the two words. */
extern unsigned long kobox_provider_preempt_save(void);
extern void kobox_provider_preempt_restore(unsigned long flags);
#define kobox_percpu_masked(expression)					\
({									\
	unsigned long kobox_pcp_mask__ = kobox_provider_preempt_save();	\
	typeof(expression) kobox_pcp_ret__ = (expression);		\
	kobox_provider_preempt_restore(kobox_pcp_mask__);		\
	kobox_pcp_ret__;						\
})
#undef raw_cpu_cmpxchg128
#undef this_cpu_cmpxchg128
#undef raw_cpu_try_cmpxchg128
#undef this_cpu_try_cmpxchg128
#define raw_cpu_cmpxchg128(pcp, oval, nval)				\
	kobox_percpu_masked(raw_cpu_generic_cmpxchg(pcp, oval, nval))
#define this_cpu_cmpxchg128(pcp, oval, nval)				\
	kobox_percpu_masked(raw_cpu_generic_cmpxchg(pcp, oval, nval))
#define raw_cpu_try_cmpxchg128(pcp, ovalp, nval)			\
	kobox_percpu_masked((bool)raw_cpu_generic_try_cmpxchg(pcp, ovalp, nval))
#define this_cpu_try_cmpxchg128(pcp, ovalp, nval)			\
	kobox_percpu_masked((bool)raw_cpu_generic_try_cmpxchg(pcp, ovalp, nval))
#else
#include <kobox/provider_percpu.h>
#endif

#endif /* KOBOX_PROVIDER_ASM_PERCPU_H */
