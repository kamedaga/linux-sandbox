/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_LIFECYCLE_H
#define KOBOX_BOOT_LIFECYCLE_H

#include "../task/host.h"

/* Process-local event port. The owner keeps callbacks/context alive until
 * process exit, including after module unload. ready/pending may not block,
 * allocate, or reenter hosted Linux. pending() runs in hardirq context and
 * returns zero while idle, one for stop, two for work, or a negative error.
 * The host validates authority/generation before publishing a request.
 */
struct kobox_linux_lifecycle {
	size_t size;
	void *context;
	int (*ready)(void *context);
	int (*pending)(void *context);
	/* Optional service callback, unlike ready/pending: runs in the opening
	 * Linux task and may call typed Linux APIs. It claims one published work
	 * item, publishes its completion, then returns zero. Negative values are
	 * infrastructure failures, not ioctl results. service is a borrowed
	 * process-local resource, never a peer pointer. No call after loop exit.
	 */
	int (*dispatch)(void *context, void *service);
	/* Optional asynchronous service, mutually exclusive with dispatch/poll.
	 * All three run on the opening Linux task and may call typed Linux APIs.
	 * start creates persistent tasks before READY; advance admits private jobs
	 * and collects their completions, returning 0 idle or 2 progress. Worker
	 * completions may wake the opening task directly through Linux scheduling.
	 * finish joins/drains before the launch owner destroys mounts or modules.
	 * Native transport retirement must precede pending's terminal publication.
	 */
	int (*start)(void *context, void *service);
	int (*advance)(void *context, void *service);
	int (*finish)(void *context, void *service);
	/* Optional pair, both called only by the opening task. poll may prepare
	 * work without blocking and returns the same states as pending. idle
	 * relinquishes polling ownership before the task sleeps. pending must
	 * remain IRQ-safe; neither callback may wait for another Linux task.
	 */
	int (*poll)(void *context);
	void (*idle)(void *context);
	/* Optional opening-task inspection monitor. It runs only while the
	 * lifecycle is idle, at most once per second, and may use Linux APIs.
	 * A negative result ends the one-shot generation through normal cleanup.
	 * It must not be used as the stop notification from hardirq context.
	 */
	int (*monitor)(void *context);
	void *monitor_context;
	/* An inspection abort must also release the native receiver before the
	 * opening task unloads modules. The host handles this as a terminal local
	 * error; it does not impersonate a peer QUIESCE request.
	 */
	int (*abort)(void *context, int error);
};

#ifdef __KERNEL__
int kobox_linux_lifecycle_wait(const struct kobox_linux_lifecycle *host);
int kobox_linux_lifecycle_serve(const struct kobox_linux_lifecycle *host,
			       void *service);
void kobox_linux_lifecycle_interrupt(void);
/* Optional executor for an asynchronous lifecycle, registered by start() on
 * the opening task. When advance() is idle the opening task calls run before
 * it sleeps; run executes at most one accepted item through
 * kobox_linux_lifecycle_execute and returns 1, or 0 with nothing to run.
 * Its completion must be collected by a later advance(), not by run.
 */
int kobox_linux_lifecycle_executor(int (*run)(void *context), void *context);
/* Execute work that may sleep. On the opening task of an executing lifecycle
 * the advance() right is lent to a resident deputy if work sleeps or a
 * control event arrives, and is back when this returns. Elsewhere it is only
 * the same call with the same sleep detection. */
void kobox_linux_lifecycle_execute(void (*work)(void *argument), void *argument);
/* Whether current may act as owner's receiver: the opening task unless the
 * right is lent, the deputy while it is. Any other owner: current == owner. */
bool kobox_linux_lifecycle_receiving(struct task_struct *owner);
/* Wake owner's current receiver after publishing a completion for it. */
void kobox_linux_lifecycle_wake(struct task_struct *owner);
#ifdef KOBOX_RUNTIME_GATES
unsigned int kobox_linux_lifecycle_gate_returns(void);
#endif
#endif

#endif
