/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "linux_signal.h"

#include <ucontext.h>

static const int register_index[KOBOX_EXCEPTION_REGISTERS] = {
	[KOBOX_EXCEPTION_AX] = REG_RAX,
	[KOBOX_EXCEPTION_BX] = REG_RBX,
	[KOBOX_EXCEPTION_CX] = REG_RCX,
	[KOBOX_EXCEPTION_DX] = REG_RDX,
	[KOBOX_EXCEPTION_SI] = REG_RSI,
	[KOBOX_EXCEPTION_DI] = REG_RDI,
	[KOBOX_EXCEPTION_BP] = REG_RBP,
	[KOBOX_EXCEPTION_R8] = REG_R8,
	[KOBOX_EXCEPTION_R9] = REG_R9,
	[KOBOX_EXCEPTION_R10] = REG_R10,
	[KOBOX_EXCEPTION_R11] = REG_R11,
	[KOBOX_EXCEPTION_R12] = REG_R12,
	[KOBOX_EXCEPTION_R13] = REG_R13,
	[KOBOX_EXCEPTION_R14] = REG_R14,
	[KOBOX_EXCEPTION_R15] = REG_R15,
};

void kobox_x86_linux_exception_import(struct kobox_linux_exception_frame *frame,
				     void *native_context, uintptr_t fault_address)
{
	ucontext_t *context = native_context;
	const greg_t *registers = context->uc_mcontext.gregs;
	unsigned int index;

	for (index = 0; index < KOBOX_EXCEPTION_REGISTERS; index++)
		frame->registers[index] = registers[register_index[index]];
	frame->ip = registers[REG_RIP];
	frame->sp = registers[REG_RSP];
	frame->flags = registers[REG_EFL];
	frame->vector = registers[REG_TRAPNO];
	frame->error_code = registers[REG_ERR];
	frame->fault_address = fault_address;
}

void kobox_x86_linux_exception_export(
	const struct kobox_linux_exception_frame *frame, void *native_context)
{
	ucontext_t *context = native_context;
	greg_t *registers = context->uc_mcontext.gregs;
	unsigned int index;

	for (index = 0; index < KOBOX_EXCEPTION_REGISTERS; index++)
		registers[register_index[index]] = frame->registers[index];
	registers[REG_RIP] = frame->ip;
	registers[REG_RSP] = frame->sp;
	/* The virtual Linux IRQ mask must never become a host RFLAGS.IF. */
	registers[REG_EFL] = (frame->flags & ~UINT64_C(0x200)) |
		(registers[REG_EFL] & UINT64_C(0x200));
}
