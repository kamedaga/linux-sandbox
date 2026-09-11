/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_64_TLS_H
#define KOBOX_X86_64_TLS_H

/* x86-64 ELF general-dynamic TLS argument, not a libc DTV layout. */
struct kobox_x86_tls_index {
	unsigned long module;
	unsigned long offset;
};

#endif
