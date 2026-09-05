# Linux task/SMP gate

This gate connects the pinned Linux scheduler to one native pthread per Linux
task. Linux retains its runqueues, scheduling classes, task selection, wakeup,
affinity, migration, and exit implementations. The POSIX adapter transfers a
logical CPU's execution token to the task selected by `switch_to()`; it has no
task-selection policy.

The architecture port supplies task creation and the first-run trampoline,
park/unpark at a context switch, the post-switch exit notification, TLS task/CPU
identity, IRQ masking and IPI delivery, and a monotonic scheduler clock.
Native pthread stacks and host FS/GS remain host-owned. All tasks in this gate
are kernel threads sharing the host address space; user-task creation is
rejected.

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
`PREEMPT_NONE` and dynamic-preemption builds are rejected. Set
`KOBOX_LINUX_TASK_BUILD_DIR` to this build directory, then run:

```sh
cmake --build /path/to/cmake-build --target kobox_linux_task_smp_test
ctest --test-dir /path/to/cmake-build -R 'kobox2.linux_task_smp' --output-on-failure
```

This is a task/SMP gate, not a complete driver-runtime gate. It does not certify
periodic scheduler ticks, timed waits, RCU grace-period/callback progress,
workqueue execution, CPU hotplug, user address spaces, or complete runtime
shutdown. Linux's boot-service threads remain parked until the fixture process
exits; RCU-deferred task reclamation is not treated as completed by native join.
