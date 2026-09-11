#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Inspect the full target core ELF without loading or executing it."""

import argparse
import importlib.util
import json
import pathlib
import sys


SPEC = importlib.util.spec_from_file_location(
    "boot_build", pathlib.Path(__file__).with_name("build_boot_runtime.py")
)
boot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot)


def require(condition, detail):
    if not condition:
        raise boot.BootBuildError(detail)


def load_segments(headers, page_size):
    segments = []
    previous_end = 0
    for line in headers.splitlines():
        fields = line.split()
        if not fields or fields[0] != "LOAD":
            continue
        start, size = int(fields[2], 16), int(fields[5], 16)
        if not size:
            continue
        flags = "".join(fields[6:-1])
        require(start % page_size == 0 and start >= previous_end,
                "PT_LOAD permissions overlap at page granularity")
        require(not ("W" in flags and "E" in flags), "writable executable PT_LOAD")
        require("R" in flags, "unreadable PT_LOAD")
        previous_end = (start + size + page_size - 1) & ~(page_size - 1)
        segments.append((start, start + size, flags))
    require(segments and segments[0][0] == 0, "ELF headers are outside the core image")
    return segments


def verify(arguments):
    headers = boot.task.run([arguments.readelf, "--program-headers", "--wide", arguments.core])
    segments = load_segments(headers, 4096)
    dynamic = boot.task.run([arguments.readelf, "--dynamic", "--wide", arguments.core])
    require("TEXTREL" not in dynamic, "core requires dynamic text relocations")
    require("NEEDED" not in dynamic, "core directly depends on a host library")
    versions = boot.task.run([arguments.readelf, "--version-info", arguments.core])
    require("GLIBC_" not in versions, "core retains a glibc ABI dependency")
    undefined = boot.task.run([arguments.nm, "--undefined-only", "--format=posix", arguments.core])
    require(not undefined.strip(), "core has an implicit host symbol dependency")
    symbols = boot.task.run([arguments.nm, "--defined-only", "--format=posix", arguments.core])
    offsets = {fields[0]: int(fields[2], 16) for line in symbols.splitlines()
               if len(fields := line.split()) >= 3}
    for symbol in ("start_kernel", "schedule", "try_to_wake_up", "mm_core_init",
                   "kthreadd", "kobox_linux_boot_start", "kobox_linux_task_dispatch",
                   "shmem_mapping", "shmem_get_folio", "shmem_file_setup",
                   "shmem_file_setup_with_mnt",
                   "vfs_truncate", "vfs_fallocate", "notify_change",
                   "filemap_remove_folio", "vm_memory_committed",
                   "file_open_root", "vfs_unlink", "kernel_read", "kernel_write",
                   "fput", "iput", "flush_delayed_fput", "task_work_run"):
        require(symbol in offsets, f"missing boot implementation: {symbol}")

    for symbol in ("mm_alloc", "mmput", "kthread_use_mm", "kthread_unuse_mm",
                   "vm_mmap", "vm_munmap", "__x64_sys_mprotect", "do_user_addr_fault",
                   "handle_mm_fault", "unmap_mapping_range", "kobox_vm_resolve_fault"):
        require(symbol in offsets, f"missing MM integration implementation: {symbol}")
    require(all(end <= offsets["__bss_stop"] for _, end, _ in segments),
            "PT_LOAD escaped the RAM image reservation")
    require(offsets["__apicdrivers_end"] > offsets["__apicdrivers"],
            "native APIC table was replaced with empty boundaries")
    require(offsets["__brk_limit"] - offsets["__brk_base"] >= 65536,
            "native early scratch reservation was lost")
    inputs = json.loads(arguments.inputs.read_text())
    require(type(inputs.get("with_gates")) is bool,
            "core inventory must declare production or Gate inputs")
    if inputs["with_gates"]:
        for symbol in ("kobox_linux_boot_memory_verify", "kobox_linux_vfs_verify",
                       "kobox_linux_shmem_verify", "kobox_linux_tls_probe"):
            require(symbol in offsets, f"missing test implementation: {symbol}")
    else:
        for symbol in ("kobox_linux_boot_memory_verify", "kobox_linux_vfs_verify",
                       "kobox_linux_shmem_verify", "kobox_linux_tls_probe"):
            require(symbol not in offsets, f"test implementation in production core: {symbol}")
    require(inputs.get("required_memory_config") == list(boot.REQUIRED_MEMORY_CONFIG),
            "core inventory lacks the real shmem/boot memory configuration")
    return offsets, segments, inputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=pathlib.Path)
    parser.add_argument("--inputs", required=True, type=pathlib.Path)
    parser.add_argument("--nm", default="llvm-nm-18")
    parser.add_argument("--readelf", default="llvm-readelf-18")
    arguments = parser.parse_args()
    try:
        verify(arguments)
        print("Boot core ELF inspected; native load and boot not executed")
    except (boot.BootBuildError, boot.task.TaskBuildError, OSError, ValueError, KeyError) as error:
        print(f"Boot core load: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
