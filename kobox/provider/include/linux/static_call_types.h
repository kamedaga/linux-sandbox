/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_LINUX_STATIC_CALL_TYPES_H
#define KOBOX_PROVIDER_LINUX_STATIC_CALL_TYPES_H

#ifdef KOBOX_BOOT_RUNTIME
#include_next <linux/static_call_types.h>
#else

/* Provider text is immutable; select Linux's indirect static-call form. */
#ifdef CONFIG_HAVE_STATIC_CALL_INLINE
#define KOBOX_RESTORE_HAVE_STATIC_CALL_INLINE
#undef CONFIG_HAVE_STATIC_CALL_INLINE
#endif
#ifdef CONFIG_HAVE_STATIC_CALL
#define KOBOX_RESTORE_HAVE_STATIC_CALL
#undef CONFIG_HAVE_STATIC_CALL
#endif

#include_next <linux/static_call_types.h>

#define static_call_mod(name) static_call(name)
#define __STATIC_CALL_MOD_ADDRESSABLE(name)

#ifdef KOBOX_RESTORE_HAVE_STATIC_CALL
#define CONFIG_HAVE_STATIC_CALL 1
#undef KOBOX_RESTORE_HAVE_STATIC_CALL
#endif
#ifdef KOBOX_RESTORE_HAVE_STATIC_CALL_INLINE
#define CONFIG_HAVE_STATIC_CALL_INLINE 1
#undef KOBOX_RESTORE_HAVE_STATIC_CALL_INLINE
#endif

#endif /* KOBOX_BOOT_RUNTIME */

#endif /* KOBOX_PROVIDER_LINUX_STATIC_CALL_TYPES_H */
