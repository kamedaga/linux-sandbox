# Boot-integrated memory and real shmem

Chapter 2, step 1 extends the **fixed, boot-rooted** core, not the standalone
memory/task closures. Merge `kobox/boot/config` after the canonical base and
`kobox/task/config`, then rebuild the canonical Linux image and its complete
hosted core. The fragment enables `SHMEM`, `TMPFS` and `MEMFD_CREATE` with MMU;
the builder also requires SLUB and sparse vmemmap. Tiny shmem, modular/missing
required features and upper VFS/MM replacements are rejected.

`SHMEM=y` selects the full upstream implementation, not the ramfs-based tiny
branch. `TMPFS=y` provides its filesystem operations and selects memfd support.
The current profile has no block/swap backend; enabling real shmem does not
pretend swap exists or certify memfd's userspace syscall entry.

## Boot ownership

Linux's `start_kernel()` still owns memory and scheduler initialization.
`vfs_caches_init()` reaches `mnt_init()`, which initializes shmem and rootfs.
Reclaim services such as kswapd come from upstream initcalls. The machine port
only supplies the existing RAM, arch, CPU, IRQ and clock bindings. Tests neither
mount an artificial replacement for `shm_mnt` nor call `shmem_init()`,
`mnt_init()`, `pagecache_init()`, SLUB initialization or other service startup.
Those entry points are additionally forbidden in the port/test link audit.

`memory_gate.c` is required by **every** real-boot launcher invocation, after
the existing boot/service/task/SMP/time verification and before the selected
chapter-1 detailed gate. It requires PID 1, `SYSTEM_RUNNING`, completed RCU boot,
two online CPUs, an upstream root mount, available SLUB/RAM and upstream kswapd
tasks. Their presence is not a claim of full pressure/reclaim coverage.

## Mandatory observations

- On each CPU, order-0 through order-4 page allocations must be zeroed and have
  correct alignment and page/PFN/direct-map/physical-address round trips.
- Six heap sizes on each CPU cover small allocations through 65,537 bytes.
  A dedicated real SLUB cache has 32 simultaneously live, independently stamped
  objects per CPU. All these allocations are released through Linux.
- Static per-CPU storage and four maximum-unit dynamic allocations are accessed
  on **both executing CPUs** using upstream SMP calls. CPU/current, distinct
  addresses and each unit's first/last words must match. At least one allocation
  must lie in the dynamic vmalloc area, so the embedded first chunk alone cannot
  pass. The report counts vmalloc-backed allocations, not distinct chunk objects.
- A two-page vmap shares bytes with the direct map in both directions across
  CPUs, and resolves to the original Linux pages. After unmap and lazy-alias
  flushing, an exception-table-protected read must fault before pages are freed.
- A sparse five-page virtual area maps pages 0, 2 and 4. A broad map/cache flush
  must tolerate holes; broad pre-unmap and post-clear TLB flushes around removing
  page 2, including a full-window TLB flush, must revoke that page without
  revoking pages 0 and 4. Holes must fault.
- On each CPU, `shmem_file_setup()` must use the real initialized shmem mount.
  `shmem_mapping()` must be true; allocating a folio must populate page cache,
  mark it swap-backed/uptodate, and supply initially zero bytes. Repeated lookup
  returns the same data. Synchronous final put of this private internal file
  must evict it from the mapping while the test safely retains a folio reference;
  the final `folio_put()` then releases that reference. No freed inode is read.

Normal counts are 10 page cases, 12 heap cases, 64 slab objects, 8 per-CPU probes,
11 alias checks and 2 shmem cases. Four vmalloc-backed per-CPU allocations are
observed in the current profile. Unexpected Linux warnings fail the gate.
Failures retain possibly live test allocations until immediate process exit.

## Machine mapping correction found by this gate

