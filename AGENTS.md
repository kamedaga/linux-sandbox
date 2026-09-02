# linux-sandbox agent notes

- Follow the upstream Linux coding style in
  [`Documentation/process/coding-style.rst`](./Documentation/process/coding-style.rst),
  including for kobox-owned C code under `kobox/`. Use the repository's
  existing `.clang-format` and `.editorconfig`; never bulk-format upstream code.
- This repository is GPL-2.0-only and is built as a process separate from the
  kobox2 controller.
- Use real, pinned Linux headers. Do not encode Linux structure offsets as
  numeric constants.
- Preserve upstream Linux paths and the pinned baseline provenance. The
  standalone repository imports a complete tagged source snapshot rather than
  the full upstream ancestry. Put only kobox-specific loader, host-contract,
  manifest, and integration code under `kobox/`.
- Prefer upstream implementations for Linux primitives and subsystems. Silent
  success stubs are forbidden; unsupported features must be rejected or abort
  through generated manifest state.
- Keep SMP enabled from the first runtime fixture. Do not build a cooperative
  coroutine approximation of kthreads, workqueues, RCU, or locking.
- Do not add PachaOS service names, role policy, package paths, or syscall
  numbers here. Host-specific adapters belong to the host OS repository.
- Do not link Apache-2.0 kobox2 controller code into this GPL-2.0-only process.
  Shared protocol files must use their independent MIT license.
- Treat the kobox host contract and shared protocols as `dev` interfaces. Do not
  assign ABI version numbers or compatibility guarantees before an explicit
  freeze.
