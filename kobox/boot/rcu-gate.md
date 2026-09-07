# Upstream Tree RCU / SRCU gate

Stage 5 runs `rcu_gate.c` after the boot/service/SMP/time gate in the same
boot-rooted `linux-boot-runtime.so`. Tree RCU, Tree SRCU, GP workers, callbacks,
barriers and task/context-tracking hooks are the pinned upstream code. The
gate neither starts services itself nor supplies quiescent states. The builder
rejects replacement definitions of upper RCU/SRCU APIs and scheduler hooks.

## Matrix and observations

The profile has two online CPUs, preemptible Tree RCU, Tree SRCU, periodic tick,
high-resolution timers and idle context tracking. Boot must have ended without
forcing normal-only or expedited-only RCU. Each row is run through both normal
and explicit expedited synchronization, with the reader initially on either CPU.

| Read-side case | Tree RCU | Dynamic SRCU | Static SRCU |
| --- | --- | --- | --- |
| Busy reader | Yes | Yes | Yes |
| Nested reader, inner unlock while outer stays held | Yes | Yes | Yes |
| Higher-priority task preempts the reader | Yes | Yes | Yes |
| Reader migrates; original CPU reaches RCU idle | Yes | Yes | Yes |
| Reader sleeps; its CPU reaches RCU idle | Invalid; excluded | Yes | Yes |

This produces 56 cases. Normal RCU readers never voluntarily sleep. SRCU uses
both `init_srcu_struct()` / `cleanup_srcu_struct()` and `DEFINE_STATIC_SRCU()`.
All test tasks use upstream kthread creation, affinity, scheduling and stop/join.

Each reader obtains three objects through `rcu_dereference()` or
`srcu_dereference()` and repeatedly checks their payloads. Only after the reader
is inside its critical section does the controller unpublish those pointers,
queue callbacks from both CPUs and start a synchronizing writer and a barrier.
While the reader remains held, no GP completion, callback, barrier return or
memory reclamation is allowed. The nested case checks this after inner unlock.

The preemption case uses an upstream FIFO task, observes the reader off-CPU and
an increased involuntary-switch count, and for Tree RCU also observes Linux's
blocked-reader marker and still-active nesting. Migration uses
`set_cpus_allowed_ptr()` and the reader's actual CPU observation. Idle is sampled
from upstream context-tracking state: idle with the RCU-watching bit clear.
The reader's `current` must agree with per-CPU current, and RCU must watch it.
No task state, CPU placement or RCU state is manufactured by the test.

The writer calls `synchronize_rcu()` / `synchronize_srcu()` or their explicit
expedited APIs. GP progress is checked with the public polling cookies (the full
cookie for Tree RCU). For SRCU, queued normal callbacks prevent legitimate
idle auto-expediting; the gate also observes whether the upstream expedited
request sequence changed. It does not modify that state machine.

## Callback, barrier and actual reclamation

After reader unlock, the synchronizing writer frees one object and two actual
RCU/SRCU callbacks free the other objects using `kmem_cache_free()`. Reader writes
preceding unlock must be visible to the reclaimers. An early reclaimer records
failure and retains the object while the reader may still access it.

One callback on CPU 0 is held briefly without sleeping, with preemption disabled
but IRQs enabled. A pinned hard hrtimer must interrupt it after its deadline.
On CPU 1, a witness explicitly wakes the barrier task and requires it to run and
block again while that callback is still active. This prevents a false pass
where an incorrect barrier merely cannot get CPU time. The callback ends only
after both the real hard IRQ and that barrier recheck. `rcu_barrier()` or
`srcu_barrier()` must not return before both callbacks complete.

Each case uses a private, nonmerged SLUB cache. After joining the test tasks,
all three objects must have been freed, GP polling must report completion,
dynamic SRCU must clean up, and the cache must shrink empty and destroy without
Linux warnings. A failed asynchronous case retains its state and requires
immediate process exit; it never frees storage underneath outstanding callbacks.

## Lowest IRQ-boundary correction

This gate exposed a real POSIX dispatcher bug: its callback-wide signal mask
and reentry guard prevented a hard timer from interrupting an RCU softirq
callback, even after Linux enabled IRQs. The correction is at machine IRQ
entry/return, not inside RCU. Callbacks enter and return IRQ-masked; the host
restores the interrupted state. Linux IRQ enable permits pending and future
nested delivery, while host bookkeeping remains protected. Return rechecks CPU
ownership after a possible task migration. See the [host contract](../host/posix/README.md).

An independent two-CPU host regression fails on the old dispatcher and checks
both synchronous pending-IRQ drain and later asynchronous nested delivery.
No callback signature, wire layout or upper Linux implementation was added.

## Running and scope

Build with `build_boot_runtime.py --link` and configure CMake as in the
[boot README](README.md), then run:

```sh
ctest --test-dir /home/kamer/os/.artifacts/kobox2-memory-gate \
  -R 'linux_rcu_gate' --output-on-failure
```

The launcher also accepts `kobox_linux_boot_test CORE.so --rcu`. Its process-local
report names the flavor, scenario, CPU, phase, source line and error bits.
Per successful run: 56 cases, 28 normal and 28 expedited synchronizations,
112 callbacks, 56 active-callback barrier probes, 168 actual object frees,
12 forced-preemption observations, 12 migrations and 20 idle observations.

Verified after the IRQ correction: 10 consecutive fresh-process RCU runs, all
39 configured CTests, 100 boot/service runs, 10 timed-wait runs and 100 independent
POSIX host runs pass with zero Linux warnings. The host gate also passes
ASan/UBSan. The new RCU gate sources pass checkpatch without warnings or errors.

This is a gate for this two-CPU profile, not a replacement for rcutorture or a
claim about every RCU configuration. CPU hot-unplug, NOCB, no-HZ-full,
PREEMPT_RT, Tasks RCU, module unloading and integrated teardown races are outside
it. Workqueue cancel/flush/rescuer coverage has its own [stage-6 gate](workqueue-gate.md); combined
IRQ/timer/work/RCU cleanup races have the [stage-7 gate](cleanup-gate.md).
GPU behavior is not certified.
