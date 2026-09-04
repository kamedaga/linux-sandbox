/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KOBOX_ABI_TYPES_H
#define KOBOX_ABI_TYPES_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 kobox_abi_u8;
typedef u16 kobox_abi_u16;
typedef u32 kobox_abi_u32;
typedef u64 kobox_abi_u64;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t kobox_abi_u8;
typedef uint16_t kobox_abi_u16;
typedef uint32_t kobox_abi_u32;
typedef uint64_t kobox_abi_u64;
#endif

#endif
