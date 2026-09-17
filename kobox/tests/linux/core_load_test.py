#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fixed core inventory and initcall checks; execution belongs to host tests."""

import argparse
import importlib.util
from pathlib import Path
import struct
import sys

SPEC = importlib.util.spec_from_file_location(
    "core_inspection", Path(__file__).resolve().parents[2] / "boot/inspect_core.py")
inspection = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inspection)
require = inspection.require
boot = inspection.boot


def verify(arguments):
    offsets, segments, inputs = inspection.verify(arguments)
    data = arguments.core.read_bytes()
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    program_offset, entry_size, entry_count = header[5], header[9], header[10]
    file_segments = []
    for index in range(entry_count):
        program = struct.unpack_from("<IIQQQQQQ", data,
                                     program_offset + index * entry_size)
        kind, file_offset, address, file_size = program[0], program[2], program[3], program[5]
        if kind == 1 and file_size:
            file_segments.append((address, address + file_size, file_offset))

    def signed_word(address):
        for start, end, file_offset in file_segments:
            if start <= address and address + 4 <= end:
                return struct.unpack_from("<i", data, file_offset + address - start)[0]
        raise boot.BootBuildError(f"initcall slot is not file-backed: {address:#x}")

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
            target = slot + signed_word(slot)
            require(any(start <= target < stop and "E" in flags
                        for start, stop, flags in segments),
                    f"initcall target is not executable core code: {slot:#x}")
            initcalls += 1
    print(f"Fixed boot core inspected; {initcalls} initcall targets retained "
          "(execution is covered by the Linux host loader Gate)")


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
