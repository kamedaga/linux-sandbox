# Linux boot core and driver closure

The `virtio-gpu-virgl` profile fixes two separate build units:

- one upstream `vmlinux`, rooted at `init/main.o:start_kernel`, is the Linux
  runtime for one sandbox process;
- the driver closure selects `.ko` artifacts only.

Linux core code is not selected from driver imports and is not rebuilt as
Kobox shared providers. The active profile declares no Linux upper-API
override.

## Boot-core gate

`generate_boot_core_inventory.py` verifies the pinned Linux identity, config,
kernel release, and the linked `vmlinux`. It requires non-empty upstream
initcall, per-CPU, scheduler-class, parameter, setup, init text/data, and core
data sections. It also proves that `start_kernel` has the unique source object
`init/main.o` and reports `linux_upper_api_override_count: 0`.

Build and verify from the `linux-sandbox` root:

```sh
linux_build=/absolute/path/to/linux-build

make LLVM=-18 O="$linux_build" ARCH=x86_64 tinyconfig
scripts/kconfig/merge_config.sh -m -r -O "$linux_build" \
    "$linux_build/.config" kobox/manifest/profiles/virtio_gpu_virgl.config
make LLVM=-18 O="$linux_build" ARCH=x86_64 olddefconfig
make LLVM=-18 O="$linux_build" ARCH=x86_64 vmlinux modules

python3 kobox/manifest/generate_boot_core_inventory.py \
    --source-tree . \
    --build-dir "$linux_build" \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --nm llvm-nm-18 \
    --readelf llvm-readelf-18
```

This gate proves the boot core's link structure. It does not claim that the
hosted architecture port can execute `start_kernel` yet.

## Driver-closure gate

`generate_closure_inventory.py` derives module dependencies from `modinfo`,
symbol exports, soft dependencies, and the explicit virtio PCI probe edge.
The resulting load and cleanup orders contain only `.ko` paths. Imports owned
by `vmlinux` become requirements of the fixed `linux-runtime`; they never
select core object files or support libraries.

```sh
python3 kobox/manifest/generate_closure_inventory.py \
    --source-tree . \
    --build-dir "$linux_build" \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --nm llvm-nm-18 \
    --output kobox/manifest/generated/virtio_gpu_virgl.json
```

The pinned closure contains 11 modules. PCI, DMA, and IRQ resources terminate
at `linux-runtime`, not at a replacement Linux API provider.

## Process lifecycle

`linux_process_lifecycle` permits exactly one boot transition per process.
Shutdown first gives the module layer exclusive quiesce ownership and then
terminates the process with `_Exit`; there is no second boot or in-process
Linux-core cleanup.

The old shared-provider builder and closure-link fixture remain PoC tooling,
but they are not part of this profile or its CMake integration gate.
