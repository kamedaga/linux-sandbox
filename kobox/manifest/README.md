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

The snapshot contains 928 module imports, 295 required module exports, 401
required shared-provider exports, and 37 dependency edges. The shared-provider
requirements are the build input for the corresponding `.so` artifacts; their
artifact digests enter the runtime closure manifest after those libraries are
linked.

## Regeneration

The host needs Clang/LLD 18.1.3, the elfutils `libelf` development files,
`modinfo`, and Python 3. Run from the `linux-sandbox` root:

```sh
virgl_out=/absolute/path/to/virgl-kbuild

make LLVM=-18 O="$virgl_out" ARCH=x86_64 tinyconfig
scripts/kconfig/merge_config.sh -m -O "$virgl_out" \
    "$virgl_out/.config" kobox/manifest/profiles/virtio_gpu_virgl.config
make LLVM=-18 O="$virgl_out" ARCH=x86_64 olddefconfig
env KBUILD_BUILD_USER=kobox KBUILD_BUILD_HOST=kobox \
    KBUILD_BUILD_VERSION=1 \
    KBUILD_BUILD_TIMESTAMP='1970-01-01 00:00:00 +0000' \
    make LLVM=-18 O="$virgl_out" ARCH=x86_64 all
python3 kobox/manifest/generate_closure_inventory.py \
    --source-tree . \
    --build-dir "$virgl_out" \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --nm llvm-nm-18 \
    --output kobox/manifest/generated/virtio_gpu_virgl.json
```

Validation against the checked-in result uses `--check` in place of
`--output`:

```sh
python3 kobox/manifest/generate_closure_inventory.py \
    --source-tree . \
    --build-dir "$virgl_out" \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --nm llvm-nm-18 \
    --check kobox/manifest/generated/virtio_gpu_virgl.json
```

Generation fails on a missing strong export, unavailable module provider,
namespace violation, ambiguous provider ownership, config mismatch, or
dependency cycle.
