# IRQ/timer/work/RCU teardown gate

Stage 7 runs `cleanup_gate.c` after the real boot/service/SMP/time gate in the
same fixed `linux-boot-runtime.so`. IRQ descriptors, threaded IRQs, timers,
workqueues, Tree RCU and memory reclamation are the pinned upstream code. This
stage adds a device-lifecycle fixture and assertions, not a generic cleanup
runtime, upper API replacement, host operation or manual service startup.

## The dependency graph determines the order

The fixture has a real `vzalloc` device, an RCU-published lookup pointer and a
separately allocated interrupt-source/oracle object that outlives the device.
Its running graph is:

```text
hard IRQ / IRQ thread / timer / hard hrtimer / BH / delayed work / RCU reader
                                 -> normal work
normal work -> timer / hard hrtimer / delayed work / one BH / one RCU callback
regular RCU callback -> normal work

after closing, a finite teardown-only graph:
final normal-work pass -> retirement RCU callback -> final work -> free
```

Normal admission and the closing transition use the **same IRQ-safe lock**.
Checking state and submitting work, arming a timer or submitting the regular
RCU callback are one critical section. Closing unpublishes the lookup pointer
under that lock. It never waits for a callback while holding the lock. Already
admitted work retains access until its corresponding synchronization completes.

The teardown task follows this graph, rather than a universal cleanup recipe:

1. Close admission and RCU-unpublish the device.
2. `disable_irq_nosync()` and `synchronize_irq()`: mask new deliveries and wait
   for both the primary handler and the sleeping IRQ thread.
3. `timer_shutdown_sync()`, `hrtimer_cancel()`,
   `disable_delayed_work_sync()` and `disable_work_sync()` for BH work. Timer
   shutdown permanently breaks timer/work rearming; hrtimers instead rely on
   the closed admission path because cancel alone does not prohibit restart.
4. `synchronize_rcu()` waits for public readers that obtained the old pointer.
   This is not callback completion. Stopping held atomic callbacks first also
   avoids making the reader GP depend on a deliberately stalled softirq.
5. Permit and queue the last normal-work pass, flush it and disable that work.
   It submits exactly one retirement callback, after public readers are gone.
   No ordinary producer can submit another RCU callback at this point.
6. `rcu_barrier()` waits for all previously submitted callbacks, including the
   retirement callback. That callback may enqueue the one final work.
7. Flush/disable that final work **after** the barrier; destroy both private
   queues and `free_irq()` the action/thread. Verify pending and active state,
   then `vfree()` the device exactly once.
8. Keep the masked source running during a post-free observation interval.
   Only afterwards synchronously cancel the source, free its IRQ descriptor and join
   fixture kthreads before freeing the stable oracle.

Steps 4, 6 and 7 protect different lifetimes. A reader GP does not drain queued
callbacks, and a callback barrier does not drain work enqueued by a callback.
The finite retirement edge is explicitly permitted after closing; it does not
reopen the normal submission path. Its atomic once flag prevents duplicate
retirement when an old running work and the final queued pass overlap.

## Required observations

Ten held-context cases are run with CPU roles reversed: hard IRQ, IRQ thread,
timer, hard hrtimer, normal work, BH work, delayed work, normal RCU reader,
retirement RCU callback and final work. Another 16 timing-varied stress cases
run for each source CPU: **52 cases, 20 synchronization probes, 52 actual frees**.
All non-reader callback kinds must execute in every stress case.

- The interrupt source is a pinned hard hrtimer delivered by the real host
  clockevent/IRQ path. It invokes `generic_handle_irq()` in actual hardirq
  context through an allocated descriptor, `handle_level_irq()`, an irqchip
  with working mask/unmask/ack and `request_threaded_irq(IRQF_ONESHOT)`.
  It is not direct task-context invocation of a device handler.
- Hardirq-only synchronization must report an active held IRQ thread;
  `synchronize_irq()` must really block until that thread finishes.
