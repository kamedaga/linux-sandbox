// SPDX-License-Identifier: GPL-2.0-only
#include "host.h"

#include <linux/errno.h>

/* Published by the loader before it starts any core threads. */
static struct kobox_runtime_host runtime_host;

int kobox_linux_runtime_bind(const struct kobox_runtime_host *host)
{
	if (!host || host->size != sizeof(*host) || !host->thread_state)
		return -EINVAL;
	if (runtime_host.thread_state)
		return -EBUSY;
	runtime_host = *host;
	return 0;
}

struct kobox_runtime_thread_state *kobox_runtime_thread_state(void)
{
	struct kobox_runtime_thread_state *state;

	if (!runtime_host.thread_state)
		__builtin_trap();
	state = runtime_host.thread_state(runtime_host.context);
	if (!state)
		__builtin_trap();
	return state;
}
