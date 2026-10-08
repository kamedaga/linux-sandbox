/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TRAPS_COMPILE_H
#define KOBOX_TRAPS_COMPILE_H

#include <linux/kernel.h>
#include <asm/processor.h>

/* traps.o alone uses TASK_SIZE_MAX twice, solely to reject user instruction
 * addresses in decode_bug/is_valid_bugaddr. Hosted core/module text lives at
 * positive native VAs, not above Linux's hardware user/kernel split. Keep the
 * upstream decoders and WARN policy, but use registered kernel text as their
 * admission boundary. No client-MM address limit changes. The build checks
 * these two uses so an upstream change cannot silently broaden this overlay.
 */
static __always_inline unsigned long kobox_trap_address_limit(unsigned long address)
{
	return kernel_text_address(address) ? 0UL : ~0UL;
}

#undef TASK_SIZE_MAX
#define TASK_SIZE_MAX kobox_trap_address_limit(addr)

#endif
