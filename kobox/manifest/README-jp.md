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

snapshotには935個のmodule import、295個の必須module export、403個の必須shared-provider
export、38本のdependency edgeがあります。shared providerのrequirementは対応する`.so`
artifactのbuild入力です。そのlibraryをlinkした時点でartifact digestをruntime closure
manifestへ入れます。

## 再生成

hostにはClang/LLD 18.1.3、elfutilsの`libelf`開発file、`modinfo`、Python 3が必要です。
`linux-sandbox` rootで実行します。

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

check済みの結果との検証では、`--output`を`--check`へ置き換えます。

```sh
python3 kobox/manifest/generate_closure_inventory.py \
    --source-tree . \
    --build-dir "$virgl_modules" \
    --core-build-dir "$virgl_core" \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --nm llvm-nm-18 \
    --check kobox/manifest/generated/virtio_gpu_virgl.json
```

必須exportの欠落、module providerの不在、namespace違反、provider所有権の曖昧性、config
不一致、dependency cycleを検出すると生成を失敗させます。

同じcanonical buildからshared providerとそのinventoryを生成します。

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

## link/load gate

link/load gateは検証済みclosure inventory、provider inventory、その正確なartifactを入力にします。
全artifactをmemfdへsealし、size、SHA-256、ELF export、dependency順、全module importを検証してから、
14 nodeをload順にmap・relocateします。実gateはvirtio PCI functionを生成し、
`virtio_pci.ko`と`virtio-gpu.ko`のprobe、DMAとIRQによるcapset requestの完了、closureの
逆順unloadまで実行します。

planは各`.ko`の`init_module`と`cleanup_module`をartifact固有entryとして保持します。entryを持つ
moduleはload順にinitし、逆順にcleanupします。PCI enumeration用に`virtio_pci.ko`直前で実行を止め、
同じcursorから再開できます。

top-levelの`kobox2` buildへ三つのartifact入力をすべて指定します。

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

gateのidentityは`dev`です。x86-64 Linux moduleのkernel code modelが使うabsolute・relative
relocationを表現できるように、Linux moduleとproviderをlower 2 GiBへloadします。
