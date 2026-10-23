// SPDX-License-Identifier: GPL-2.0-only

#include "lifecycle.h"
#include "../arch/x86_64/host_call.h"

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/jiffies.h>

static DECLARE_COMPLETION(stop_requested);
static struct kobox_linux_lifecycle lifecycle;
static bool armed;
static struct task_struct *opening_task;

/* The right to call advance() for an asynchronous lifecycle. The opening task
 * holds it, except while it executes accepted work that sleeps or overlaps a
 * control event: then the deputy holds it, so a sleeping VFS call never stops
 * reception. Only these transitions exist:
 *
 *   OWNER -> EXECUTING             opening task starts one work item
 *   EXECUTING -> OWNER             it finished without lending (no wake)
 *   EXECUTING -> LENT_IDLE         it sleeps, or a control event arrives
 *   LENT_IDLE <-> LENT_ADVANCING   the deputy brackets each advance() call
 *   LENT_IDLE -> OWNER             work finished while the deputy was idle
 *   LENT_ADVANCING -> RETURNING    work finished during a deputy advance()
 *   RETURNING -> OWNER             that advance() returned; deputy wakes owner
 *
 * So exactly one task is inside advance() at a time, and native/pool state
 * owned by the receiver is handed over only between advance() calls.
 */
enum {
	RECEIVE_OWNER,
	RECEIVE_EXECUTING,
	RECEIVE_LENT_IDLE,
	RECEIVE_LENT_ADVANCING,
	RECEIVE_RETURNING,
};
static atomic_t receive_state = ATOMIC_INIT(RECEIVE_OWNER);
static struct task_struct *deputy_task;
static int (*executor_run)(void *context);
static void *executor_context;
/* First infrastructure failure seen by the deputy; the opening task ends the
 * lifecycle with it once it receives again, like its own advance() failure. */
static atomic_t deputy_error = ATOMIC_INIT(0);
#ifdef KOBOX_RUNTIME_GATES
static unsigned int gate_returns;

unsigned int kobox_linux_lifecycle_gate_returns(void)
{
	return gate_returns;
}
#endif

static bool receive_lent(int state)
{
	return state == RECEIVE_LENT_IDLE || state == RECEIVE_LENT_ADVANCING ||
	       state == RECEIVE_RETURNING;
}

/* Hardirq, schedule() or worker context: never sleeps. A wake that races a
 * transition reaches a task that rechecks the state, never a lost receiver:
 * every transition into a lent state wakes the deputy itself. New work lends
 * the right of an executing opening task; a completion does not, because the
 * opening task collects it after its own item anyway.
 */
static void lifecycle_wake_receiver(bool lend)
{
	int state = atomic_read(&receive_state);

	while (state == RECEIVE_EXECUTING) {
		if (!lend)
			return;
		state = atomic_cmpxchg(&receive_state, RECEIVE_EXECUTING,
				       RECEIVE_LENT_IDLE);
		if (state == RECEIVE_EXECUTING) {
			wake_up_process(deputy_task);
			return;
		}
	}
	if (receive_lent(state))
		wake_up_process(deputy_task);
	else if (state == RECEIVE_OWNER)
		wake_up_process(opening_task);
}

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
		if (!lifecycle.advance)
			complete(&stop_requested);
		else if (deputy_task)
			/* Work arriving while the opening task executes is received
			 * by the deputy rather than waiting for that item to end. */
			lifecycle_wake_receiver(true);
		else
			wake_up_process(opening_task);
	}
}

int kobox_linux_lifecycle_executor(int (*run)(void *context), void *context)
{
	/* Only an asynchronous start, on the opening task, before READY. */
	if (!run || current != opening_task || smp_load_acquire(&armed) ||
	    !lifecycle.advance || executor_run)
		return -EINVAL;
#ifndef CONFIG_BLOCK
	/* The executor lends on Linux's plug flush before schedule(). Profiles
	 * without block support have no such hook and must keep worker execution. */
	return -EOPNOTSUPP;
#endif
	executor_run = run;
	executor_context = context;
	return 0;
}

