#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Verify the OS-independent fixed-core ELF and initcall layout."""

import argparse
import importlib.util
import pathlib
import struct
import sys


SPEC = importlib.util.spec_from_file_location(
    "boot_build", pathlib.Path(__file__).with_name("build_boot_runtime.py")
)
boot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot)


def require(condition, detail):
    if not condition:
        raise boot.BootBuildError(detail)


def load_segments(headers):
    segments = []
    previous_end = 0
    for line in headers.splitlines():
        fields = line.split()
        if not fields or fields[0] != "LOAD":
            continue
        file_offset = int(fields[1], 16)
        address = int(fields[2], 16)
        file_size = int(fields[4], 16)
        memory_size = int(fields[5], 16)
        flags = "".join(fields[6:-1])
        require(address % 4096 == 0 and address >= previous_end,
                "PT_LOAD permissions overlap at page granularity")
        require("R" in flags and not ("W" in flags and "E" in flags),
                "invalid fixed-image PT_LOAD permissions")
        previous_end = (address + memory_size + 4095) & ~4095
        segments.append((file_offset, address, file_size, memory_size, flags))
    require(segments, "fixed image has no PT_LOAD mappings")
    return segments


def image_bytes(path, segments, address, size):
    for file_offset, start, file_size, _, _ in segments:
        if start <= address and address + size <= start + file_size:
            with path.open("rb") as stream:
                stream.seek(file_offset + address - start)
                data = stream.read(size)
            require(len(data) == size, "short fixed-image data read")
            return data
    raise boot.BootBuildError(f"symbol data is not file-backed: {address:#x}")


def verify(arguments):
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    (arguments.output_dir / ".metadata").mkdir(exist_ok=True)
    arguments.architecture_include = arguments.source_tree / "kobox/task/include"
    arguments.extra_include_dirs = [arguments.source_tree / "kobox/boot/include"]
    arguments.protocol_include = arguments.source_tree.parent / "protocol/generated/include"
    arguments.extra_cflags = ["-DKOBOX_BOOT_RUNTIME=1"]
    script = boot.compile_linker_script(arguments)
    source = boot.task.compile_support(arguments, "kobox/boot/layout_fixture.c")
    output = arguments.output_dir / "layout-fixture"
    boot.task.run([
        arguments.ld, "-static", "-Bsymbolic", "-z", "defs",
        "--build-id=none", "--entry=fixture_entry", "--script=" + str(script),
        "-o", output, source,
    ])

    file_header = boot.task.run([
        arguments.readelf, "--file-header", "--wide", output
    ])
    headers = boot.task.run([
        arguments.readelf, "--program-headers", "--wide", output
    ])
    require("EXEC (Executable file)" in file_header,
            "layout fixture is not a fixed ET_EXEC image")
    require(" DYNAMIC " not in headers and " TLS " not in headers,
            "fixed image retained dynamic-loader or compiler-TLS state")
    segments = load_segments(headers)
    require(segments[0][0] == 0 and segments[0][1] == boot.CORE_LINK_BASE,
            "ELF headers are outside the fixed RAM image")

    undefined = boot.task.run([
        arguments.nm, "--undefined-only", "--format=posix", output
    ])
    require(not undefined.strip(), "fixed image has an implicit host dependency")
    symbols = boot.task.run([
        arguments.nm, "--defined-only", "--format=posix", output
    ])
    addresses = {
        fields[0]: int(fields[2], 16)
        for line in symbols.splitlines()
        if len(fields := line.split()) >= 3
    }

    def address(name):
        require(name in addresses, f"missing fixed-image symbol: {name}")
        return addresses[name]

    def word(name):
        return struct.unpack(
            "<Q", image_bytes(output, segments, address(name), 8)
        )[0]

    page = word("fixture_page_size")
    require(page == 4096, "fixture page size differs from fixed-image ABI")
    image_start, image_end = address("_text"), address("__bss_stop")
    init_start, init_end = address("__init_begin"), address("__init_end")
    require(image_start == boot.CORE_LINK_BASE and image_start % page == 0,
            "ELF image base differs from the loader contract")
    require(image_start < init_start < init_end <= image_end,
            "invalid image/init range")
    require(init_start % page == init_end % page == 0,
            "init range is not page aligned")
    require(init_start <= address("_sinittext") < address("_einittext") < init_end,
            "missing real init text")
    require(init_start <= address("fixture_init_data") < init_end,
            "init data escaped its fixed-image range")
    require(address("__start_rodata") <= address("fixture_rodata") <
            address("__end_rodata"), "constant escaped rodata bounds")
    require(address("_sdata") <= address("fixture_data") < address("_edata"),
            "data escaped writable bounds")
    require(address("__start_ro_after_init") <=
            address("fixture_ro_after_init") < address("__end_ro_after_init"),
            "read-only-after-init data escaped bounds")
    for name in ("current_task", "cpu_current_top_of_stack"):
        require(address("__per_cpu_start") <= address(name) <
                address("__per_cpu_end"), f"per-CPU data escaped bounds: {name}")
    stack_start = address("__start_init_stack")
    stack_end = address("__end_init_stack")
    require(stack_end - stack_start == word("fixture_thread_size"),
            "wrong fixed-image init stack size")
    require(stack_end - address("__top_init_kernel_stack") ==
            word("fixture_stack_padding"),
            "init stack top differs from the Linux pt_regs layout")

    for begin, end, target in (
        ("__initcall_start", "__initcall0_start", "fixture_early"),
        ("__initcall6_start", "__initcall7_start", "fixture_device"),
    ):
        slot = address(begin)
        require(address(end) - slot == 4, f"initcall table changed: {begin}")
        displacement = struct.unpack(
            "<i", image_bytes(output, segments, slot, 4)
        )[0]
        require(slot + displacement == address(target),
                f"wrong relocated initcall target: {begin}")
        require(init_start <= address(target) < init_end,
                f"initcall target escaped init range: {target}")

    entry = address("fixture_entry")
    require(any(start <= entry < start + memory_size and "E" in flags
                for _, start, _, memory_size, flags in segments),
            "fixed-image entry is not executable")
    require(all(start + memory_size <= image_end
                for _, start, _, memory_size, _ in segments),
            "PT_LOAD escaped the fixed RAM image")
    print("Fixed core ELF layout/initcalls passed (OS-independent format Gate)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-tree", "provider-build-dir", "output-dir"):
        parser.add_argument("--" + name, type=pathlib.Path, required=True)
    for name, default in (("cc", "clang-18"), ("ld", "ld.lld"),
                          ("nm", "llvm-nm-18"), ("readelf", "llvm-readelf-18")):
        parser.add_argument("--" + name, default=default)
    arguments = parser.parse_args()
    for name in ("source_tree", "provider_build_dir", "output_dir"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    try:
        verify(arguments)
    except (boot.BootBuildError, boot.task.TaskBuildError,
            OSError, ValueError, KeyError, struct.error) as error:
        print(f"Fixed core ELF layout test: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
