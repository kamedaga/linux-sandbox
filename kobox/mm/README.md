# Hosted user address spaces

`port.c` replaces only x86 mm activation and TLB invalidation operations. The
fixed boot-rooted core retains upstream mm allocation/destruction, VMA
operations, page tables, shmem, rmap and fault handling. The builder exposes
the existing local `do_user_addr_fault` symbol without changing its code.
Its negative ownership tests reject replacement MM/fault/VMA implementations.

One bound external host process has one real Linux `mm`. Kernel service tasks
borrow it with upstream `kthread_use_mm` / `kthread_unuse_mm`. The host process
contains the native access client, not a second copy of the Linux core.
The managed mapping aperture is distinct from that client's native bootstrap
code, stack and control page. The Linux test client reserves 16 pages
at `0x4000000000`; out-of-aperture operations are rejected, not remapped elsewhere.

## Mapping and lifetime contract

- Linux performs `vm_mmap`, native `mprotect`, `vm_munmap`, and
  `do_user_addr_fault`. The port neither selects pages nor invents VMA policy.
- After Linux resolves a delivered access fault, the port rereads the actual
  PTE under `mmap_lock` and the page-table lock. PROT_NONE/nonpresent PTEs cannot
  publish translations. Host write access requires a dirty, writable PTE,
  since native host stores cannot set Linux's PTE dirty bit. Large leaf and
  non-RAM PFNs return `-EOPNOTSUPP`; they never report a successful map.
- PTE publication and host reset serialize on the binding's translation lock.
  Publication masks Linux IRQs for that lock's entire lifetime: VM IRQ dispatch
  takes the registry lock, which a concurrent flush holds while waiting for
  the translation lock. Masking only the native host call is insufficient.
  `flush_tlb_mm_range` completes host invalidation before returning to Linux's
  deferred page-release path. Batched architecture flush conservatively resets
  all bound address spaces; it does not implement another rmap subsystem.
- The caller must join all service tasks using a binding before destruction.
  The registry protects it against concurrent IRQ/flush traversal. Close
  actually reaps the client before registry removal and upstream `mmput`.
  A host invalidation failure cannot be converted into a successful flush.

## Linux host transport

`posix.c` translates the process-local machine interface to the separately
linked Linux-specific `host/posix/vm*.c` transport. Those files use ptrace,
eventfd/signalfd and a bootstrap futex; they are not part of the restricted
portable `kobox_posix_host` archive. Positive host errno becomes negative Linux
errno at this adapter. No controller, native descriptor or core pointer is
added to the shared wire protocol.

A dedicated native thread owns all ptrace operations. Synchronous map/reset
requests need no Linux task progress, including under page-table locks or
disabled IRQs. Client execution is asynchronous. Completion notification
enters the existing logical-CPU IRQ domain and wakes upstream waitqueues;
Linux's scheduler still chooses the task/CPU. VM notifications use the unfrozen
process-local interface, not the wire ABI.

The manager must be created before other threads so they inherit blocked
SIGCHLD, and destroyed on its creator after callers join and all remotes close.
Closed handles remain tombstones until manager destruction. Sequence checks
reject stale execution resumes. Ordinary client syscalls are intercepted
before execution; only authorized native signal return is allowed, so the
inherited RAM descriptor cannot bypass the mapping authority.

PTRACE_INTERRUPT stops even an unbounded user loop. A fault already generated
before that interrupt can arrive before the injected syscall executes. The
transport recognizes that precise stop and restores/retries the saved access
instruction after the mapping operation, rather than attributing the fault to
the syscall trampoline. Repeated tests cover interruption during fault capture
and signal return. Reset may acknowledge process death only after actual
reaping; ESRCH or an absent response alone is insufficient. Death of a stopped
client is also reported, once, as a terminal completion.

## Integration tests

`boot/vm_gate.c` provides integration probes. Each starts
from upstream boot and uses two real processes and two distinct mm/service
tasks on two logical CPUs. It checks real read/write faults, shared writes,
per-mapping read-only protection, partial unmap, unaffected peer mappings,
Linux SIGSEGV decisions, and SIGBUS after truncate. Denied clients stay stopped
until teardown: the probe observes Linux's queued signal decision and does not
exercise Linux userspace signal handlers.

The MM gate is the complete `linux-mm-vma` CTest label, together with the host
transport/service tests and the foundation regression suite. A single probe
passing does not certify the whole gate.

The invalidation/reuse probe delays the second real host reset acknowledgement
during truncate. The other CPU allocates pages without holding a test reference
to the old folio. After acknowledgement it must reacquire that PFN from buddy,
overwrite it, and observe Linux rejecting real accesses through both old aliases.

Publication-race probes cover a pending VM IRQ during the architecture broadcast
flush, native `vfs_truncate` while the publishing PTE is locked, and host
close/reap either before publication completes or before the delayed fault
resume. Truncate cases reacquire and overwrite the old PFN before allowing the
client to resume. Dead bindings must reject both delayed maps and resumes.

Lifetime probes cover normal teardown, unsolicited process death waking a
sleeping Linux task, and native rejection of `MAP_SYNC` on shmem after partial
setup. Every probe joins its mm borrowers and drains native lazy-mm references,
task work, delayed fput and RCU. Observer references are released before
reacquiring the retired mm/task/file/inode cache slots and remaining file PFNs
from the native allocators. Raw slab probes are storage-reuse checks, not
substitute Linux objects.

Run the probes with the boot test launcher, a newly built full core, and the
native client executable:

```text
kobox_linux_boot_test CORE --vm-probe CLIENT
kobox_linux_boot_test CORE --vm-probe-ro CLIENT
kobox_linux_boot_test CORE --vm-probe-reuse CLIENT
kobox_linux_boot_test CORE --vm-probe-irq CLIENT
kobox_linux_boot_test CORE --vm-probe-truncate CLIENT
kobox_linux_boot_test CORE --vm-probe-exit-publish CLIENT
kobox_linux_boot_test CORE --vm-probe-exit CLIENT
kobox_linux_boot_test CORE --vm-probe-lifetime CLIENT
kobox_linux_boot_test CORE --vm-probe-death CLIENT
kobox_linux_boot_test CORE --vm-probe-rollback CLIENT
```
