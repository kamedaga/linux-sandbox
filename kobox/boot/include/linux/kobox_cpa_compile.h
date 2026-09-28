/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_CPA_COMPILE_H
#define KOBOX_CPA_COMPILE_H

/* Preload the unchanged x86 definition, then route only set_memory.o's CPA
 * cache drain through the hosted machine. The native cache-policy syscall
 * has already completed its all-CPU WBINVD during TLB reconciliation.
 */
#include <asm/special_insns.h>
void kobox_cpa_wbinvd(void);
#define wbinvd() kobox_cpa_wbinvd()

#endif
