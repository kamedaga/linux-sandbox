# Upstream workqueue gate

Stage 6 runs `workqueue_gate.c` after the real boot/service/SMP/time gate in
the same fixed `linux-boot-runtime.so`. Worker creation, concurrency management,
BH dispatch, timers, cancellation, flush colors, rescuer creation and mayday
delivery all remain in the pinned upstream Linux implementation. There is no
new host operation, upper replacement, service-start shortcut or failure stub.
The builder's negative tests reject replacements of workqueue APIs and worker
scheduler hooks.

## Matrix

The six queue types are per-CPU normal, per-CPU highpri, BH, BH-highpri, unbound
and ordered. Sixteen cases run on each type, with the worker's CPU reversed:
192 matrix cases. The controller uses the other CPU. Threaded callbacks may
sleep; held BH callbacks only busy-wait with a bounded watchdog. Helper kthreads
are created **before** holding a BH callback, since `kthreadd` may need that CPU.

| Area | Required observations |
| --- | --- |
| Execution context | Correct CPU/current, IRQs enabled, threaded worker identity and `current_work()`, elevated nice for highpri, softirq rather than hardirq for BH |
| Pending cancellation | Duplicate queue rejected; asynchronous and synchronous cancel return true only for pending work; canceled work never executes and can be reused |
| Running cancellation | Non-sync cancel returns false without waiting; sync cancel waits for the running callback even when its return value is false |
| Running and pending | A callback has already requeued itself; sync cancel removes the pending instance, waits for the running instance and returns true |
| Self-requeue cancellation | The running callback attempts requeue while sync cancel is in progress; no second invocation survives the cancel |
| Flush | `flush_work()` and running `flush_delayed_work()` wait for callback completion; idle flush returns false |
| Queue flush snapshot | After flush has blocked, a later held work is queued; flush must finish while that later work is still running |
| Self-requeue | Normal and zero-delay delayed work each execute 32 times without concurrent invocation of the same item |
| Drain | Threaded queues drain 32 chained requeues; BH queues drain their held callback without new submission |
| Delayed work | Timer cancellation/reuse, idle versus pending `mod_delayed_work_on()` returns, earlier and later rearming, actual expiry, pending-timer flush, timer-to-work transition and cancellation |

Threaded sync-cancel tests observe upstream's temporary work-disable bits and
require the helper task to be genuinely inactive inside the API, not merely
unscheduled or parked for its final join. No work data is written by the oracle.
For BH sync-cancel, upstream permits non-hardirq atomic context. The calling
task disables preemption and enters the actual cancel API. A pinned hard
hrtimer must interrupt that caller while Linux has disabled the work and the
remote callback is still active; only that timer releases the callback. The
API must not return before the timer ran and the callback ended. This also
checks real nested IRQ progress through the busy-wait cancellation path.

Queue-flush helpers must actually block inside upstream flush before the later
work is submitted. This tests flush's snapshot semantics, not just eventual
completion after stopping every producer. Callback input and output stamps
check the queue/flush memory-ordering handoff. Every case ends with no active or
pending work and no pending delayed timer; test tasks are joined before reuse.

## Additional dispatch and affinity checks

- Ordered FIFO: 16 submissions alternate between real CPU 0/1 call-function
  contexts. The entire queue must preserve submission order and single-active
  execution, independent of the producer CPU.
- Unbound execution: two different workers must be active concurrently with
  both CPUs allowed. Changing attributes from CPU 0 to CPU 1 while old work is
  held must allow new work on CPU 1 while the old pool drains correctly.
- Twelve system-queue cases: `system_wq`, `system_highpri_wq`, both BH queues,
  `system_unbound_wq` and `system_dfl_wq`, submitted from either CPU. Each of
  16 items must execute exactly once. BH batches must retain per-CPU FIFO.
  The gate flushes only its own items, never an entire shared system queue.
- BH priority on both CPUs: queue normal before highpri while BH/IRQ delivery is
  disabled in task context, then enable delivery. The highpri callback must run
  first. This does not demand preemption of an already executing BH callback.

