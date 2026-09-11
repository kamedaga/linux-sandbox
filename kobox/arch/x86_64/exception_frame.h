/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_EXCEPTION_FRAME_H
#define KOBOX_X86_EXCEPTION_FRAME_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Architectural registers, not a Linux pt_regs layout or a wire format. */
enum kobox_linux_exception_register {
	KOBOX_EXCEPTION_AX,
	KOBOX_EXCEPTION_BX,
	KOBOX_EXCEPTION_CX,
	KOBOX_EXCEPTION_DX,
	KOBOX_EXCEPTION_SI,
	KOBOX_EXCEPTION_DI,
	KOBOX_EXCEPTION_BP,
	KOBOX_EXCEPTION_R8,
	KOBOX_EXCEPTION_R9,
	KOBOX_EXCEPTION_R10,
	KOBOX_EXCEPTION_R11,
	KOBOX_EXCEPTION_R12,
	KOBOX_EXCEPTION_R13,
	KOBOX_EXCEPTION_R14,
	KOBOX_EXCEPTION_R15,
	KOBOX_EXCEPTION_REGISTERS,
};

struct kobox_linux_exception_frame {
	size_t size;
	uint64_t registers[KOBOX_EXCEPTION_REGISTERS];
	uint64_t ip;
	uint64_t sp;
	uint64_t flags;
	uint64_t fault_address;
	uint64_t error_code;
	uint32_t vector;
	uint32_t cpu;
};

#endif
