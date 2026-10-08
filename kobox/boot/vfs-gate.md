# Upstream VFS object lifetime gate

Chapter 2, step 2 executes the upstream mount, pathname, file, inode and
address-space paths in the fixed real-shmem boot core. There is **no new runtime
port, host operation, VFS implementation, filesystem-operations wrapper or
initialization fixture**. The canonical configuration and all 599 built-in
objects / 129 initcall targets remain unchanged from step 1. The builder now
requires their VFS source owners and rejects VFS, fput, iput and task-work
replacements.

The separate opt-in storage port below does not change this lifetime workload.

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

## Storage port and differential gate (dev)

`fs_port.c` is an opt-in GPL, process-local port (`--device-profile storage`).
It owns a reference to a granted mount and a dynamically allocated xarray of
`struct file` references. Handles are nonzero, monotonically increasing and
never reused within a port. The table mutex protects lookup plus `get_file()`
and removal; no filesystem I/O holds that mutex. Dup retains the same file,
including its upstream flags and offset. Offset operations follow Linux's
`FMODE_ATOMIC_POS` locking contract, including the union used by nonseekable
files. Concurrent close cannot invalidate a reference already acquired by I/O.
Callers must retain the port and immutable credentials throughout each call,
and quiesce callers before destroying the port.

Every request uses `override_creds()` / `revert_creds()`. Open uses the pinned
`build_open_flags()` and `do_file_open_root()`, including openat2 resolve flags.
Read/write use `kernel_read()` / `kernel_write()` after checking their required
access modes. Other operations use `iterate_dir()`, `vfs_getattr()`, upstream
parent lookup and mutation helpers, `vfs_get_link()`, `do_ftruncate()` and
`vfs_fsync()`. Getdents returns aligned `linux_dirent64` records, without a
fixed name array or an artificial transfer limit. The previous record and
final record receive upstream directory cookies; padding is zeroed.

This is **not a wire ABI or a complete syscall adapter**. Paths currently use
the granted mount as their root, not an arbitrary openat directory handle.
The getattr API returns a real `kstat`; Linux's private `vfs_statx()` wrapper
and its mount-ID augmentation are not exported by this interface. Controller
credential validation, request umask/context, full at-style/schema semantics,
device authorization/UUID selection, and hardware package/service integration
belong to the subsequent controller integration. No Linux structures cross
the controller protocol boundary.

`manifest/profiles/storage.config` enables modular ext4, jbd2, mbcache and
crc16. It is separate from existing pinned device profiles. The test-only
`storage_test.config` instead embeds those implementations and one 32 MiB brd
disk; it fixes base-page/no-swap conditions for the existing shmem gate.
The storage core grants no GPU or virtio device. It omits the unrelated virtio
device gate, rather than inventing sync-file or hardware stubs.

From the kobox2 root, use LLVM/LLD 18.1.8 and the matching Kbuild host libelf
environment. `HOST_CC` selects the POSIX test compiler; `MUSL_CC` selects the
static native reference compiler (set `REALGCC` when using a musl wrapper).
The output directory must be fresh because provider caches pin canonical
config, vmlinux and archive identities:

```sh
export HOST_CC=/usr/bin/cc
export MUSL_CC=/usr/bin/musl-gcc
export REALGCC=/usr/bin/x86_64-linux-gnu-gcc
OUT=/absolute/path/to/fresh-artifacts/fs-port
bash linux-sandbox/kobox/boot/build_fs_port_test.sh "$OUT"
python3 linux-sandbox/kobox/boot/test_fs_port.py \
  --core "$OUT/runtime/linux-boot-runtime.so" \
  --host-test "$OUT/host/linux-sandbox/kobox/kobox_linux_boot_test" \
  --native-test "$OUT/native-test" --iso /path/to/alpine-virt.iso \
  --out "$OUT/results" --new-run
```

The launcher runs after real `start_kernel()` and mandatory boot/memory gates.
It copies a **new disposable ext4 image**, not a rootfs, into brd through real
block-file I/O, mounts ext4 through `fs_context`, and tests tmpfs separately.
It drains task work, delayed fput and RCU, unmounts ext4, invalidates the block
cache and exports the resulting disk before the host syncs its mapping.

One shared C workload runs through the port and native Linux syscalls in a
serial-console Alpine guest, chrooted into fresh ext4 and tmpfs mounts. It
checks sparse/positioned I/O, truncate and extension, append, dup offsets,
open-unlink lifetime, O_PATH errors, UID/GID/supplementary-group DAC, sticky
directories, rename/exchange/noreplace, symlinks and resolve restrictions,
255/256-byte names, short directory buffers, cookies, names/types/duplicates,
and fsync/fdatasync. Directory order and padding are deliberately not treated
as a contract: ext4 hash seeds and syscall buffer padding may differ.

Additional port-only tests retain 10,000 simultaneous handles per filesystem,
reject stale handles, perform 512 shared-offset reads across both CPUs in
total, and race read against close 1,024 times. Linux warnings fail the gate.
Both exported ext4 images must pass `e2fsck -fn`, with persisted renamed and
credential-owned data verified by debugfs. Results keep executable/source
snapshots, hashes, core inventory, native QEMU arguments and console logs.

For CTest, configure `KOBOX_LINUX_STORAGE_RUNTIME_CORE`, `KOBOX_FS_NATIVE_TEST`
and `KOBOX_FS_NATIVE_ISO`; `kobox2.linux_fs_port` keeps a fresh results directory
on each run. Separately, `build_fs_port_test.sh NEW_OUTPUT --modules-only`
builds and checks the real four-module closure without brd or builtin ext4.
