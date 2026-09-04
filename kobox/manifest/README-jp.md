# Linux boot coreとdriver closure

`virtio-gpu-virgl` profileはbuild単位を二つに分けます。

- 一つのsandbox processにつき、`init/main.o:start_kernel`をrootにしたupstream
  `vmlinux`を一つ使う
- driver closureは`.ko`だけを選択する

driver importからLinux core codeを選んだり、Kobox shared providerとして再構成したりしません。
現行profileのLinux upper API overrideは0です。

## boot-core gate

`generate_boot_core_inventory.py`は固定したLinux identity、config、kernel release、link済み
`vmlinux`を検証します。upstreamのinitcall、per-CPU、scheduler-class、parameter、setup、
init text/data、core data sectionが空でないことを要求します。また、`start_kernel`の唯一の
source objectが`init/main.o`であることを証明し、`linux_upper_api_override_count: 0`を出力します。

`linux-sandbox` rootでbuild・検証します。

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

このgateが証明するのはboot coreのlink構造です。hosted architecture portが`start_kernel`を
実行できるという意味ではありません。

## driver-closure gate

`generate_closure_inventory.py`は`modinfo`、symbol export、soft dependency、明示したvirtio PCI
probe edgeからmodule依存を導出します。load順とcleanup順には`.ko` pathしか入りません。
`vmlinux`所有のimportは固定`linux-runtime`へのrequirementとなり、core objectやsupport
libraryの選択には使いません。

```sh
python3 kobox/manifest/generate_closure_inventory.py \
    --source-tree . \
    --build-dir "$linux_build" \
    --profile kobox/manifest/profiles/virtio_gpu_virgl.json \
    --nm llvm-nm-18 \
    --output kobox/manifest/generated/virtio_gpu_virgl.json
```

固定closureは11 moduleです。PCI、DMA、IRQ resourceはLinux APIの代替providerではなく
`linux-runtime`へ渡します。

## process lifecycle

`linux_process_lifecycle`は一つのprocessでboot transitionを一度だけ許可します。shutdownでは
module layerが排他的にquiesceしてから`_Exit`でprocessを終了します。二度目のbootやprocess内での
Linux core cleanupは行いません。

旧shared-provider builderとclosure-link fixtureはPoC toolingとして残しますが、このprofileと
CMake integration gateには含めません。
