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

extern __thread unsigned long kobox_provider_percpu_offset;

#define __my_cpu_offset kobox_provider_percpu_offset
#include <asm-generic/percpu.h>

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
