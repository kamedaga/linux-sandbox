// SPDX-License-Identifier: GPL-2.0-only

#include "lifecycle.h"
#include "../arch/x86_64/host_call.h"

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/smp.h>

static DECLARE_COMPLETION(stop_requested);
static struct kobox_linux_lifecycle lifecycle;
static bool armed;

void kobox_linux_lifecycle_interrupt(void)
{
	if (smp_load_acquire(&armed) &&
	    kobox_host_call(lifecycle.pending(lifecycle.context)))
		complete(&stop_requested);
}

int kobox_linux_lifecycle_serve(const struct kobox_linux_lifecycle *host,
			       void *service)
{
	int result;

	/* One launch owner in PID 1; there is no in-process generation reuse. */
	if (!host || host->size != sizeof(*host) || !host->context ||
	    !host->ready || !host->pending)
		return -EINVAL;
	if (smp_load_acquire(&armed))
		return -EBUSY;
	lifecycle = *host;
	smp_store_release(&armed, true);
	result = kobox_host_call(lifecycle.ready(lifecycle.context));
	if (result)
		return result < 0 ? result : -EPROTO;
	for (;;) {
		/* State is authoritative; notifications may repeat or precede wait.
		 * Never reinitialize completion after observing an idle state.
		 */
		result = kobox_host_call(lifecycle.pending(lifecycle.context));
		if (!result) {
			wait_for_completion(&stop_requested);
			continue;
		}
		if (result == 1)
			return 0;
		if (result < 0)
			return result;
		if (result != 2 || !lifecycle.dispatch)
			return -EPROTO;
		result = kobox_host_call(lifecycle.dispatch(lifecycle.context, service));
		if (result)
			return result < 0 ? result : -EPROTO;
	}
}

int kobox_linux_lifecycle_wait(const struct kobox_linux_lifecycle *host)
{
	return kobox_linux_lifecycle_serve(host, NULL);
}
