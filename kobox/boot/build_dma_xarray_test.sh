#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

if [[ $# != 2 ]]; then
    echo "usage: $0 VERIFIED_CANONICAL NEW_OUTPUT_DIRECTORY" >&2
    exit 2
fi
source_tree="$(cd "$(dirname "$0")/../.." && pwd)"
baseline="$(realpath "$1")"
output="$(realpath -m "$2")"
if [[ -e "$output" ]]; then
    echo "output already exists: $output" >&2
    exit 2
fi
mkdir -p "$output/canonical"
canonical="$output/canonical"
jobs="${JOBS:-8}"
entry="$(python3 "$source_tree/kobox/boot/prepare_kconfig.py" \
    --source-tree "$source_tree" --build-dir "$canonical")"
# This is a test-only configuration. Start from the verified DMA/FS core
# and add upstream fault-injection controls without changing production inputs.
"$source_tree/scripts/kconfig/merge_config.sh" -m -r -O "$canonical" \
    "$baseline/.config" "$source_tree/kobox/boot/pressure.config"
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$entry" olddefconfig
diff -u "$baseline/.config" "$canonical/.config" > "$output/config.diff" || \
    [[ $? == 1 ]]
for option in FAILSLAB FAIL_PAGE_ALLOC FAULT_INJECTION_DEBUG_FS; do
    rg -q "^CONFIG_${option}=y$" "$canonical/.config"
done
make -C "$source_tree" O="$canonical" ARCH=x86 LLVM=-18 \
    KBUILD_KCONFIG="$entry" -j"$jobs" vmlinux
python3 "$source_tree/kobox/boot/build_boot_runtime.py" \
    --source-tree "$source_tree" --canonical-build-dir "$canonical" \
    --provider-build-dir "$output/provider" --output-dir "$output/runtime" \
    --device-profile storage --with-gates --link --ld ld.lld-18 --jobs "$jobs"
