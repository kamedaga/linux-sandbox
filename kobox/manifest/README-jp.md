# Linux closure inventory

`virtio-gpu-virgl` profileは、一つの固定buildからLinux artifact closure全体を導出します。
rootは`virtio-gpu.ko`です。symbol、`modinfo`、probe、shared provider、orderingのedgeから
残りのartifactを確定します。

## 固定する入力

| 入力 | 値 |
|---|---|
| Linux | `v6.18.48` (`5bbb9c9f8f808710e2123f2b30f0d61d7d698f52`) |
| architecture | x86_64、SMP |
| compiler | Clang 18.1.3 |
| linker | LLD 18.1.3 |
| Kconfig | `profiles/virtio_gpu_virgl.config` |
| config SHA-256 | `96ef6cd7abc68550cb43f00ab36845d6ccc5a11ec5dde58f0d3b5abbfb101fc5` |
| profile | `profiles/virtio_gpu_virgl.json` |

生成結果は`generated/virtio_gpu_virgl.json`です。kernel config digest、全`.ko`のcontent
digestとimport、各moduleとshared providerに必要なexport、dependency edge、決定的なload順と
cleanup順を記録します。

## 導出したclosure

現在の固定buildは11個のmoduleと3個のshared providerを含みます。

| artifact | 役割 |
|---|---|
| `core/primitive.so` | module importから選択したLinux core、device model、DMA primitive |
| `device-pci.so` | PCI、IRQ、IOMMU provider |
| `drm.so` | `vmlinux`へ組み込まれたdma-bufとvideo provider |
| `drm_panel_orientation_quirks.ko` | DRM panel orientation data |
| `i2c-core.ko` | DRMのI2C service |
| `drm.ko` | DRM core |
| `drm_kms_helper.ko` | DRM KMS helper |
| `drm_shmem_helper.ko` | shmem-backed DRM GEM helper |
| `virtio_dma_buf.ko` | virtio dma-buf共有 |
| `virtio_pci_modern_dev.ko` | modern virtio PCI device access |
| `virtio_ring.ko` | virtio device queue実装 |
| `virtio.ko` | virtio busとdevice model |
| `virtio_pci.ko` | virtio PCI transportとdevice probe |
| `virtio-gpu.ko` | virtio GPU driverとVirGL command path |

`virtio_pci.ko`は`virtio-gpu.ko`の明示的なprobe依存です。device enumerationがこのedgeを
作るため、root moduleのsymbol importと`depends` fieldには現れません。

snapshotには928個のmodule import、295個の必須module export、401個の必須shared-provider
export、37本のdependency edgeがあります。shared providerのrequirementは対応する`.so`
artifactのbuild入力です。そのlibraryをlinkした時点でartifact digestをruntime closure
manifestへ入れます。

## 再生成

hostにはClang/LLD 18.1.3、elfutilsの`libelf`開発file、`modinfo`、Python 3が必要です。
`linux-sandbox` rootで実行します。

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

check済みの結果との検証では、`--output`を`--check`へ置き換えます。

```sh
python3 kobox/manifest/generate_closure_inventory.py \
    --source-tree . \
    --build-dir "$virgl_out" \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --nm llvm-nm-18 \
    --check kobox/manifest/generated/virtio_gpu_virgl.json
```

必須exportの欠落、module providerの不在、namespace違反、provider所有権の曖昧性、config
不一致、dependency cycleを検出すると生成を失敗させます。
