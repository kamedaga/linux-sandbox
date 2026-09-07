# Linux early-memory gate

This gate boots the upstream Linux `memblock`, sparse vmemmap, buddy, SLUB,
per-CPU allocator, and `vmap` code on shared POSIX backing used as Linux RAM.

Only the following boundary is host-specific:

- mapping one backing into the direct-map, vmemmap, and vmalloc-alias windows;
- placing `page_offset_base`, `vmemmap_base`, `vmalloc_base`, and `phys_base`;
- bounding `VMALLOC_END` by the actual reserved host window, for both allocation
  and upstream `is_vmalloc_addr()`/`kvfree()` classification;
- current-CPU, IRQ-disable, and bootstrap-task state during early boot; and
- host alias publication/invalidation at Linux's cache/PTE/TLB boundaries.

The allocators in `linux-early-memory-gate.so` come from Linux objects. The inventory
pins every gate symbol to its source object. Imports into later task, workqueue,
RCU, IRQ, reclaim, and OOM phases are isolated in
`linux-early-memory-boundary.so`.
That test fixture terminates with a diagnostic on function calls and places
data behind `PROT_NONE`, so entering an unavailable phase cannot produce a
passing gate. The early-boot `__cond_resched()` path also stops if it observes
an actual reschedule request.

The gate executes and checks:

- `mm_core_init()` and SLUB availability;
- `alloc_pages()` plus page/PFN/direct-map round trips;
- `kmalloc()` and a dedicated `kmem_cache`;
- distinct static and dynamic per-CPU storage for two logical CPUs;
- retained early CPU-hotplug callbacks for the page allocator and radix tree; and
- a two-page `vmap()` whose writes alias the same RAM backing both ways.

The alias check also rejects direct RAM and core-image addresses as vmalloc
addresses. A native 32-TiB upper bound over a small host window can misclassify
ordinary slab allocations, even when an individual `vmap()` succeeds.

Set `KOBOX_LINUX_BOOT_BUILD_DIR` to the canonical Linux build to add
`kobox2.linux_early_memory_gate` to the regular CMake build and CTest run. This
gate makes no claim about the task port, reclaim, timers, RCU, IRQs, or
workqueues, and it is not the final Linux boot-core DSO.

The current [boot memory gate](../boot/memory-gate.md) verifies these allocations
after real upstream boot, also forcing dynamic per-CPU vmalloc chunks and using
real shmem. It found and corrected sparse per-CPU flush spans in this machine
port. Host revocation now follows post-clear TLB invalidation; tests requiring
inaccessible retired aliases explicitly drain upstream lazy vmalloc aliases.
The standalone fixture is historical regression coverage, not completion
evidence for boot/service initialization.
