/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_ASM_CPUFEATURE_H
#define KOBOX_PROVIDER_ASM_CPUFEATURE_H

#include_next <asm/cpufeature.h>

#undef static_cpu_has
#define static_cpu_has(bit) boot_cpu_has(bit)

#endif /* KOBOX_PROVIDER_ASM_CPUFEATURE_H */
