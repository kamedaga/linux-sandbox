/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_ASM_FUTEX_H
#define KOBOX_ASM_FUTEX_H

#ifdef KOBOX_BOOT_RUNTIME
#define arch_futex_atomic_op_inuser kobox_native_futex_op
#define futex_atomic_cmpxchg_inatomic kobox_native_futex_cmpxchg
#endif
#include_next <asm/futex.h>

#ifdef KOBOX_BOOT_RUNTIME
#undef arch_futex_atomic_op_inuser
#undef futex_atomic_cmpxchg_inatomic

int kobox_futex_op(int operation, int operand, int *old, u32 __user *address);
int kobox_user_cmpxchg(void __user *address, unsigned int size,
		       u64 expected, u64 value, u64 *observed);

static inline int arch_futex_atomic_op_inuser(int operation, int operand,
					     int *old, u32 __user *address)
{
	return kobox_futex_op(operation, operand, old, address);
}

static inline int futex_atomic_cmpxchg_inatomic(u32 *old, u32 __user *address,
					       u32 expected, u32 value)
{
	u64 observed;
	int result = kobox_user_cmpxchg(address, sizeof(*address), expected,
				      value, &observed);

	if (!result)
		*old = observed;
	return result;
}
#endif
#endif
