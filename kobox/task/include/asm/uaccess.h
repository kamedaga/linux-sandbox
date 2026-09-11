/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_ASM_UACCESS_H
#define KOBOX_ASM_UACCESS_H

#ifdef KOBOX_BOOT_RUNTIME
#define __clear_user kobox_native_clear_user
#endif
#include_next <asm/uaccess.h>

#ifdef KOBOX_BOOT_RUNTIME
#undef __clear_user

unsigned long kobox_raw_copy_from_user(void *to, const void __user *from,
				       unsigned long size);
unsigned long kobox_raw_copy_to_user(void __user *to, const void *from,
				     unsigned long size);
unsigned long kobox_clear_user(void __user *to, unsigned long size);
unsigned long kobox_read_user_nontemporal(void *to, const void __user *from,
					unsigned long size);
unsigned long kobox_read_user_flushcache(void *to, const void __user *from,
				       unsigned long size);
int kobox_user_cmpxchg(void __user *address, unsigned int size,
		       u64 expected, u64 value, u64 *observed);

/* Retain native exception-table helpers for kernel probes and trusted boot
 * inputs. Client addresses are translated independently for every access;
 * there is no process-wide SMAP window to open or save across a schedule.
 */
#define raw_copy_from_user kobox_raw_copy_from_user
#define raw_copy_to_user kobox_raw_copy_to_user
#define __clear_user kobox_clear_user
#define clear_user kobox_clear_user
#define copy_from_user_inatomic_nontemporal kobox_read_user_nontemporal
#define copy_from_user_flushcache kobox_read_user_flushcache

#undef masked_user_access_begin
#define masked_user_access_begin(ptr) mask_user_address(ptr)
#undef user_access_begin
#undef user_access_end
#undef user_access_save
#undef user_access_restore
#define user_access_begin(ptr, size) access_ok(ptr, size)
#define user_access_end() do { } while (0)
#define user_access_save() 0UL
#define user_access_restore(flags) do { (void)(flags); } while (0)

#undef __get_user
#undef get_user
#undef __put_user
#undef put_user
#define __get_user(value, pointer) ({ \
	__inttype(*(pointer)) __gu_value = 0; \
	int __gu_result; \
	__chk_user_ptr(pointer); \
	__gu_result = kobox_raw_copy_from_user(&__gu_value, (pointer), \
					      sizeof(*(pointer))) ? -EFAULT : 0; \
	(value) = (__force __typeof__(*(pointer)))(__gu_result ? 0 : __gu_value); \
	instrument_get_user(__gu_value); \
	__gu_result; \
})
#define get_user(value, pointer) ({ \
	might_fault(); \
	__get_user(value, pointer); \
})
#define __put_user(value, pointer) ({ \
	__typeof__(*(pointer)) __pu_value = (__force __typeof__(*(pointer)))(value); \
	__chk_user_ptr(pointer); \
	kobox_raw_copy_to_user((pointer), &__pu_value, \
			       sizeof(*(pointer))) ? -EFAULT : 0; \
})
#define put_user(value, pointer) ({ \
	might_fault(); \
	__put_user(value, pointer); \
})
#undef unsafe_get_user
#undef unsafe_put_user
#undef unsafe_copy_to_user
#define unsafe_get_user(value, pointer, label) do { \
	if (__get_user(value, pointer)) \
		goto label; \
} while (0)
#define unsafe_put_user(value, pointer, label) do { \
	if (__put_user(value, pointer)) \
		goto label; \
} while (0)
#define unsafe_copy_to_user(to, from, size, label) do { \
	if (kobox_raw_copy_to_user(to, from, size)) \
		goto label; \
} while (0)

#undef unsafe_try_cmpxchg_user
#define unsafe_try_cmpxchg_user(pointer, old_pointer, value, label) ({ \
	__auto_type __cu_pointer = (pointer); \
	__auto_type __cu_old_pointer = (old_pointer); \
	u64 __cu_expected = *__cu_old_pointer; \
	u64 __cu_observed; \
	if (kobox_user_cmpxchg(__cu_pointer, sizeof(*__cu_pointer), \
			       __cu_expected, (value), &__cu_observed)) \
		goto label; \
	*__cu_old_pointer = __cu_observed; \
	__cu_observed == __cu_expected; \
})

#endif /* KOBOX_BOOT_RUNTIME */
#endif
