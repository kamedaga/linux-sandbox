// SPDX-License-Identifier: GPL-2.0-only

#include "lifecycle.h"
#include "../arch/x86_64/host_call.h"

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/jiffies.h>

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
	u64 poll_until = 0;

	/* One launch owner in PID 1; there is no in-process generation reuse. */
	if (!host || host->size != sizeof(*host) || !host->context ||
	    !host->ready || !host->pending || !!host->poll != !!host->idle ||
	    !!host->monitor != !!host->monitor_context ||
	    !!host->monitor != !!host->abort)
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
		result = lifecycle.poll ?
			kobox_host_call(lifecycle.poll(lifecycle.context)) :
			kobox_host_call(lifecycle.pending(lifecycle.context));
		if (!result) {
			/* A synchronous peer may publish its next request just after
			 * the previous response. Keep the opening task available for
			 * a bounded burst instead of switching through idle for each
			 * request. IRQs remain enabled; scheduler work ends the poll.
			 */
			if (poll_until && !need_resched() &&
			    ktime_get_mono_fast_ns() < poll_until) {
				cpu_relax();
				continue;
			}
			poll_until = 0;
			if (lifecycle.idle)
				kobox_host_call((lifecycle.idle(lifecycle.context), 0));
			if (lifecycle.monitor) {
				if (!wait_for_completion_timeout(&stop_requested, HZ)) {
					result = lifecycle.monitor(lifecycle.monitor_context);
					if (result) {
						int aborted = kobox_host_call(
							lifecycle.abort(lifecycle.context, result));
						if (aborted)
							return aborted;
						return result;
					}
				}
			} else {
				wait_for_completion(&stop_requested);
			}
			continue;
		}
		if (result == 1)
			return 0;
		if (result < 0)
			return result;
		if (result != 2 || !lifecycle.dispatch)
			return -EPROTO;
		/* Consume a wake already observed by polling. Never reset the
		 * completion: a concurrent notification must remain pending.
		 */
		try_wait_for_completion(&stop_requested);
		result = kobox_host_call(lifecycle.dispatch(lifecycle.context, service));
		if (result)
			return result < 0 ? result : -EPROTO;
		cond_resched();
		poll_until = ktime_get_mono_fast_ns() + 150 * NSEC_PER_USEC;
	}
}

int kobox_linux_lifecycle_wait(const struct kobox_linux_lifecycle *host)
{
	return kobox_linux_lifecycle_serve(host, NULL);
}