bool kobox_linux_lifecycle_receiving(struct task_struct *owner)
{
	int state;

	if (!deputy_task || owner != opening_task)
		return current == owner;
	state = atomic_read(&receive_state);
	return current == opening_task ? !receive_lent(state) :
	       current == deputy_task && receive_lent(state);
}

void kobox_linux_lifecycle_wake(struct task_struct *owner)
{
	/* Order the caller's completion publication before reading the state:
	 * whoever receives after a lend collects it even if this sees no lender. */
	smp_mb();
	if (deputy_task && owner == opening_task)
		lifecycle_wake_receiver(false);
	else
		wake_up_process(owner);
}

#ifdef CONFIG_BLOCK
/* Called by Linux from schedule() before the task sleeps with this plug
 * installed (sched_submit_work), and by a plug flush without a sleep (bio
 * polling), which is treated the same: either way it is no longer certain to
 * return to receiving soon. Runs with the task state already set: it may
 * only use atomics and wakeups. Linux unlinks the hook before calling it.
 */
struct lifecycle_execution {
	struct blk_plug plug;
	struct blk_plug_cb hook;
	bool flushed;
};

static void lifecycle_execution_unplug(struct blk_plug_cb *hook, bool from_schedule)
{
	struct lifecycle_execution *execution =
		container_of(hook, struct lifecycle_execution, hook);

	execution->flushed = true;
	if (deputy_task &&
	    atomic_cmpxchg(&receive_state, RECEIVE_EXECUTING,
			   RECEIVE_LENT_IDLE) == RECEIVE_EXECUTING)
		wake_up_process(deputy_task);
}

#endif

void kobox_linux_lifecycle_execute(void (*work)(void *argument), void *argument)
{
#ifndef CONFIG_BLOCK
	/* No executor can be registered here, so no receiving right can be lent. */
	work(argument);
#else
	struct lifecycle_execution execution = { .flushed = false };
	bool receiver = deputy_task && current == opening_task;
	int state;

	if (receiver)
		WARN_ON_ONCE(atomic_xchg(&receive_state, RECEIVE_EXECUTING) !=
			     RECEIVE_OWNER);
	/* A plug already installed by the caller stays installed; the hook then
	 * joins it and is unlinked below before this frame returns. */
	blk_start_plug(&execution.plug);
	execution.hook.callback = lifecycle_execution_unplug;
	execution.hook.data = NULL;
	list_add(&execution.hook.list, &current->plug->cb_list);
	work(argument);
	/* Only current flushes its plug, so an unfired hook is still linked. */
	if (!execution.flushed)
		list_del(&execution.hook.list);
	blk_finish_plug(&execution.plug);
	if (!receiver)
		return;
	state = atomic_cmpxchg(&receive_state, RECEIVE_EXECUTING, RECEIVE_OWNER);
	if (state == RECEIVE_EXECUTING)
		return;
	/* Lent: an idle deputy is not inside advance(), take the right back now.
	 * Otherwise let its advance() finish; it hands the right back. */
	if (atomic_cmpxchg(&receive_state, RECEIVE_LENT_IDLE, RECEIVE_OWNER) ==
	    RECEIVE_LENT_IDLE)
		return;
	WARN_ON_ONCE(atomic_cmpxchg(&receive_state, RECEIVE_LENT_ADVANCING,
				    RECEIVE_RETURNING) != RECEIVE_LENT_ADVANCING);
#ifdef KOBOX_RUNTIME_GATES
	gate_returns++;
#endif
	for (;;) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (atomic_read_acquire(&receive_state) == RECEIVE_OWNER)
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
#endif
}

/* One deputy advance() while the right is lent. Returns its result, or
 * -EAGAIN without calling anything when the right is not lent. */
