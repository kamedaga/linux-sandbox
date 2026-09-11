/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_VM_PROGRAM_H
#define KOBOX_VM_PROGRAM_H

#include <stdint.h>

/* Guest Linux ABI workloads, shared by native OS test frontends. The
 * frontend supplies syscall entry; it must not emulate any Linux syscall.
 */
uint64_t kobox_vm_program_syscall(uint64_t number, uint64_t a0, uint64_t a1,
	uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
uint64_t kobox_vm_stack_access(uint64_t address, uint64_t value);
_Noreturn void kobox_vm_child_program(uint64_t base);
_Noreturn void kobox_vm_root_program(uint64_t base, unsigned int cpu);

#endif
