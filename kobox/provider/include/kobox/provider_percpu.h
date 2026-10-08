/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_PERCPU_H
#define KOBOX_PROVIDER_PERCPU_H

/*
 * Provider objects execute in a userspace address space.  Use the generic
 * Linux percpu operations with one offset owned by each native host thread.
 */
#ifndef __ASSEMBLY__
#ifndef _ASM_X86_PERCPU_H
#define _ASM_X86_PERCPU_H

extern unsigned long kobox_provider_current_percpu_offset(void);

#define __my_cpu_offset kobox_provider_current_percpu_offset()
#include <asm-generic/percpu.h>

#ifndef KOBOX_PERCPU_GENERIC_IRQ
/*
 * Native x86 this_cpu_*() is one %gs-relative instruction: it cannot be split
 * by an interrupt or a migration. The generic fallback restores that by
 * disabling IRQs, which here is a host IRQ transaction (CPU lookup, state query
 * and change, each inside a notification mask) on both sides of the access.
 *
 * Only the notification mask is needed. While it is held no Linux interrupt
 * callback is entered on this task (the logical CPU keeps the event pending
 * until unmask), and the task cannot leave its logical CPU, because that only
 * happens through Linux scheduling. The per-CPU offset therefore stays valid
 * and no handler on this CPU interleaves, exactly as for the preempt_count
 * operations in task/include/asm/preempt.h. The virtual IRQ state itself is
 * neither read nor cached, and remote per_cpu() access keeps its own locking,
 * as on native x86 where these operations are only CPU-local atomic.
 *
 * Every size-specific this_cpu_*() above expands to these helpers when used,
 * so redefining them after the include changes all of them. Define
 * KOBOX_PERCPU_GENERIC_IRQ to build the upstream IRQ-disabling fallback for
 * comparison.
 */
extern unsigned long kobox_provider_preempt_save(void);
extern void kobox_provider_preempt_restore(unsigned long flags);

#define kobox_this_cpu_masked(expression) \
({ \
	unsigned long kobox_pcp_mask__ = kobox_provider_preempt_save(); \
	typeof(expression) kobox_pcp_ret__ = (expression); \
	kobox_provider_preempt_restore(kobox_pcp_mask__); \
	kobox_pcp_ret__; \
})

#undef this_cpu_generic_read
#define this_cpu_generic_read(pcp) \
({ \
	TYPEOF_UNQUAL(pcp) kobox_pcp_value__; \
	unsigned long kobox_pcp_mask__ = kobox_provider_preempt_save(); \
	if (__native_word(pcp)) \
		kobox_pcp_value__ = READ_ONCE(*raw_cpu_ptr(&(pcp))); \
	else \
		kobox_pcp_value__ = raw_cpu_generic_read(pcp); \
	kobox_provider_preempt_restore(kobox_pcp_mask__); \
	kobox_pcp_value__; \
})

#undef this_cpu_generic_to_op
#define this_cpu_generic_to_op(pcp, val, op) \
do { \
	unsigned long kobox_pcp_mask__ = kobox_provider_preempt_save(); \
	raw_cpu_generic_to_op(pcp, val, op); \
	kobox_provider_preempt_restore(kobox_pcp_mask__); \
} while (0)

#undef this_cpu_generic_add_return
#define this_cpu_generic_add_return(pcp, val) \
	kobox_this_cpu_masked(raw_cpu_generic_add_return(pcp, val))

#undef this_cpu_generic_xchg
#define this_cpu_generic_xchg(pcp, nval) \
	kobox_this_cpu_masked(raw_cpu_generic_xchg(pcp, nval))

#undef this_cpu_generic_try_cmpxchg
#define this_cpu_generic_try_cmpxchg(pcp, ovalp, nval) \
	kobox_this_cpu_masked((bool)raw_cpu_generic_try_cmpxchg(pcp, ovalp, nval))

#undef this_cpu_generic_cmpxchg
#define this_cpu_generic_cmpxchg(pcp, oval, nval) \
	kobox_this_cpu_masked(raw_cpu_generic_cmpxchg(pcp, oval, nval))
#endif /* KOBOX_PERCPU_GENERIC_IRQ */

#define __percpu_seg_override
#define __my_cpu_var(var) (*raw_cpu_ptr(&(var)))
#define __percpu_arg(name) "%" #name

#define this_cpu_read_stable(pcp) \
	({ \
		TYPEOF_UNQUAL(pcp) stable_value__ = \
			READ_ONCE(*raw_cpu_ptr(&(pcp))); \
		stable_value__; \
	})
#define this_cpu_read_const(pcp) this_cpu_read_stable(pcp)
#define raw_cpu_read_long(pcp) raw_cpu_read_8(pcp)
#define x86_this_cpu_test_bit(nr, var) \
	test_bit((nr), (unsigned long *)raw_cpu_ptr(&(var)))

#define DEFINE_EARLY_PER_CPU(type, name, initial) \
	DEFINE_PER_CPU(type, name) = initial; \
	__typeof__(type) name##_early_map[NR_CPUS] __initdata = \
		{ [0 ... NR_CPUS - 1] = initial }; \
	__typeof__(type) *name##_early_ptr __refdata = name##_early_map
#define DEFINE_EARLY_PER_CPU_READ_MOSTLY(type, name, initial) \
	DEFINE_PER_CPU_READ_MOSTLY(type, name) = initial; \
	__typeof__(type) name##_early_map[NR_CPUS] __initdata = \
		{ [0 ... NR_CPUS - 1] = initial }; \
	__typeof__(type) *name##_early_ptr __refdata = name##_early_map
#define EXPORT_EARLY_PER_CPU_SYMBOL(name) EXPORT_PER_CPU_SYMBOL(name)
#define DECLARE_EARLY_PER_CPU(type, name) \
	DECLARE_PER_CPU(type, name); \
	extern __typeof__(type) *name##_early_ptr; \
	extern __typeof__(type) name##_early_map[]
#define DECLARE_EARLY_PER_CPU_READ_MOSTLY(type, name) \
	DECLARE_PER_CPU_READ_MOSTLY(type, name); \
	extern __typeof__(type) *name##_early_ptr; \
	extern __typeof__(type) name##_early_map[]
#define early_per_cpu_ptr(name) (name##_early_ptr)
#define early_per_cpu_map(name, cpu) (name##_early_map[cpu])
#define early_per_cpu(name, cpu) \
	*(early_per_cpu_ptr(name) ? &early_per_cpu_ptr(name)[cpu] : \
		&per_cpu(name, cpu))

#endif /* _ASM_X86_PERCPU_H */
#endif /* __ASSEMBLY__ */

#endif /* KOBOX_PROVIDER_PERCPU_H */