static int deputy_advance(void *service)
{
	int result, advanced = 0;

	if (atomic_cmpxchg(&receive_state, RECEIVE_LENT_IDLE,
			   RECEIVE_LENT_ADVANCING) != RECEIVE_LENT_IDLE)
		return -EAGAIN;
	/* Stop and errors stay with the opening task: a stop cannot complete
	 * while its executed item is still accepted work. */
	result = kobox_host_call(lifecycle.pending(lifecycle.context));
	if (result == 0 || result == 2) {
		advanced = kobox_host_call(lifecycle.advance(lifecycle.context, service));
		if (advanced < 0 || (advanced != 0 && advanced != 2)) {
			atomic_cmpxchg(&deputy_error, 0, advanced < 0 ? advanced : -EPROTO);
			advanced = 0;
		}
	}
	if (atomic_cmpxchg(&receive_state, RECEIVE_LENT_ADVANCING,
			   RECEIVE_LENT_IDLE) != RECEIVE_LENT_ADVANCING) {
		/* RETURNING: the opening task waits for exactly this. */
		atomic_set_release(&receive_state, RECEIVE_OWNER);
		wake_up_process(opening_task);
	}
	return advanced;
}

static int lifecycle_deputy(void *service)
{
	for (;;) {
		if (deputy_advance(service) == 2) {
			cond_resched();
			continue;
		}
		/* Same arm-then-recheck as the opening task: a lend, control
		 * event or completion wake after this state cannot be lost. */
		set_current_state(TASK_IDLE);
		if (kthread_should_stop())
			break;
		if (deputy_advance(service) != 2)
			schedule();
		__set_current_state(TASK_RUNNING);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void lifecycle_deputy_stop(void)
{
	if (!deputy_task)
		return;
	WARN_ON_ONCE(atomic_read(&receive_state) != RECEIVE_OWNER);
	kthread_stop_put(deputy_task);
	deputy_task = NULL;
}

static int lifecycle_async_serve(void *service)
{
	int result, advanced = 0, ran, finished;
	bool advance_due = true;

	for (;;) {
		result = kobox_host_call(lifecycle.pending(lifecycle.context));
		if (result == 1 || result < 0)
			break;
		if (result != 0 && result != 2) {
			result = -EPROTO;
			break;
		}
		if (advance_due) {
			advanced = kobox_host_call(lifecycle.advance(lifecycle.context, service));
			if (advanced < 0 || (advanced != 0 && advanced != 2)) {
				result = advanced < 0 ? advanced : -EPROTO;
				break;
			}
		}
		advance_due = true;
		ran = 0;
		if (executor_run) {
			/* Accepted work runs right after the advance() that accepted
			 * it, here instead of on a woken worker, which would cost two
			 * task switches; while it sleeps the deputy receives. Another
			 * advance() first would only find the ring empty again: each
			 * such pass costs native receive syscalls that serialize with
			 * the client's SEND on the kernel lock, before the reply. Its
			 * result is collected by the next advance(). */
			ran = executor_run(executor_context);
			result = atomic_read(&deputy_error);
			if (ran < 0 || ran > 2 || result) {
				result = ran < 0 ? ran : result ? result : -EPROTO;
				break;
			}
		}
		/* More accepted work, or progress with nothing to run (control,
		 * retirement, or work given to workers): look again before sleeping.
		 * After a lone executed item the recheck below is the next pass: its
		 * advance() collects that item and accepts whatever arrived since. */
		if (ran == 2 || (!ran && advanced)) {
			cond_resched();
			continue;
		}
		if (ran)
			cond_resched();
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
		/* What this recheck accepted runs next, without another pass. */
		advance_due = !advanced;
	}
	/* The right is the opening task's here; the deputy is idle or about to
	 * observe that and must be gone before finish() destroys its service. */
	lifecycle_deputy_stop();
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
	if (executor_run) {
		/* Created before READY, like the service's workers: lending the
		 * right never allocates or fails while a request is executing. */
		deputy_task = kthread_run(lifecycle_deputy, service, "lifecycle-deputy");
		if (IS_ERR(deputy_task)) {
			result = PTR_ERR(deputy_task);
			deputy_task = NULL;
			kobox_host_call(lifecycle.finish(lifecycle.context, service));
			return result;
		}
		get_task_struct(deputy_task);
	}
	smp_store_release(&armed, true);
	result = kobox_host_call(lifecycle.ready(lifecycle.context));
	if (result) {
		lifecycle_deputy_stop();
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
