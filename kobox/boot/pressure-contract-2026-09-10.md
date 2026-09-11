# Pressure Gate allocation contract, 2026-09-10

## Correction

The direct-reclaim positive case previously stopped at the first failed
`__GFP_NORETRY` allocation. That failure does not prove that the test cache
cannot be reclaimed. An upstream reclaim pass may make progress in other
caches and return before this shrinker receives a scan budget, while the
allocation still fails. `mm/shrinker.c:do_shrink_slab()` distinguishes counting
from scanning; `mm/vmscan.c:do_try_to_free_pages()` can finish a pass after its
reclaim target is reached. Neither a count callback nor `NULL` proves that
this cache was scanned.

`require_direct_reclaim()` now makes real allocation requests until both a
successful allocation and actual `direct_freed > 0` are observed. Every
successful page remains held. Requests retain `__GFP_NORETRY`, `__GFP_NOWARN`
and disabled `__GFP_KSWAPD_RECLAIM`. The positive case fails after 8192 requests
or the existing ten-second Gate interval, whichever is reached first. These
are test bounds, not a promise about Linux's shrinker scan schedule.

The subsequent negative cases retain their original allocation flags and
require actual allocation failure, shmem `-ENOMEM`, retained data and pins,
then recovery after pressure pages are released. The final checks still
require slab scans and no OOM kills. No shrinker callback is called manually,
no reclaim policy is changed, and no failure is converted into success.
There is no public report, host contract, OS kernel or ABI change.

## Linux verification

Candidate ELF:

```text
.artifacts/kobox2-pressure-contract-runtime/linux-boot-runtime.so
6d3104e247811ffae5d6d1864b4acfde998e798bd58831bf6244ea24fc3d0344
```

It includes the previously added failure-only diagnostics. All 735 canonical
objects, configuration and initcall inventory are unchanged:
`linux-boot-inputs.json` is byte-identical to the preserved canonical manifest.
The canonical ELF was not overwritten; its SHA256 remains
`83a85d89568e35039491f1ff584fc4fa6d0bcd2bd6ca3aaedb96ee7ee6aae8fd`.

Results from the existing Linux launcher rebuilt from current source:

| Mode | Result |
| --- | --- |
| `--pressure`, three fresh processes | PASS; direct frees 7296/6400/7552, kswapd frees 896/1792/640; each has two failures, one recovery and zero warnings |
| `--alloc-failure` | PASS; 13 injected failures, 11 rollbacks, five sweeps |
| `--all` | PASS; boot/SMP, memory, VFS, shmem, timed waits, RCU, workqueue and cleanup |
| `--vm-probe-pressure-fault` | PASS; actual delayed fault, pressure phase 6, direct frees 7296, kswapd frees 896, two revoked aliases and zero warnings |

Logs are under `.artifacts/tests/kobox2-pressure-contract/` in the enclosing
workspace. These results establish Linux regression coverage, not completion
of the PachaOS adapter or proof of its interrupted pressure case. `--all`
does not include the separate pressure or allocation-failure modes.
Build/link/ELF inspection passed with `-Werror`; checkpatch reported zero
errors and warnings.

## Reproduction

From the enclosing workspace root:

```sh
python3 kobox2/linux-sandbox/kobox/boot/build_boot_runtime.py \
  --source-tree kobox2/linux-sandbox \
  --canonical-build-dir .artifacts/kobox2-client-exec-canonical \
  --provider-build-dir .artifacts/kobox2-client-elf-hosted-provider \
  --output-dir .artifacts/kobox2-pressure-contract-runtime \
  --link --with-gates --jobs 4
cmake --build .artifacts/kobox2-client-elf-gcc-build \
  --target kobox_linux_boot_test kobox_posix_vm_client -j 4
```

Pass the absolute candidate ELF path to
`.artifacts/kobox2-client-elf-gcc-build/linux-sandbox/kobox/kobox_linux_boot_test`
with each mode above. For `--vm-probe-pressure-fault`, append the absolute path
to `kobox_posix_vm_client` in the same build directory. Run with a 90-second
outer timeout and retain each process's output and exit status.

Pressure source SHA256:
`18288532b7d5927b0829fbd001e3940c90ebb3db2bdddb579b106a7f85976181`.
Rebuilt launcher SHA256:
`a75461b0a943f4798f367d3dab61e38581fd1c0344d84f68003d049aebf69450`.
