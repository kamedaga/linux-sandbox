# Upstream timed-wait gate

Stage 4 runs `wait_gate.c` after the stage-3 service/SMP/time gate in the same
boot-rooted `linux-boot-runtime.so`. It does not initialize services, replace
wait functions, manufacture pending-signal bits, or add host timer queues.
All waits, timer cancellation, signals and wakeups use the pinned Linux code.
The builder rejects replacements of these upper APIs.

The controller and waiter are pinned to opposite logical CPUs, and every case
is repeated with their CPUs reversed. Waiters are real `kthread_create()` tasks;
the controller joins them with `kthread_stop()` after observing their results.
An asynchronous test failure retains its objects and requires immediate process
exit, rather than freeing storage that an outstanding callback might still use.

## API coverage and return contracts

| APIs | Expiry | Successful early wake | Signal interruption |
| --- | --- | --- | --- |
| `schedule_timeout`, uninterruptible/interruptible/killable/idle wrappers, `io_schedule_timeout` | 0 | Remaining jiffies | Remaining jiffies when state permits |
| `wait_event_timeout`, interruptible/killable variants | 0 with false condition | Remaining jiffies, minimum 1 | `-ERESTARTSYS` when state permits |
| `wait_for_completion_timeout`, interruptible/killable/I/O variants | 0 | Remaining jiffies, minimum 1 | `-ERESTARTSYS` when state permits |
| `schedule_hrtimeout` relative/absolute/interruptible, `schedule_hrtimeout_range_clock` | 0 | `-EINTR` | `-EINTR` for interruptible state |
| `wait_event_hrtimeout`, interruptible variant | `-ETIME` | 0 | `-ERESTARTSYS` for interruptible variant |
| `msleep`, `usleep_range` | No return value | Explicit wake must not end the sleep | Uninterruptible |
| `msleep_interruptible` | 0 | Explicit wake retries the remaining budget | Remaining **milliseconds**, not jiffies |

The relative and absolute high-resolution waits use monotonic time. The range
case permits 1 ms timer slack. Timer slack is not a hard scheduling-latency
guarantee. Expiry checks use jiffies for jiffy APIs and `ktime_get()` for
high-resolution APIs; they do not demand nanosecond precision from the timer
wheel or impose an arbitrary wall-clock upper bound on task dispatch.

For jiffy-returning early waits, the oracle bounds Linux's internal samples
using the call's start/end, the observed inactive task, and the actual wake
submission. Waking is delayed until jiffies advance, so always returning the
original timeout fails. Repeated spurious wakes followed by a genuine event
(or a signal for `msleep_interruptible`) must retain the original budget. The
millisecond result is checked with Linux's actual `jiffies_to_msecs()` conversion.

## Race and edge cases

- Expiry, early wake, zero timeout, and pre-existing condition/completion.
  A true condition or completed event with zero timeout returns 1 for jiffy APIs,
  not the same 0 as an unsuccessful poll.
- Deterministic wake-before-schedule: a waiter publishes its sleep state while
  paused immediately before the public scheduling API. The other CPU must
  successfully execute `wake_up_process()` before releasing it. This does not
  patch the scheduler or wrap its state transitions. State-setting convenience
  wrappers are deliberately excluded from this case, since they reset state.
- Signals delivered to an actually inactive task, and signals pending before
  the call. Test kthreads opt in with `allow_signal()` and receive real
  `send_sig()` calls. Ordinary signals must not interrupt uninterruptible or
  killable waits; SIGKILL must interrupt killable waits. `flush_signals()` is
  called only after recording each result.
- Three unrelated hard IRQ callbacks on the waiter's CPU, each acknowledged
  through a **different** completion. `wait_task_inactive()` must report the
  same switch count after every IRQ, and the tested wait must still expire
  normally. No callback wakes the tested task or satisfies its condition.
  A separate pinned hard hrtimer supplies these real clockevent interrupts.
  Remote `irq_work` is unsuitable as proof of hardirq entry: upstream also
  drains its call-single queue from idle task context. That legitimate path
  was observed with an unchanged sleeping-task switch count during gate
  development; it must not be misreported as a wake or a runtime IRQ failure.
- Spurious explicit task wakeups must make waitqueue/completion and sleep-loop
  APIs recheck their conditions and sleep again. Repeated wakes must not reset
  the timeout budget.
- Infinite `MAX_SCHEDULE_TIMEOUT` and NULL high-resolution deadlines end only
  on explicit wake, with the respective upstream return values.
- Every returned wait leaves `current` runnable, on its bound CPU, consistent
  with per-CPU current, and no longer marked as waiting for I/O. Linux warnings
  fail the gate.

## Running

Build the fixed core using `build_boot_runtime.py --link`, configure CMake's
`KOBOX_LINUX_BOOT_RUNTIME_CORE` and `KOBOX_LINUX_TASK_BUILD_DIR` as described in
[the boot README](README.md), then run:

```sh
ctest --test-dir /home/kamer/os/.artifacts/kobox2-memory-gate \
  -R 'linux_timed_wait_gate' --output-on-failure
```

The launcher also accepts `kobox_linux_boot_test CORE.so --timed-wait`. Failure
diagnostics identify the API, scenario, CPU, source line, result and elapsed
nanoseconds. `wait_gate.h` describes only process-local test diagnostics; it
does not change the host contract or a wire ABI.

Verified result: 350 cases over 22 API variants and both CPUs, with 132
unrelated hard IRQ callbacks and zero Linux warnings per run. Ten consecutive
fresh-process runs pass. The complete configured CTest suite passes all 38
tests, and the corrected boot/service gate passes 100 fresh-process runs.

This gate is about in-kernel driver waits. Userspace syscall restart/copyout,
wall-clock adjustment, suspend/freezer behaviour, complete RCU/workqueue
semantics, teardown races, and GPU operations have separate requirements.