## Real memory-pressure rescue

Each CPU gets a fresh `WQ_UNBOUND | WQ_MEM_RECLAIM` queue with a distinct nice
attribute and a single-CPU affinity mask. Its upstream rescuer and all test work
objects are allocated before pressure begins. The controller retains actual
Linux pages using `alloc_page(GFP_NOWAIT | __GFP_NOWARN)` until allocation fails;
this consumes roughly 240 MiB of the fixture's 256 MiB Linux RAM, not the host's
available memory. Sixteen works then make forward progress depend on rescue.

Ordinary workers retain any further pages returned by earlier asynchronous
cleanup and wait for the rescue completion. Their non-sleeping page operations
hold the same CPU execution domain using upstream `preempt_disable()`; the
rescuer cannot free their lists concurrently. Only a callback for which upstream
`current_is_workqueue_rescuer()` is true releases the retained pages and wakes
those workers. It must run on the queue's selected CPU. The gate never calls
the mayday handler or rescuer directly, edits pool counters, or substitutes a
failed allocator/worker creation function.

The controller holds upstream `oom_lock` during this deliberately bounded
pressure interval. This serializes against the OOM killer so that testing
reclaim progress does not kill PID 1 or panic for lack of killable tasks. It
does not change allocator failure, reclaim or worker retry semantics; the
controller makes no direct-reclaim allocation while holding that lock. Once
the rescuer completes, the lock is released, all 16 callbacks must finish, all
retained-page lists must be empty, a normal GFP_KERNEL allocation must succeed,
and the private queue must destroy cleanly. Both CPU-targeted cases must pass.

## Pinned-upstream constraints and failure policy

Two invalid combinations are deliberately not misreported as port failures:

- A held BH callback prevents nested `TIMER_SOFTIRQ` processing. For BH's
  timer-to-pending-work cancellation case, zero-delay `mod_delayed_work_on()`
  performs that transition instead. Natural delayed expiry is independently
  tested for BH; threaded queues also test expiry while another work is held.
- In this pin, `is_chained_work()` recognizes only task-context workers through
  `current_wq_worker()`. BH requeue during `drain_workqueue()` is therefore
  rejected by upstream. BH self-requeue and BH drain are tested separately;
  this combination is not supported or certified. No upper patch hides it.

All unexpected Linux warnings fail the gate. Asynchronous failures retain test
storage and require immediate process exit; a failed pressure wait also retains
its OOM lock until process exit, avoiding a concurrent free underneath rescue.
This fixture does not attempt recovery after a failed oracle.

## Running and scope

Build with `build_boot_runtime.py --link` and configure CMake as described in
the [boot README](README.md), then run:

```sh
ctest --test-dir /home/kamer/os/.artifacts/kobox2-memory-gate \
  -R 'linux_workqueue_gate' --output-on-failure
```

Direct invocation is `kobox_linux_boot_test CORE.so --workqueue`. Its
process-local report identifies queue, scenario, CPU, phase, source line and
error bits. A successful run has 211 cases and 1,456 callbacks, with rescuer
execution required for both CPU-targeted queues. Page totals and the number of
callbacks handled by a rescuer can vary with ordinary background cleanup.

Verified: ten consecutive fresh-process runs pass all 211 cases and 1,456
callbacks per run, including both CPU-targeted rescues. All 40 configured CTests
pass, including the prior memory, task/SMP, boot/time, timed-wait and RCU gates.
Linux warnings are zero. The new gate sources pass checkpatch with no warnings
or errors. The canonical 598 boot objects and 129 initcall targets remain
retained, and upstream source paths are unchanged.

The target is the stated two-CPU preemptible profile. CPU hot-unplug, multiple
NUMA nodes, PREEMPT_RT, freezer/suspend, every optional workqueue API and GPU
operations are not certified here. Integrated IRQ/timer/work/RCU cleanup races
have their own [stage-7 gate](cleanup-gate.md).
