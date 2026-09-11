// SPDX-License-Identifier: GPL-2.0-only
#include "host.h"

#include <linux/errno.h>

/* Published by the loader before it starts any core threads. */
static struct kobox_runtime_host runtime_host;

int kobox_linux_runtime_bind(const struct kobox_runtime_host *host)
{
	if (!host || host->size != sizeof(*host) || !host->tls_address)
		return -EINVAL;
	if (runtime_host.tls_address)
		return -EBUSY;
	runtime_host = *host;
	return 0;
}

void *kobox_runtime_tls_address(unsigned long module, unsigned long offset)
{
	void *address;

	/* A missing binding cannot be reported through errno: even reading
	 * current requires TLS. Do not return an alias of another task's state.
	 */
	if (!runtime_host.tls_address)
		__builtin_trap();
	address = runtime_host.tls_address(runtime_host.context, module, offset);
	if (!address)
		__builtin_trap();
	return address;
}
