# Upstream VFS object lifetime gate

Chapter 2, step 2 executes the upstream mount, pathname, file, inode and
address-space paths in the fixed real-shmem boot core. There is **no new runtime
port, host operation, VFS implementation, filesystem-operations wrapper or
initialization fixture**. The canonical configuration and all 599 built-in
objects / 129 initcall targets remain unchanged from step 1. The builder now
requires their VFS source owners and rejects VFS, fput, iput and task-work
replacements.

Run `kobox_linux_boot_test CORE.so --vfs`, or CTest
`kobox2.linux_vfs_lifetime_gate`. `--all` also runs this gate, after the mandatory
boot/service/SMP/memory checks and before the chapter-1 detailed regressions.
The core must be rebuilt with the new gate; an old ELF cannot pass the updated
load check just by retaining the old memory entry point.

## Operations and references

The eight cases cover both CPUs, task-work versus kthread delayed fput, and
closing a Linux FD before versus after unlinking its name. Every case:

- Gets the boot-registered tmpfs type and mounts it with `kern_mount()`. No
  filesystem registration, cache initialization or service startup is invoked.
- Creates a named file with `file_open_root_mnt()`. `kernel_write()` writes an
  offset, three-page pattern; `kernel_read()` checks its initial zero gap,
  contents, position and EOF through separately opened files.
- Finally closes a linked file without retaining a file/inode/folio reference,
  runs task work, then reopens the name and checks persistent data before unlink.
- Installs a genuine Linux FD, checks `fget()` / `get_file()` reference counts,
  closes the FD and verifies that a separately held reference still permits I/O.
- Unlinks an open file and requires pathname lookup to fail while the old
  reference still works. Recreating the same name must create a different inode
  without replacing the old file's data.
- Checks `-EEXIST`, missing lookup/unlink, repeated close (`-EBADF`) and a
  read-only write rejection. That last check uses the upstream `vfs_write()`
  mode check with a NULL user buffer; it does not exercise user copying.
  `kernel_write()` instead requires a writable caller and warns on misuse.
- Releases the mount owner's reference with `kern_unmount()` while an explicit
  `mntget()` reference and an open file keep the mount usable. The retained
  mount is not finally put until all dependent inode references are gone.

An `address_space` is embedded in its inode, not independently reference-counted.
The test keeps it alive through a real `igrab()` reference after the final file
release. Mapping identity, page-cache occupancy and data must remain intact.
Independent folio references then retain the pages after the last `iput()` has
evicted the inode and detached its page cache. They do **not** keep the inode or
mapping alive. The folios must retain their bytes, have no mapping, and have only
the test reference before their final puts.

## Deferred completion and actual reclamation

For the ordinary-task path, real hosted PID 1 is not `PF_KTHREAD`: final `fput()`
must queue task work, leave the inode's dentry reference alive before the
explicit `task_work_run()` boundary, and drop it afterwards. This is a
kernel-side harness boundary, not a claim that a userspace syscall-return path
has already been implemented.

For the kthread path, a real upstream kthread on the other CPU performs final
`fput()`. The test first observes inode-reference release by the real delayed
worker, then calls `flush_delayed_fput()` to wait for any in-flight completion.
The controller holds no locks or dependency needed by these private files'
unmount/release operations. No test callback runs the delayed worker for it.

Before final `iput()`, another real kthread holds an RCU read-side section.
After eviction it observes `I_FREEING | I_CLEAR` and an empty mapping while
still protecting the retired inode's storage. A queued marker must not have run.
The reader never sleeps voluntarily in its RCU section; it has a bounded failure
deadline. After releasing and joining it, `rcu_barrier()` must complete the
callbacks. This marker is only an ordering observation, not the reclamation
oracle.

Fresh upstream allocations must then reuse the retired file and inode storage.
Independent zero-length shmem files keep a companion object in each target slab,
preventing whole empty-slab destruction from moving that storage to another
cache before observation. Companions belong to the separate internal shmem
mount; they hold **no reference to the target**. They neither alter allocator
policy nor substitute free operations. Reuse probes are simultaneously retained,
bounded, and performed on both CPUs. No freed object is dereferenced. Temporary
probe/companion files are synchronously put, then their RCU callbacks drained.

This distinction matters for `filp`: its `SLAB_TYPESAFE_BY_RCU` cache protects
slab storage, not each old file object's identity. A file object may be reused
without waiting for a GP. The inode's own RCU-delayed free is a different rule.

Final folio puts are followed by real page allocations that must reacquire all
three PFNs. Probes use the original mapping's GFP zone/mobility class; unrelated
unmovable allocations are not a valid probe of shmem's movable PCP lists.

Finally, releasing the last mount reference destroys its superblock through
upstream RCU **then system work then `kfree()`**. A GP alone is insufficient.
Fresh, bounded allocations must reuse its storage after that work progresses.
The test neither flushes a system-wide workqueue nor dereferences a potentially
freed `destroy_work` pointer. Probe allocation failure or failure to observe
reclamation fails the gate; it is never treated as a successful free.

## Scope and regression

Expected totals: 8 mounts/cases, 56 I/O checks, 8 linked-file reopens, 40 negative
checks, 4 task-work and 4 delayed-fput cases, 8 RCU holds, 8 file reclaims,
8 inode reclaims, 24 reacquired page PFNs and 8 superblock reclaims. Unexpected
Linux warnings fail the gate. Failure retains possibly referenced test storage
until the launcher's immediate process exit.

Build with the step-1 real-shmem canonical profile and a fresh runtime output
directory using `build_boot_runtime.py --link`; update CMake's
`KOBOX_LINUX_BOOT_RUNTIME_CORE` to that core. No Kconfig or host/wire ABI change
is required. The expanded builder checks also protect VFS implementation owners.

Verified on 2026-09-06: strict core build/load with 129 retained initcall targets,
all 8 boot-builder tests, all 43 configured CTests, and 50 additional fresh-process
VFS runs passed. The dedicated gate's C/header pass checkpatch without warnings.
All VFS runs reported the complete counts above and no Linux warnings.
Ten additional fresh-process `--all` runs also passed: every run executed this
VFS gate and all chapter-1 detailed gates in the same boot, without warnings.

This gate covers kernel-side VFS calls and Linux FD lifetime in the sandbox.
It does not certify a userspace syscall entry, user-copy fault handling,
mount-namespace propagation, shared mappings, memory-pressure policy, GEM,
external-client FDs or DRM IPC. Those retain their own implementation gates.
