# Upstream boot runtime integration

This is the integration of the existing machine/task/time ports into the
boot-rooted Linux core. It is not another manually initialized subsystem
fixture. The stage-3 boot/service gate and integrated two-CPU IRQ/time gates
now pass. This is not completion of the full driver runtime.

`build_boot_runtime.py` compiles every built-in object from the canonical
`vmlinux.a`, preserving its input order, init text, and initcall tables. Its
`linux-boot-inputs.json` only certifies those compiler inputs, not a linked or
running core. Driver modules do not select core source objects.

The compiler uses upstream's full-tree `vmlinux_o` target, not a single-file
`.a` target that can reuse stale libraries. The boot build retains upstream
static-call types and implementation; the isolated provider's indirect-call
header selection is not used here.

The boot-only `asm/sync_core.h` port uses the serializing `CPUID` instruction
at CPL 3 instead of native IRET-to-self's kernel-model addresses. It remains a
local instruction-stream barrier; Linux retains cross-CPU synchronization.
Concurrent text-patching protocols remain a separate integration concern.

The fixed native assembly inputs are made position-independent by the small
patches under `patches/`. They change address formation, not service logic or
initcall membership. The builder applies them only to generated copies, replays
the recorded Kbuild compiler and objtool commands, and checks that every global
definition remains. Source, patch, generated-source and object hashes are
recorded in `machine-bindings.json`. Retaining this native code does not make
its privileged entry points executable in a hosted process.

The C boundary patches keep Linux's exception decoder/reporting but replace
the native high-address test with owned core-text ranges, and omit reading
host-owned CR/MSR/debug registers in register dumps. The sole `init/main.c`
change hands PID 1 to the hosted entry **after** normal boot finalization and
sysctl arguments, before attempting a userspace init executable. It does not
replace initcalls, rootfs setup, service startup, or emulate a successful exec.

`runtime.lds.S` uses Linux's linker macros for initcalls, per-CPU data,
scheduler-class ordering and the initial task stack. The standalone ELF layout
test checks real init ranges, relocation of synthetic initcall entries, RAM
image bounds, and native TLS after `dlopen()`. Its fixture is never linked into
the runtime and does not demonstrate Linux boot or service progress.

`--link` additionally builds a strict `linux-boot-runtime.so`. A reviewed
symbol/owner allowlist restricts machine replacements; unexpected upper API
or data replacements are rejected. No unresolved-symbol boundary library is
generated. The full core now links and loads, importing only native TLS access.
`load_test.py` checks page-disjoint PT_LOAD permissions, absence of RWX and
dynamic text relocations, the complete linked initcall tables (currently 129
targets), and executable target addresses without invoking those initcalls.
Explicit ELF program headers prevent neighbouring metadata sections from
remapping a relocation target read-only during `dlopen()`.

The current boot profile requires real shmem/tmpfs. Merge `boot/config` after
the canonical base and `task/config`, rebuild the canonical image, and use fresh
hosted/output directories for its changed identity. The mandatory
[boot memory gate](memory-gate.md) verifies page/SLUB/per-CPU/vmap and real shmem
after upstream boot, with no fixture initialization. `--all` runs it and every
chapter-1 gate in one boot; CTest exposes `kobox2.linux_full_foundation_gate`.

Chapter 2's [VFS lifetime gate](vfs-gate.md), selected by `--vfs` or `--all`,
adds real tmpfs mounts, named-file I/O, Linux FD close/unlink, independent
file/inode/folio references, task-work and delayed fput, RCU inode freeing and
superblock destruction. It requires actual allocator reuse after reclamation,
without a new runtime port or upper-subsystem replacement.

The [shmem / page-cache gate](shmem-gate.md), selected by `--shmem` or `--all`,
adds two-CPU shared-folio access, sparse/partial-page zeroing, size changes,
truncate/hole-punch contention, preallocation rollback, exact block/inode/commit
accounting and pinned-page reclamation. It uses the existing boot core and ports.

`image.c` aliases the relocated DSO's actual PT_LOAD pages to its reserved Linux
RAM backing before CPU entry. It preserves their permissions and ELF/TLS data;
it does not just reserve a numerically corresponding physical range. The image
test checks capacity failures before mutation, bidirectional data visibility,
code bytes and execution/TLS after remapping. Backing without executable-map
support is rejected before replacing any DSO page. Boot also checks that its
data and direct-map views actually share storage.

The boot-only runtime-constant header preserves upstream's sites, values and
table traversal; instruction writes use the writable RAM alias and a local
instruction-stream barrier, leaving the executable mapping RX. This is not
the concurrent text-patching protocol. Boot-end publication protects text,
rodata and ro-after-init in both the image and direct-RAM views. Init reclamation
first revokes the image alias, then uses upstream `free_reserved_area()` to
poison, release and account real Linux pages; direct RAM remains available for
buddy reuse. Image tests verify actual write/access faults and reject RWX.

`boot_test.c` enters real `start_kernel()` through the same POSIX machine
bindings as the standalone task fixture (`task/posix_machine.c`). It reaches
`SYSTEM_RUNNING` through real `mm_core_init()`, PID 1, CPUHP, initcalls and
boot-end reclamation/protection, then executes `service_gate.c` in PID 1.
The core is strict-linked without any phase-boundary DSO or success stub.

