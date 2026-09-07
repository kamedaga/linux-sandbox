# POSIX host boundary

This directory is the Linux PoC implementation of the lowest Kobox host
boundary. It provides only:

- pthread creation and join;
- a monotonic-clock counting permit;
- `CLOCK_MONOTONIC` and boot wall-clock time;
- a rearmable one-shot timer;
- anonymous mapping plus shared backing and fixed alias windows; and
- asynchronous tick/IRQ notification with POSIX real-time signals.

It does not implement Linux tasks, scheduling classes, waitqueues, mutexes,
workqueues, timers, or IRQ subsystems.

Each logical CPU has an execution token. A thread must enter that domain before
running Linux code. A switch transfers the token to the explicitly supplied
next task and parks the previous pthread; the host never chooses a task.
This serializes entry to one
logical CPU while leaving different CPU domains independent. Tick and IRQ
notification is directed to the current owner with `pthread_kill`, so a
CPU-bound owner is interruptible. Notification counts are retained when no
owner exists or local IRQ delivery is disabled. The final IRQ enable drains
the pending counts before returning.
The callback enters and returns with logical IRQs masked. The host restores
the interrupted IRQ state on return, like a machine interrupt-return boundary.
If Linux enables IRQs during softirq/callback execution, both already-pending
and later interrupts can nest immediately. Native signals are masked only for
host bookkeeping, not across the whole Linux notification callback. On return
the dispatcher rechecks CPU ownership, since Linux may have migrated the task.
This corrects the unfrozen notification contract; callback signatures and wire
layouts are unchanged. A blanket callback reentry guard would break Linux's
IRQ-enable semantics and prevent timers from interrupting an RCU callback.
Idle wait checks both the notification sequence and pending counts under the
ownership lock: observing a new sequence before signal delivery cannot lose
the wakeup.

Notification callbacks execute in signal context. Calls to non-async-signal-safe
host services must mask both reserved notification signals, including allocation,
mapping, thread creation, and join. CPU ownership locks are also masked. A parked
thread has no active CPU, so a late signal cannot reenter Linux there. This is
the signal-masking discipline described in
[POSIX sigaction](https://pubs.opengroup.org/onlinepubs/9799919799/functions/sigaction.html);
arbitrary libc calls from Linux or notification callbacks are not supported.
The notification sender retains the ownership lock through `pthread_kill` so
the target cannot exit and be joined between lookup and delivery. CPU objects
are destroyed only after producers and all possible owner threads have stopped.

The one-shot timer rechecks the current programmed deadline after a timed wait
expires: cancel or rearm can win before the timer thread reacquires its lock.
Cancel does not join a callback already in flight; destroy joins the producer
and all its callbacks. The Linux clockevent device tolerates stale notifications
by checking its current armed state and deadline in the target CPU domain.

`kobox2.posix_host_gate` exercises two logical CPUs and proves serialization,
cross-CPU concurrency, CPU-bound tick delivery, IRQ deferral,
wake-before-park, pending-before-idle-wait, and native task handoff/exit/join.
It also checks nested IRQ entry/return on both CPUs: a queued IRQ must run
before enable returns, and a later IRQ must run before its parent callback ends.
It also verifies that two windows alias the same shared backing.
Timer tests cover cancel followed by reuse and reprogramming in both directions.
`kobox2.posix_host_surface_gate` rejects undeclared imports
and Linux-specific `futex`, `eventfd`, and `timerfd` shortcuts.

All state objects must be zero-initialized before their init/start call and
must not be copied while active.

The separately linked `kobox_posix_vm` / `vm*.c` address-space transport is
**Linux-specific**, not part of that restricted POSIX archive. It uses ptrace
and eventfd/signalfd to control real external processes. See the
[MM port contract](../../mm/README.md).
VM completion notifications share the same IRQ masking/CPU-domain rules; the
host tests verify their deferred delivery on both CPUs.
