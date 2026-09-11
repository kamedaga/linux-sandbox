/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DIAGNOSTIC_H
#define KOBOX_BOOT_DIAGNOSTIC_H

#include <linux/compiler_attributes.h>

/* Core-only failure reporting; the host receives formatted bytes, not types. */
__printf(1, 2) void kobox_linux_boot_diagnostic(const char *format, ...);

#endif