Dynamic per-CPU allocation exposed a faulty assumption in the old memory port:
`flush_cache_vmap()` spans can contain unpopulated space between CPU units.
The port now maps only pages present in Linux's page tables. It also no longer
revokes an entire span at **pre-unmap** cache flush: Linux has not cleared its
PTEs yet, and other live mappings may lie within that span.

Host revocation instead follows `flush_tlb_kernel_range()` / `__flush_tlb_all()`
after PTE removal. Cleared ranges become inaccessible; still-present pages are
preserved. Host publication and invalidation are serialized so a coalesced flush
cannot erase a concurrent publisher's new host mapping. The host map/reset
contract requires leaf memory operations with no reentry or wait for hosted
Linux work, including when guest IRQs/preemption are disabled. No host operation
signature or wire ABI changes.

The full-window check also caught the distinction between inclusive `VMALLOC_END`
and the exclusive end of a TLB range. The added regression trapped before the
boundary correction; the port now converts that endpoint explicitly.

Linux vmalloc legitimately batches invalidation. Both this gate and the chapter-1
cleanup gate therefore call upstream `vm_unmap_aliases()` before asserting that
a retired alias is inaccessible. The cleanup gate's admission, IRQ/timer/work/
RCU synchronization and post-free producer checks remain intact. This corrects
the oracle to the actual upstream contract; it does not weaken its post-flush
late-access check. Page tables, vmalloc/per-CPU allocators and reclamation policy
remain upstream. This is not general user-mm/VMA/fault/protection support.

## Build and regression

Use fresh artifact directories for the changed canonical configuration; old
chapter-1 artifacts need not be overwritten. For an existing canonical base:

```sh
scripts/kconfig/merge_config.sh -m -O /path/to/new-canonical \
    /path/to/base/.config kobox/task/config kobox/boot/config
make O=/path/to/new-canonical LLVM=-18 olddefconfig
make O=/path/to/new-canonical LLVM=-18 -j4 vmlinux
python3 kobox/boot/build_boot_runtime.py --source-tree . \
    --canonical-build-dir /path/to/new-canonical \
    --provider-build-dir /path/to/new-hosted \
    --output-dir /path/to/new-runtime --link
```

Create the new output directory before merging. Native Kbuild requires its
normal host dependencies (including libelf headers/libraries for objtool).
Point CMake's `KOBOX_LINUX_BOOT_RUNTIME_CORE` at the new core. The isolated
memory/task fixture paths can stay as historical regression inputs, but their
manual initialization is not used by this gate or its completion evidence.

`kobox_linux_boot_test CORE.so --all` runs the boot/service/SMP/time gate, this
memory gate, timed waits, Tree RCU/SRCU, workqueue and integrated cleanup in
**one Linux boot**. CTest adds `kobox2.linux_full_foundation_gate`; individual
chapter-1 gates remain available for diagnostics. The current canonical input
set has 599 built-in objects and 129 retained initcall targets. The extra native
object is memfd; the full shmem implementation replaces the tiny branch within
the existing `mm/shmem.o`, without closure-based pruning.

Verification on 2026-09-06 with this real-shmem profile:

- Native canonical image and strict hosted-core build passed; ELF checks retain
  all 129 initcall targets and reject imports of host implementations.
- The eight boot-builder tests passed, including rejection of tiny/missing/
  modular required memory features and upper-subsystem/startup replacements.
- All 42 configured CTests passed after both mapping-boundary corrections.
- One complete boot reports the memory counts above, 350 timed-wait cases,
  56 RCU/SRCU cases (112 callbacks, 168 frees), 211 workqueue cases (1,456
  callbacks, rescuer progress on both CPUs), and 52 cleanup cases (20 held
  synchronization probes, 52 frees), with no Linux warnings.
- Ten additional fresh processes each ran `--all` successfully with the same
  final core, including the sparse and full-window TLB regression.

This completes only the boot/configuration step when all these gates pass.
Detailed VFS operations, shmem truncation/pressure, user mappings, GEM and
external-client sharing retain their own chapter-2/3 gates.
