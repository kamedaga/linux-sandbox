#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Linux-native core load and relocated initcall checks; never run during build."""

import argparse
import ctypes
import errno
import importlib.util
from pathlib import Path
import sys

SPEC = importlib.util.spec_from_file_location(
    "core_inspection", Path(__file__).resolve().parents[2] / "boot/inspect_core.py")
inspection = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inspection)
require = inspection.require
boot = inspection.boot


def verify(arguments):
    offsets, segments, inputs = inspection.verify(arguments)
    library = ctypes.CDLL(str(arguments.core))
    library.kobox_linux_boot_start.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    library.kobox_linux_boot_start.restype = ctypes.c_int
    require(library.kobox_linux_boot_start(None, None) == -errno.EINVAL,
            "loaded boot entry does not reject an invalid machine binding")
    base = ctypes.cast(library.kobox_linux_boot_start, ctypes.c_void_p).value - offsets[
        "kobox_linux_boot_start"]
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
    parser.add_argument("--core", required=True, type=Path)
    parser.add_argument("--inputs", required=True, type=Path)
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
