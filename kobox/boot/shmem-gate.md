# Upstream shmem / folio / page-cache gate

This chapter-2 step-3 gate runs after real `start_kernel()` and the mandatory
boot/service, SMP/time and memory gates. `--shmem` selects it independently;
`--all` runs VFS, shmem and every chapter-1 gate in the same boot. No subsystem
initialization, runtime replacement, new machine operation or host/wire ABI is
introduced. The fixed core still contains all 599 canonical built-in objects
and 129 retained initcall targets.

## Ownership and accounting

`shmem_gate.c` uses upstream `fs_context_for_mount()` / `fc_mount()` to create
an unattached regular tmpfs mount with a 16-page block limit and an inode limit.
Unlike `SB_KERNMOUNT` internal tmpfs, this exercises actual block and inode
accounting. Limits are parsed as mount options; the gate never modifies Linux
accounting fields. `shmem_file_setup_with_mnt()` creates an unlinked file;
`dentry_open()` creates a distinct open file referring to the same inode and
address space. The mount remains owned until its files are released.

Four cases cover both controller CPUs and both upstream accounting modes:

- `VM_NORESERVE`: incremental commit charges follow allocated backing pages.
- Flags zero: the file size is reserved before allocating backing. Size changes
  go through `vfs_truncate()` / `notify_change()` / shmem setattr. Writes stay
  inside that reservation. Preallocation and write-beyond-EOF races are tested
  on incremental tmpfs, not misapplied to fixed-size pre-reserved objects.

At quiescent operation boundaries, assertions compare `i_size`, mapping
`nrpages`, shmem `alloced` / `swapped`, `i_blocks`, the exact per-CPU sum of
mount `used_blocks`, inode `free_ispace`, and global committed-memory charges.
The inode charge is measured from upstream accounting, not copied from a
private structure constant. After final close, all mount data/inode charges
and committed memory must return to the pre-file baseline. Counters are never
expected to be an atomic snapshot during concurrent mutation.

## Required behavior

- Sparse reads return zeros without allocating pages; `SGP_READ`,
  `SGP_NOALLOC` and out-of-file `SGP_CACHE` have their distinct upstream results.
  An explicitly allocated cache folio is entirely zero and uptodate.
- Two independent opens observe patterned cross-page data and identical EOF
  boundaries. Reads leave the untouched part of the destination unchanged.
- An unaligned punch spanning four pages removes its two complete pages and
  zeros only the edge ranges. File size and unaffected bytes remain unchanged.
  A punch beyond EOF neither grows nor allocates the file.
- Truncation through a hole, inside a nonzero allocated page, and at an exact
  page boundary releases the appropriate pages and charges. Regrowth exposes
  zeros, including the discarded partial-page tail, not the previous contents.
- Invalid truncate length, fallocate offsets/length/modes/overflow and
  no-allocation/out-of-file folio requests fail without changing accounting.
- Two CPU-bound real kthreads acquire the same initially absent offset and
  perform 256 locked increments per case. They must obtain the same PFN and
  leave exactly 256 increments in the shared backing, not separate copies.
- Two different upstream `vmap()` aliases of that pinned shmem page observe
  the same data as file I/O. Truncate detaches the page cache entry and returns
  its charges while the aliases and their explicit folio reference retain RAM.
- With a folio locked on one CPU, remote truncate and hole punch must actually
  enter an uninterruptible wait, without completing or detaching that folio.
  Unlock permits completion and correct reclamation. This is a deterministic
  contention check in addition to timing-dependent stress.
- In 64 rounds per case, two kthreads issue read/write operations alongside
  controller truncate/regrow or hole punch. The reader accepts valid short
  reads and bytewise old/new/zero data, not an unsupported atomic-snapshot
  guarantee. Every operation must finish; final shrink/regrow is empty and zero.
- Incremental tmpfs preallocation beyond EOF with `KEEP_SIZE` preserves size;
  subsequent growth reads zeros. A second file fills all but one available
  block, forcing a multi-page fallocate to fail partway with `ENOSPC`. Its new
  pages and commit charges must roll back, leaving prior dirty data, size and
  the second file's allocation intact. Closing the second file restores space.

## Reclamation oracle and scope

Hole punch, truncate, pinned aliases and final close retain explicit folio
references while checking detachment and preserved data. After aliases are
unmapped and upstream lazy aliases drained, the last reference is released.
Fresh buddy allocations on both CPUs, using the original mapping GFP class,
must reacquire the saved numeric PFN. No freed object is dereferenced and no
allocator behavior is replaced. Five physical-page reclaims are required per
case, twenty per invocation; dropping a mapping reference alone cannot pass.

Workers finish each operation before its state is reused and are stopped and
joined before files or worker storage are freed. Private file references use
upstream `__fput_sync()`; deferred-fput and inode/superblock lifetime behavior
remain covered by the separate VFS gate. Failure preserves potentially live
storage until the launcher's immediate process exit.

This is the current order-0, no-swap profile; the gate explicitly rejects THP
or swap configurations rather than applying these exact accounting oracles to
different semantics. It does not certify swap, memcg, global memory pressure,
user VMA faults/unmapping, cross-process mappings, GEM or external DRM clients.
The aliases here are pinned kernel mappings, not user VMAs that truncate revokes.

Upstream ownership is auditable in [shmem](../../mm/shmem.c),
[VFS truncate/fallocate](../../fs/open.c) and [page cache](../../mm/filemap.c).
Builder tests reject replacements for these implementations and the load audit
requires their real symbols alongside the gate entry.

## Verification

Run `kobox_linux_boot_test /absolute/path/linux-boot-runtime.so --shmem` and
`--all`, or CTest's `kobox2.linux_shmem_pagecache_gate` and
`kobox2.linux_full_foundation_gate`. The boot-core load audit is a prerequisite;
link/load success alone is not runtime evidence. Any Linux warning fails.

The verified artifact is
`.artifacts/kobox2-shmem-pagecache-runtime/linux-boot-runtime.so` in the workspace,
SHA-256 `d2a1d0a56892bcf27a71c95e9c81b6a289551c1494711f7ccfedbc51f02ef746`.
Its unchanged canonical configuration hash is
`5e8618e196f9096b3a149a22ac6869b8f2e2b671a2508ec0ca7a75ccf0a09d60`.
The strict build/load audit, eight builder tests, checkpatch and all 44 CTests
passed. Fifty standalone shmem runs all reported four cases, 64 I/O checks,
98 accounting checks, 16 sharing checks, 32 negative checks, 1,024 increments,
256 race rounds, eight observed lock waits, twenty physical-page reclaims,
two allocation rollbacks and zero warnings. Five additional `--all` runs passed
VFS, shmem, timed waits, RCU, workqueue and cleanup sequentially in each boot,
also with zero warnings. Verified run logs are `repeat-1.log` through
`repeat-50.log` and `all-1.log` through `all-5.log` in the artifact directory;
the complete suite log is `.artifacts/kobox2-shmem-pagecache-ctest.log`.
