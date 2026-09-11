// SPDX-License-Identifier: GPL-2.0-only

#include "lifecycle.h"

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/smp.h>

static DECLARE_COMPLETION(stop_requested);
static struct kobox_linux_lifecycle lifecycle;
static bool armed;

void kobox_linux_lifecycle_interrupt(void)
{
	if (smp_load_acquire(&armed) && lifecycle.pending(lifecycle.context))
		complete_all(&stop_requested);
}

int kobox_linux_lifecycle_wait(const struct kobox_linux_lifecycle *host)
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
	result = lifecycle.ready(lifecycle.context);
	if (result)
		return result < 0 ? result : -EPROTO;
	/* Also covers publication before ready/park; completion retains wake. */
	kobox_linux_lifecycle_interrupt();
	wait_for_completion(&stop_requested);
	result = lifecycle.pending(lifecycle.context);
	return result == 1 ? 0 : result < 0 ? result : -EPROTO;
}
