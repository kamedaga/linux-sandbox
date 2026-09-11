/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_RUNTIME_HOST_H
#define KOBOX_RUNTIME_HOST_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#endif

/* Process-local loader contract. The module cookie and offset come from the
 * selected architecture's TLS relocations, never from a client or the wire.
 * Bind once before core execution; context must outlive every core thread.
 * The backend returns this thread's storage, with the ELF TLS initializer and
 * alignment preserved. No guest service may be entered by this callback.
 */
struct kobox_runtime_host {
	size_t size;
	void *context;
	void *(*tls_address)(void *context, unsigned long module,
			     unsigned long offset);
};

int kobox_linux_runtime_bind(const struct kobox_runtime_host *host);
void *kobox_runtime_tls_address(unsigned long module, unsigned long offset);

#endif