Synchronous host faults enter Linux's exception-table and WARN/BUG paths;
INT3 uses the upstream text-poke handler and die-notifier chain (including
Linux's alternatives self-test). CPU setup advertises the common x86-64
FP/SSE instruction baseline, not host APIC, vendor firmware or XCR0 ownership.
Linux retains FPU-state initialization, task allocation sizing and minimal
FP-state cloning; native pthreads preserve the executing register context.
The upstream real-mode platform hook instead installs the pthread AP entry.
There is no legacy PIC/APIC or native PCI configuration-port access; device
resources and MSI domains must come from the later host bridge integration.

The boot integration must preserve upstream `start_kernel()`, PID 1 and
`kthreadd` creation, early and normal initcalls, and the CPUHP state machine.
In particular, `workqueue_init()` precedes pre-SMP initcalls; the latter run
before the AP is online. The host only starts an AP pthread when the upstream
CPU bringup path requests it.

The memory port separates binding host mappings from `setup_arch()` work.
Upstream boot remains responsible for `setup_per_cpu_areas()` and
`mm_core_init()`. The task port accepts `user_mode_thread()` with a kernel
start function and no user mm, as needed by PID 1. Ordinary userspace cloning
and a return to user instructions are not thereby implemented.

In the boot build, the standalone manual memory/scheduler initialization
entries and AP idle loop are excluded. `kobox_linux_task_verify_boot()` reuses
the SMP/time checks only after Linux reaches `SYSTEM_RUNNING`; it does not
create `kthreadd`, invoke `cpu_stop_init()`, or initialize scheduler services.

The gate verifies both CPUs' `ksoftirqd` and actual RCU/kworker threads, local
and remote `irq_work` with IRQs disabled, a self-requeued tasklet that runs
first on IRQ exit and then in `ksoftirqd`, per-CPU basic work, and an RCU callback that frees its
allocation. It then reruns the existing SMP/IRQ/idle/timer gates, including
high-resolution operation and tick-driven busy-task preemption. Unexpected
Linux warnings fail the gate. Tests neither initialize services nor choose
scheduler tasks.

Remote `irq_work` can legally run in idle's call-single queue flush, outside
hardirq context. The tasklet's first batch is therefore triggered by a pinned
hard hrtimer, proving actual hardirq/IRQ-exit progress without requiring the
upstream idle flush to change its semantics.

The [stage-4 timed-wait gate](wait-gate.md) uses the same completed boot and
tests jiffy/high-resolution waits, completion, waitqueue and sleep APIs. It
covers expiry, early wake, deterministic wake-before-schedule, real signals,
remaining budgets across repeated wakes and unrelated hard clockevent IRQs.
Neither the wait APIs nor the host contract are replaced for this gate.

The [stage-5 Tree RCU/SRCU gate](rcu-gate.md) verifies held-reader safety,
normal/expedited GPs, preemption, migration, idle, callback/barrier ordering and
actual SLUB reclamation. It also caught and corrected callback-wide IRQ
suppression in the POSIX machine boundary; upper RCU implementations stay intact.

The [stage-6 workqueue gate](workqueue-gate.md) covers normal/highpri/BH/
unbound/ordered queues, pending and running cancel/flush, delayed work,
self-requeue, FIFO, live affinity changes and actual RAM-pressure rescue.
It adds tests only, not a host or upper workqueue implementation. The pinned
upstream restriction on BH requeue during drain is documented explicitly.

The [stage-7 cleanup gate](cleanup-gate.md) checks a live IRQ/timer/work/RCU
dependency graph through admission closure, synchronization, reader GP,
callback barrier, final-work drain and actual vmalloc alias revocation.
Held-context probes and concurrent IRQ attempts continue across teardown.
No host or upper implementation is replaced.

After building with `build_boot_runtime.py --link`, set the CMake cache entry
`KOBOX_LINUX_BOOT_RUNTIME_CORE` to the resulting `linux-boot-runtime.so` (along
with `KOBOX_LINUX_TASK_BUILD_DIR`). CTest adds `kobox2.linux_boot_runtime_load`,
`kobox2.linux_boot_service_gate`, `kobox2.linux_timed_wait_gate` and
`kobox2.linux_rcu_gate`, `kobox2.linux_workqueue_gate` and
`kobox2.linux_cleanup_gate`.
The adjacent `linux-boot-inputs.json` is
required. The load check alone is not the execution gate.

Stage-7 verification: all 41 configured CTests pass. Ten consecutive fresh
processes each pass all 52 integrated-cleanup cases, 20 observed synchronization
waits and 52 actual vmalloc frees, with zero Linux warnings. No host or upper
implementation changed in this stage.

Stage-6 verification: all 40 configured CTests pass. Ten consecutive fresh
processes each pass all 211 workqueue cases and 1,456 callbacks, including actual
RAM-pressure rescue on both CPUs, with zero Linux warnings. No host or upper
workqueue implementation changed in this stage.

Verification after the stage-5 IRQ correction: all 39 configured CTests pass.
The boot/service and independent POSIX host gates each pass
100 consecutive fresh-process executions. The timed-wait gate passes all
350 cases across 22 API variants and both CPUs, including 132 unrelated hard
IRQ callbacks, in each of 10 consecutive fresh-process executions. The RCU/SRCU
gate passes 56 cases, 112 callbacks and 168 actual object frees in each of 10
consecutive fresh-process executions. Linux warnings remain zero. The POSIX
host gate also passes ASan/UBSan. The image mapping/protection test passed
ASan/UBSan during stage 3; that implementation is unchanged by stages 4 and 5.

Actual device/DMA removal and GPU operations remain later gates.
CPU hot-unplug, Linux userspace execution, general module
text protection/patching and complete panic/shutdown handling are not certified
by this gate.
