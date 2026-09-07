#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Load the full core and check its ELF contract, not Linux service progress."""

import argparse
import ctypes
import errno
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
    undefined = boot.task.run([arguments.nm, "--undefined-only", "--format=posix", arguments.core])
    require({line.split()[0].split("@")[0] for line in undefined.splitlines()} <=
            {"__tls_get_addr"}, "core imports a non-TLS implementation from the host")
    symbols = boot.task.run([arguments.nm, "--defined-only", "--format=posix", arguments.core])
    offsets = {fields[0]: int(fields[2], 16) for line in symbols.splitlines()
               if len(fields := line.split()) >= 3}
    for symbol in ("start_kernel", "schedule", "try_to_wake_up", "mm_core_init",
                   "kthreadd", "kobox_linux_boot_start", "kobox_linux_task_dispatch",
                   "shmem_mapping", "shmem_get_folio", "shmem_file_setup",
                   "kobox_linux_boot_memory_verify", "kobox_linux_vfs_verify",
                   "kobox_linux_shmem_verify", "shmem_file_setup_with_mnt",
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
    library = ctypes.CDLL(str(arguments.core))
    library.kobox_linux_boot_start.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    library.kobox_linux_boot_start.restype = ctypes.c_int
    require(library.kobox_linux_boot_start(None, None) == -errno.EINVAL,
            "loaded boot entry does not reject an invalid machine binding")
    base = ctypes.cast(library.kobox_linux_boot_start, ctypes.c_void_p).value - offsets[
        "kobox_linux_boot_start"]
    inputs = json.loads(arguments.inputs.read_text())
    require(inputs.get("required_memory_config") == list(boot.REQUIRED_MEMORY_CONFIG),
            "core inventory lacks the real shmem/boot memory configuration")
    initcalls = 0
    for begin, end, prefix in (
        ("__initcall_start", "__initcall_end", ".initcall"),
        ("__con_initcall_start", "__con_initcall_end", ".con_initcall.init"),
    ):
        expected = sum(size for record in inputs["objects"]
                       for name, size in record["hosted_boot_sections"].items()
                       if name.startswith(prefix))
        require(offsets[end] - offsets[begin] == expected and expected % 4 == 0,
                f"linked initcall table differs from all canonical inputs: {begin}")
        for slot in range(offsets[begin], offsets[end], 4):
            target = slot + ctypes.c_int32.from_address(base + slot).value
            require(any(start <= target < stop and "E" in flags
                        for start, stop, flags in segments),
                    f"initcall target is not executable core code: {slot:#x}")
            initcalls += 1
    print(f"Full boot core loaded; {initcalls} initcall targets retained "
          "(not a Linux boot/service Gate)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=pathlib.Path)
    parser.add_argument("--inputs", required=True, type=pathlib.Path)
    parser.add_argument("--nm", default="llvm-nm-18")
    parser.add_argument("--readelf", default="llvm-readelf-18")
    arguments = parser.parse_args()
    try:
        verify(arguments)
    except (boot.BootBuildError, boot.task.TaskBuildError, OSError, ValueError, KeyError) as error:
        print(f"Boot core load: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
