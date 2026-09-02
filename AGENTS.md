# linux-sandbox agent notes

- This repository is GPL-2.0-only and is built as a process separate from the
  kobox2 controller.
- Use real, pinned Linux headers. Do not encode Linux structure offsets as
  numeric constants.
- Preserve upstream Linux paths and history. Put only kobox-specific loader,
  host-contract, manifest, and integration code under `kobox/`.
- Prefer upstream implementations for Linux primitives and subsystems. Silent
  success stubs are forbidden; unsupported features must be rejected or abort
  through generated manifest state.
- Keep SMP enabled from the first runtime fixture. Do not build a cooperative
  coroutine approximation of kthreads, workqueues, RCU, or locking.
- Do not add PachaOS service names, role policy, package paths, or syscall
  numbers here. Host-specific adapters belong to the host OS repository.
- Do not link Apache-2.0 kobox2 controller code into this GPL-2.0-only process.
  Shared protocol files must use their independent MIT license.
