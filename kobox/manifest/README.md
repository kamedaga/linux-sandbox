# Linux closure inventory

The `virtio-gpu-virgl` profile derives the complete Linux artifact closure from
one pinned build. Its root is `virtio-gpu.ko`; symbol, `modinfo`, probe, shared
provider, and ordering edges determine every other artifact.

## Pinned input

| Input | Value |
|---|---|
| Linux | `v6.18.48` (`5bbb9c9f8f808710e2123f2b30f0d61d7d698f52`) |
| Architecture | x86_64, SMP |
| Compiler | Clang 18.1.3 |
| Linker | LLD 18.1.3 |
| Kconfig | `profiles/virtio_gpu_virgl.config` |
| Config SHA-256 | `96ef6cd7abc68550cb43f00ab36845d6ccc5a11ec5dde58f0d3b5abbfb101fc5` |
| Profile | `profiles/virtio_gpu_virgl.json` |

The generated inventory is `generated/virtio_gpu_virgl.json`. It records the
kernel config digest, every `.ko` content digest and import, exact required
exports of each module and shared provider, dependency edges, and deterministic
load and cleanup orders.

## Derived closure

The current pinned build contains 11 modules and three shared providers:

| Artifact | Role |
|---|---|
| `core/primitive.so` | Linux core, device-model, and DMA primitives selected by module imports |
| `device-pci.so` | PCI, IRQ, and IOMMU providers |
| `drm.so` | dma-buf and video providers built into `vmlinux` |
| `drm_panel_orientation_quirks.ko` | DRM panel orientation data |
| `i2c-core.ko` | DRM I2C services |
| `drm.ko` | DRM core |
| `drm_kms_helper.ko` | DRM KMS helpers |
| `drm_shmem_helper.ko` | shmem-backed DRM GEM helpers |
| `virtio_dma_buf.ko` | virtio dma-buf sharing |
| `virtio_pci_modern_dev.ko` | modern virtio PCI device access |
| `virtio_ring.ko` | virtio device queue implementation |
| `virtio.ko` | virtio bus and device model |
| `virtio_pci.ko` | virtio PCI transport and device probe |
| `virtio-gpu.ko` | virtio GPU driver and VirGL command path |

`virtio_pci.ko` is an explicit probe dependency of `virtio-gpu.ko`. Device
enumeration creates this edge even though it is absent from the root module's
symbol imports and `depends` field.

The snapshot contains 935 module imports, 295 required module exports, 403
required shared-provider exports, and 38 dependency edges. The shared-provider
requirements are the build input for the corresponding `.so` artifacts; their
artifact digests enter the runtime closure manifest after those libraries are
linked.

## Regeneration

The host needs Clang/LLD 18.1.3, the elfutils `libelf` development files,
`modinfo`, and Python 3. Run from the `linux-sandbox` root:

```sh
virgl_core=/absolute/path/to/virgl-core-build
virgl_modules=/absolute/path/to/virgl-module-build

make LLVM=-18 O="$virgl_core" ARCH=x86_64 tinyconfig
scripts/kconfig/merge_config.sh -m -O "$virgl_core" \
    "$virgl_core/.config" kobox/manifest/profiles/virtio_gpu_virgl.config
make LLVM=-18 O="$virgl_core" ARCH=x86_64 olddefconfig
env KBUILD_BUILD_USER=kobox KBUILD_BUILD_HOST=kobox \
    KBUILD_BUILD_VERSION=1 \
    KBUILD_BUILD_TIMESTAMP='1970-01-01 00:00:00 +0000' \
    make LLVM=-18 O="$virgl_core" ARCH=x86_64 all

mkdir -p "$virgl_modules"
cp "$virgl_core/.config" "$virgl_modules/.config"
cp "$virgl_core/Module.symvers" "$virgl_modules/Module.symvers"
make LLVM=-18 O="$virgl_modules" ARCH=x86_64 prepare modules_prepare
linux_include="-I$PWD/kobox/provider/include -I$PWD/arch/x86/include \
-I$virgl_modules/arch/x86/include/generated -I$PWD/include \
-I$virgl_modules/include -I$PWD/arch/x86/include/uapi \
-I$virgl_modules/arch/x86/include/generated/uapi -I$PWD/include/uapi \
-I$virgl_modules/include/generated/uapi \
-include $PWD/include/linux/compiler-version.h \
-include $PWD/include/linux/kconfig.h"
make LLVM=-18 O="$virgl_modules" ARCH=x86_64 KBUILD_MODPOST_WARN=1 \
    "LINUXINCLUDE=$linux_include" modules

python3 kobox/manifest/generate_closure_inventory.py \
    --source-tree . \
    --build-dir "$virgl_modules" \
    --core-build-dir "$virgl_core" \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --nm llvm-nm-18 \
    --output kobox/manifest/generated/virtio_gpu_virgl.json
```

Validation against the checked-in result uses `--check` in place of
`--output`:

```sh
python3 kobox/manifest/generate_closure_inventory.py \
    --source-tree . \
    --build-dir "$virgl_modules" \
    --core-build-dir "$virgl_core" \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --nm llvm-nm-18 \
    --check kobox/manifest/generated/virtio_gpu_virgl.json
```

Generation fails on a missing strong export, unavailable module provider,
namespace violation, ambiguous provider ownership, config mismatch, or
dependency cycle.

The shared providers and their inventory are produced from the same canonical
build:

```sh
python3 kobox/provider/build_shared_providers.py \
    --source-tree . \
    --canonical-build-dir "$virgl_core" \
    --provider-build-dir /path/to/virgl-provider-build \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --inventory kobox/manifest/generated/virtio_gpu_virgl.json \
    --output-dir /path/to/virgl-providers \
    --protocol-include /path/to/kobox2/protocol/generated/include \
    --output-inventory /path/to/provider-inventory.json \
    --prepare --cc clang-18 --ld ld.lld --nm llvm-nm-18 \
    --readelf llvm-readelf-18 --objcopy llvm-objcopy-18
```

## Link/load gate

The link/load gate consumes the checked closure inventory, the provider
inventory, and their exact artifacts. It seals every artifact in a memfd,
verifies size, SHA-256, ELF exports, dependency order, and every module import,
then maps and relocates all 14 nodes in load order. The real gate creates a
virtio PCI function, runs `virtio_pci.ko` and `virtio-gpu.ko` probe, completes
a capset request through DMA and IRQ, and unloads the closure in reverse order.

The plan retains each `.ko` `init_module` and `cleanup_module` as
artifact-local entries. Entry-bearing modules initialize in load order and
clean up in reverse order. Execution can stop immediately before
`virtio_pci.ko` for PCI enumeration, then continue from the same cursor.

Configure the top-level `kobox2` build with all three artifact inputs:

```sh
cmake -S /path/to/kobox2 -B /path/to/build \
    -DCMAKE_C_COMPILER=clang-18 \
    -DKOBOX_VIRGL_LINUX_BUILD_DIR=/path/to/virgl-module-build \
    -DKOBOX_VIRGL_PROVIDER_DIR=/path/to/virgl-providers \
    -DKOBOX_VIRGL_PROVIDER_INVENTORY=/path/to/provider-inventory.json
cmake --build /path/to/build --target kobox_real_virgl_closure_link_test
ctest --test-dir /path/to/build -R real_virgl_closure_link \
    --output-on-failure
```

The gate uses the `dev` identity. x86-64 Linux module code and providers are
loaded in the lower 2 GiB so kernel-code-model absolute and relative
relocations remain representable.
