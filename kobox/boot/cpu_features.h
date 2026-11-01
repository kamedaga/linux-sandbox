/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_CPU_FEATURES_H
#define KOBOX_BOOT_CPU_FEATURES_H

/* CPUID leaf 1: EDX.CLFSH qualifies EBX's cache-line-size field. Never
 * infer support from a nonzero field or an unrelated optional ISA feature. */
static inline unsigned int kobox_cpuid_clflush_size(unsigned int edx,
						 unsigned int ebx,
						 unsigned int page_size)
{
	unsigned int line;
	if (!(edx & (1u << 19)))
		return 0;
	line = ((ebx >> 8) & 0xff) * 8;
	return line && line <= page_size && !(line & (line - 1)) ? line : 0;
}
#endif