- Sleeping teardown APIs must have a genuinely inactive cleanup task inside
  the expected phase, not merely an unscheduled task or its final join wait.
- For busy-wait synchronization, a hard hrtimer on the other CPU must interrupt
  the **cleanup task itself** in the expected API phase while the target
  callback is active. Only that observed interrupt releases the callback.
- A held reader prevents the public-reader GP from completing. A later held
  retirement callback prevents the barrier from completing despite the
  already completed reader GP. A held final work prevents release despite the
  already completed barrier.
- Every held normal producer attempts reentry after closing, and its own
  admission-rejection counter must increment. Before freeing still-live
  storage, direct rearm/queue attempts additionally verify the timer-shutdown
  and work-disable semantics.
- Callback context/current, active counters, one retirement, one final-work
  completion and one free are checked. All timers/work must be inactive or
  not pending at teardown. During post-free observation, IRQ attempts and
  mask rejections must increase but device callback counters must not change.

The device uses real Linux vmalloc storage. Before use its alias must read
correctly. After real `vfree()` and upstream `vm_unmap_aliases()` to drain lazy
TLB invalidation, the same alias must fault via upstream's
exception-table-protected `__get_kernel_nofault` load. The host vmap boundary
revokes the mapping, making late embedded timer/work/RCU accesses fail instead
of relying only on a magic word in still-mapped allocator storage. The native
x86 `copy_from_kernel_nofault()` address filter rejects host user addresses,
including hosted kernel vmalloc addresses; the oracle deliberately uses the
architecture's fault-safe load for this known fixture mapping. It does not
certify that native address-filter API or replace it with a success stub.

All helper threads are created before holding an atomic callback. In the
deterministic cases, after upstream has completed IRQ-thread startup/initial
affinity setup, the public scheduler affinity API places that thread on the
controller CPU. Otherwise a held BH on its CPU could starve the IRQ thread
needed by the earlier IRQ synchronization, invalidating a test intended to
observe BH cancellation. Stress cases retain ordinary IRQ-thread affinity.
No Linux scheduler, IRQ-thread code or workqueue internals are changed.

## Running and limits

Use `kobox_linux_boot_test CORE.so --cleanup` or CTest
`kobox2.linux_cleanup_gate`. The process-local report contains scenario, CPU,
phase, source line, error bits, completed cases, callback/rejection/probe/free
counts and Linux warnings. Callback and rejection counts vary with timing.
Every unexpected Linux warning fails the gate. On asynchronous failure the
fixture retains storage and the launcher exits immediately; it does not try
to recover by freeing potentially live objects. The process watchdog bounds
an unresponsive upstream wait.

Verified: ten consecutive fresh-process runs each pass all 52 cases, 20
synchronization probes and 52 actual frees, with zero Linux warnings. All 41
configured CTests pass, including the prior memory, task/SMP, boot/time,
timed-wait, RCU/SRCU and workqueue gates. The new C/header pass checkpatch with
zero warnings/errors, and the builder's negative override tests pass. The
598 canonical boot objects and 129 initcall targets remain intact. No upstream
source path, host operation or PachaOS kernel code changed in this stage.

This certifies the stated two-CPU preemptible fixture and dependency graph,
with one teardown owner. It is not proof of every driver's removal graph,
concurrent duplicate removal, SRCU-integrated teardown, CPU hot-unplug,
PREEMPT_RT, module-text unloading or sandbox process kill/revoke. SRCU's own
reader/callback/barrier behavior is covered by the separate stage-5 gate.
Actual PCI/MSI-X transport, DMA fencing and GPU removal must add their real
dependencies to future device gates. The synthetic IRQ source is not an
external PCI backend. No PachaOS kernel or adapter is changed here.

Chapter 2's [boot memory gate](memory-gate.md) corrected sparse-range handling
in the mapping port: revocation follows PTE removal/TLB invalidation, not the
pre-unmap cache hook. This gate now explicitly drains lazy aliases before the
fault oracle. Its callback synchronization and post-free IRQ checks are unchanged.
