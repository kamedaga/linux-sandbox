#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || ( $# -eq 2 && "$2" != --modules-only ) ]]; then
    echo "usage: $0 NEW_OUTPUT_DIRECTORY [--modules-only]" >&2
    exit 2
fi
source_tree="$(cd "$(dirname "$0")/../.." && pwd)"
output="$(realpath -m "$1")"
# Provider caches pin configuration, vmlinux and archive identities. Use a
# separate fresh build rather than changing a production package's cache.
if [[ -e "$output" ]]; then
    echo "output already exists: $output" >&2
    exit 2
fi
mkdir -p "$output/canonical"
canonical="$output/canonical"
jobs="${JOBS:-8}"
entry="$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" \
    --source-tree "$source_tree" --build-dir "$canonical")"
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$entry" tinyconfig
fragments=("$source_tree/kobox/task/config" "$source_tree/kobox/boot/config"
           "$source_tree/kobox/manifest/profiles/storage.config")
if [[ "${2:-}" != --modules-only ]]; then
    fragments+=("$source_tree/kobox/manifest/profiles/storage_test.config")
fi
"$source_tree/scripts/kconfig/merge_config.sh" -m -r -O "$canonical" \
    "$canonical/.config" "${fragments[@]}"
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$entry" olddefconfig
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$entry" -j"$jobs" vmlinux modules
if [[ "${2:-}" == --modules-only ]]; then
    # Verify the actual module closure, not just the fragment's spelling.
    for module in fs/ext4/ext4.ko fs/jbd2/jbd2.ko fs/mbcache.ko lib/crc/crc16.ko; do
        test -s "$canonical/$module"
    done
    exit 0
fi
python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
    --source-tree "$source_tree" --canonical-build-dir "$canonical" \
    --provider-build-dir "$output/provider" --output-dir "$output/runtime" \
    --device-profile storage --with-gates --link --ld ld.lld-18 --jobs "$jobs"
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    INSTALL_HDR_PATH="$output/uapi" headers_install
"${MUSL_CC:-musl-gcc}" -static -std=gnu11 -O2 -Wall -Wextra -Werror \
    -isystem "$output/uapi/include" -o "$output/native-test" \
    "$source_tree/kobox/boot/fs_port_native.c" \
    "$source_tree/kobox/boot/fs_workload.c"
# Kbuild may run in a pinned SDK while the POSIX harness uses the system
# toolchain. Its compiler and linker must agree on libc and the ELF loader;
# a system cc followed by an SDK ld wrapper can produce a mixed-libc ELF.
cmake_command="$(command -v cmake)"
host_cc="$(command -v "${HOST_CC:-cc}")"
export PATH="$(dirname "$host_cc"):$PATH"
unset LIBRARY_PATH HOSTCFLAGS
"$cmake_command" -S "$source_tree/.." -B "$output/host" \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="$host_cc" \
    -DKOBOX_LINUX_TASK_BUILD_DIR="$canonical" \
    -DKOBOX_LINUX_BOOT_RUNTIME_CORE="$output/runtime/linux-boot-runtime.so"
"$cmake_command" --build "$output/host" --target kobox_linux_boot_test -j"$jobs"
