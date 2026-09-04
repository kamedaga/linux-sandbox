# POSIX host boundary

This directory is the Linux PoC implementation of the lowest Kobox host
boundary. It provides only:

- pthread creation and join;
- a monotonic-clock counting permit;
- `CLOCK_MONOTONIC` time;
- a rearmable one-shot timer;
- anonymous mapping plus shared backing and fixed alias windows; and
- asynchronous tick/IRQ notification with POSIX real-time signals.

It does not implement Linux tasks, scheduling classes, waitqueues, mutexes,
workqueues, timers, or IRQ subsystems.

Each logical CPU has an execution lock. A thread must enter that domain before
running Linux code and leave it before parking. This serializes entry to one
logical CPU while leaving different CPU domains independent. Tick and IRQ
notification is directed to the current owner with `pthread_kill`, so a
CPU-bound owner is interruptible. Notification counts are retained when no
owner exists or local IRQ delivery is disabled. The final IRQ enable drains
the pending counts before returning.

Notification callbacks execute in signal context and therefore may use only
async-signal-safe operations. CPU objects are destroyed only after producers
and all possible owner threads have stopped.

`kobox2.posix_host_gate` exercises two logical CPUs and proves serialization,
cross-CPU concurrency, CPU-bound tick delivery, IRQ deferral, and
wake-before-park. It also verifies that two windows alias the same shared
backing. `kobox2.posix_host_surface_gate` rejects undeclared imports
and Linux-specific `futex`, `eventfd`, and `timerfd` shortcuts.

All state objects must be zero-initialized before their init/start call and
must not be copied while active.
