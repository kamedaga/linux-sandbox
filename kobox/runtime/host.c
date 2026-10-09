// SPDX-License-Identifier: GPL-2.0-only
#include "host.h"

#include <linux/errno.h>

/* Published by the loader before it starts any core threads. */
static struct kobox_runtime_host runtime_host;

int kobox_linux_runtime_bind(const struct kobox_runtime_host *host)
{
	if (!host || host->size != sizeof(*host) || !host->thread_state)
		return -EINVAL;
#ifdef KOBOX_PERCPU_SEGMENT
	if (!host->set_percpu_base)
		return -EINVAL;
#endif
	if (runtime_host.thread_state)
		return -EBUSY;
	runtime_host = *host;
	return 0;
}

void kobox_runtime_set_percpu_base(unsigned long base)
{
	if (!runtime_host.set_percpu_base)
		__builtin_trap();
	runtime_host.set_percpu_base(runtime_host.context, base);
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
