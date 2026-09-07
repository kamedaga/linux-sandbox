# Linux task/SMP gate

This gate connects the pinned Linux scheduler to one native pthread per Linux
task. Linux retains its runqueues, scheduling classes, task selection, wakeup,
affinity, migration, and exit implementations. The POSIX adapter transfers a
logical CPU's execution token to the task selected by `switch_to()`; it has no
task-selection policy.

The architecture port supplies task creation and the first-run trampoline,
park/unpark at a context switch, the post-switch exit notification, TLS task/CPU
identity, IRQ masking and IPI delivery, and a monotonic scheduler clock.
Native pthread stacks and host FS/GS remain host-owned. Tasks execute kernel
start functions in the host address space. A regression also covers the
`user_mode_thread()` kernel-entry path used by PID 1: it has no user mm and
does not have `PF_KTHREAD`. Ordinary user-address-space cloning and returning
to user instructions remain unsupported.

The boot fixture uses the real memory bootstrap, `init_task`, `sched_init()`,
`workqueue_init_early()`, `rcu_init()`, timer/softirq initialization, and Linux
timekeeping backed by a host clocksource. It creates the second CPU's idle task
with Linux `fork_idle()`, and uses real `kthreadd` and CPU stopper threads.
`cpu_stop_init` is the one explicitly retained early initcall: only its symbol
visibility changes, not its implementation. CPU hotplug registration uses Linux
instead of the memory-only fixture's callback recorder.

The runtime gate requires all of the following on two logical CPUs:

- local and remote switching through Linux `schedule()`/`try_to_wake_up()`;
- agreement between TLS `current`, the per-CPU current slot, and CPU identity;
- both sleeping-task migration and forced migration of a running task;
- affinity enforcement and remote reschedule IPI delivery;
- no switch on the affected CPU while `preempt_disable()` is held;
- IPI retention while local IRQs are disabled, followed by delivery on enable;
- Linux task exit, `kernel_wait()` reaping, and native pthread join.

The IRQ/time extension additionally requires, on both CPUs:

- upstream `irq_enter()`/`irq_exit()` with hardirq accounting, matching
  `current`/CPU identity, and balanced RCU context tracking;
- upstream `default_idle_call()` entering RCU idle, interrupt entry making RCU
  watch the CPU, interrupt exit restoring idle, and idle exit restoring watching;
- a registered host clocksource and per-CPU one-shot clockevents driving Linux
  tick, the timer wheel, and high-resolution hrtimers;
- actual hres-active state and 500-us hrtimer deadlines (below the 4-ms tick),
  with no early callback and the proper hardirq/softirq execution contexts;
- masked, delayed delivery, cancellation of an already-expired pending event,
  rearming to a later deadline, and stale notifications without wrong callbacks;
- two simultaneously runnable busy kthreads per CPU, progressing through
  repeated involuntary switches without yielding; and
- tick progress during each busy task's `preempt_disable()` interval, with a
  reschedule request pending but no task switch until preemption is enabled.

`time_port.c` implements the clockevent device, not a Linux timer queue. Its
host contract adds only per-CPU absolute monotonic arm, cancel, and final
producer-stop operations. The POSIX timer callback merely notifies the CPU;
Linux's selected event handler runs in that CPU's execution domain. A stale
host notification cannot expire a disarmed or not-yet-due device. The IRQ
register frame describes a logical Linux kernel thread, not the host's ring-3
execution. Scheduler clock reads, including `sched_clock_noinstr()`, use the
same host monotonic source.

The fixture initializes upstream static keys before memory bootstrap and uses
Linux's own `jiffies_64`, jiffies lock, and sequence counter. `time_gate.c` keeps
the tests separate from the clockevent device implementation. These tests
check Linux time semantics; they do not promise hard real-time host latency.

Join waits on a Linux completion signaled by the architecture post-switch hook,
after Linux clears `on_cpu` and releases the runqueue lock. It does not block
the host while an exiting task still needs the same logical CPU to finish.

`build_task_smp.py` records the canonical config/image hashes, source objects,
gate-symbol origins, architecture definitions, and fail-closed phase imports.
Unresolved functions terminate the fixture; ordinary unresolved data is guarded
with `PROT_NONE`. Unresolved per-CPU data is rejected, because adding a per-CPU
offset to a foreign guard could escape it. The few data-only architecture inputs
retain Linux's actual per-CPU sections without pulling in native context-switch
or FPU code. Overlay content is part of Kbuild's command fingerprint, including
when a newly added header shadows an existing x86 header.

Use the canonical Linux build for the memory gate, merge `task/config`, and
rebuild `vmlinux`. The task gate requires two CPUs and static `CONFIG_PREEMPT=y`;
`PREEMPT_NONE` and dynamic-preemption builds are rejected. It also requires
`HIGH_RES_TIMERS`, `CONTEXT_TRACKING_IDLE`, `BUG`, and `RCU_EQS_DEBUG` so the
runtime checks cannot silently disappear. Set
`KOBOX_LINUX_TASK_BUILD_DIR` to this build directory, then run:

```sh
cmake --build /path/to/cmake-build --target kobox_linux_task_smp_test
ctest --test-dir /path/to/cmake-build -R 'kobox2.linux_task_smp' --output-on-failure
```

This is a task/SMP/IRQ/time gate, not a complete driver-runtime gate. It does
not certify the full timed-wait API/remaining-time matrix, RCU grace-period or
callback reclamation, workqueue execution, CPU hotplug, user address spaces,
or complete runtime shutdown. Linux's boot-service threads remain until the
fixture process exits; RCU-deferred task reclamation is not treated as completed
by native join. The production boot-rooted core and upstream service/initcall
integration remain separate from this isolated subsystem fixture.

The fixture and real-boot launcher share `posix_machine.c` for host task,
CPU-domain, IRQ and clockevent bindings. This shares the machine adapter, not
the fixture's manual Linux initialization or its completion claim.
