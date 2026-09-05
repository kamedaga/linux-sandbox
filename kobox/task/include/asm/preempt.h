/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_ASM_PREEMPT_H
#define KOBOX_TASK_ASM_PREEMPT_H

#include <asm/asm.h>

/*
 * Keep x86's count/NEED_RESCHED representation and upstream preemption entry.
 * A pthread can migrate between obtaining a per-CPU address and using it.
 * Mask the hosted interrupt during each architecture count operation, just
 * as the native single-instruction per-CPU operation cannot be interrupted.
 */
extern unsigned long kobox_provider_preempt_save(void);
extern void kobox_provider_preempt_restore(unsigned long flags);

#define preempt_count kobox_native_preempt_count
#define preempt_count_set kobox_native_preempt_count_set
#define set_preempt_need_resched kobox_native_set_preempt_need_resched
#define clear_preempt_need_resched kobox_native_clear_preempt_need_resched
#define test_preempt_need_resched kobox_native_test_preempt_need_resched
#define __preempt_count_add kobox_native_preempt_count_add
#define __preempt_count_sub kobox_native_preempt_count_sub
#define __preempt_count_dec_and_test kobox_native_preempt_count_dec_and_test
#define should_resched kobox_native_should_resched
#include_next <asm/preempt.h>
#undef preempt_count
#undef preempt_count_set
#undef set_preempt_need_resched
#undef clear_preempt_need_resched
#undef test_preempt_need_resched
#undef __preempt_count_add
#undef __preempt_count_sub
#undef __preempt_count_dec_and_test
#undef should_resched

#define KOBOX_PREEMPT_READ(type, name, args, expression) \
static __always_inline type name args \
{ \
	unsigned long flags = kobox_provider_preempt_save(); \
	type result = (expression); \
	kobox_provider_preempt_restore(flags); \
	return result; \
}

#define KOBOX_PREEMPT_WRITE(name, args, expression) \
static __always_inline void name args \
{ \
	unsigned long flags = kobox_provider_preempt_save(); \
	expression; \
	kobox_provider_preempt_restore(flags); \
}

KOBOX_PREEMPT_READ(int, preempt_count, (void), kobox_native_preempt_count())
KOBOX_PREEMPT_READ(bool, test_preempt_need_resched, (void),
		   kobox_native_test_preempt_need_resched())
KOBOX_PREEMPT_READ(bool, __preempt_count_dec_and_test, (void),
		   kobox_native_preempt_count_dec_and_test())
KOBOX_PREEMPT_READ(bool, should_resched, (int offset),
		   kobox_native_should_resched(offset))
KOBOX_PREEMPT_WRITE(preempt_count_set, (int count),
		    kobox_native_preempt_count_set(count))
KOBOX_PREEMPT_WRITE(set_preempt_need_resched, (void),
		    kobox_native_set_preempt_need_resched())
KOBOX_PREEMPT_WRITE(clear_preempt_need_resched, (void),
		    kobox_native_clear_preempt_need_resched())
KOBOX_PREEMPT_WRITE(__preempt_count_add, (int count),
		    kobox_native_preempt_count_add(count))
KOBOX_PREEMPT_WRITE(__preempt_count_sub, (int count),
		    kobox_native_preempt_count_sub(count))

#undef KOBOX_PREEMPT_READ
#undef KOBOX_PREEMPT_WRITE
#endif
