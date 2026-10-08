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
static struct task_struct *opening_task;

static int lifecycle_monitor_once(void)
{
	int result = lifecycle.monitor(lifecycle.monitor_context);

	if (result) {
		int aborted = kobox_host_call(
			lifecycle.abort(lifecycle.context, result));

		return aborted ? aborted : result;
	}
	return 0;
}

void kobox_linux_lifecycle_interrupt(void)
{
	if (smp_load_acquire(&armed) &&
	    kobox_host_call(lifecycle.pending(lifecycle.context))) {
		if (lifecycle.advance)
			wake_up_process(opening_task);
		else
			complete(&stop_requested);
	}
}

static int lifecycle_async_serve(void *service)
{
	int result, advanced, finished;

	for (;;) {
		result = kobox_host_call(lifecycle.pending(lifecycle.context));
		if (result == 1 || result < 0)
			break;
		if (result != 0 && result != 2) {
			result = -EPROTO;
			break;
		}
		advanced = kobox_host_call(lifecycle.advance(lifecycle.context, service));
		if (advanced < 0 || (advanced != 0 && advanced != 2)) {
			result = advanced < 0 ? advanced : -EPROTO;
			break;
		}
		if (advanced) {
			cond_resched();
			continue;
		}
		/* A worker wake is not a native control-event completion. Arm the
		 * actual task state, then recheck both authoritative sources before
		 * scheduling. A wake between recheck and schedule leaves us runnable.
		 */
		set_current_state(TASK_INTERRUPTIBLE);
		result = kobox_host_call(lifecycle.pending(lifecycle.context));
		advanced = result ? 0 :
			kobox_host_call(lifecycle.advance(lifecycle.context, service));
		if (!result && !advanced)
			schedule();
		__set_current_state(TASK_RUNNING);
		if (advanced < 0 || (advanced != 0 && advanced != 2)) {
			result = advanced < 0 ? advanced : -EPROTO;
			break;
		}
	}
	finished = kobox_host_call(lifecycle.finish(lifecycle.context, service));
	return finished ? (finished < 0 ? finished : -EPROTO) : result == 1 ? 0 : result;
}

int kobox_linux_lifecycle_serve(const struct kobox_linux_lifecycle *host,
			       void *service)
{
	int result;
	u64 poll_until = 0;
	u64 monitor_after = 0;

	/* One launch owner in PID 1; there is no in-process generation reuse. */
	if (!host || host->size != sizeof(*host) || !host->context ||
	    !host->ready || !host->pending || !!host->poll != !!host->idle ||
	    !!host->start != !!host->advance || !!host->start != !!host->finish ||
	    (host->advance && (host->dispatch || host->poll || host->monitor)) ||
	    !!host->monitor != !!host->monitor_context ||
	    !!host->monitor != !!host->abort)
		return -EINVAL;
	if (smp_load_acquire(&armed))
		return -EBUSY;
	lifecycle = *host;
	opening_task = current;
	if (lifecycle.start) {
		result = kobox_host_call(lifecycle.start(lifecycle.context, service));
		if (result)
			return result < 0 ? result : -EPROTO;
	}
	smp_store_release(&armed, true);
	result = kobox_host_call(lifecycle.ready(lifecycle.context));
	if (result) {
		if (lifecycle.finish)
			kobox_host_call(lifecycle.finish(lifecycle.context, service));
		return result < 0 ? result : -EPROTO;
	}
	if (lifecycle.advance)
		return lifecycle_async_serve(service);
	if (lifecycle.monitor)
		monitor_after = ktime_get_mono_fast_ns() + NSEC_PER_SEC;
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
					result = lifecycle_monitor_once();
					if (result)
						return result;
					monitor_after = ktime_get_mono_fast_ns() +
						NSEC_PER_SEC;
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
		/* An active DRM client can keep requests pending continuously.
		 * Sampling only while idle silently disables inspection limits. */
		if (lifecycle.monitor &&
		    ktime_get_mono_fast_ns() >= monitor_after) {
			result = lifecycle_monitor_once();
			if (result)
				return result;
			monitor_after = ktime_get_mono_fast_ns() +
				NSEC_PER_SEC;
		}
		cond_resched();
		poll_until = ktime_get_mono_fast_ns() + 150 * NSEC_PER_USEC;
	}
}

int kobox_linux_lifecycle_wait(const struct kobox_linux_lifecycle *host)
{
	return kobox_linux_lifecycle_serve(host, NULL);
}
