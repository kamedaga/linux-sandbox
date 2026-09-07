#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Verify the hosted ELF layout and native TLS, not Linux boot/service progress."""

import argparse
import ctypes
import importlib.util
import pathlib
import re
import sys
import threading


SPEC = importlib.util.spec_from_file_location(
    "boot_build", pathlib.Path(__file__).with_name("build_boot_runtime.py")
)
boot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot)


def require(condition, detail):
    if not condition:
        raise boot.BootBuildError(detail)


def verify(arguments):
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    (arguments.output_dir / ".metadata").mkdir(exist_ok=True)
    arguments.architecture_include = arguments.source_tree / "kobox/task/include"
    arguments.extra_include_dirs = [arguments.source_tree / "kobox/boot/include"]
    arguments.protocol_include = arguments.source_tree.parent / "protocol/generated/include"
    arguments.extra_cflags = ["-DKOBOX_BOOT_RUNTIME=1"]
    script = boot.compile_linker_script(arguments)
    source = boot.task.compile_support(arguments, "kobox/boot/layout_fixture.c")
    output = arguments.output_dir / "layout-fixture.so"
    boot.task.run([
        arguments.cc, "-shared", "-nostartfiles", "--ld-path=" + arguments.ld,
        "-Wl,-Bsymbolic,-z,defs,-z,now,--build-id=none",
        "-Wl,-e,fixture_advance", "-Wl,-T," + str(script), source, "-o", output,
    ])
    library = ctypes.CDLL(str(output))
    symbols = boot.task.run([arguments.nm, "--defined-only", "--format=posix", output])
    offsets = {fields[0]: int(fields[2], 16) for line in symbols.splitlines()
               if len(fields := line.split()) >= 3}
    load_bias = ctypes.cast(library.fixture_advance, ctypes.c_void_p).value - offsets["fixture_advance"]

    def address(name):
        # glibc's dlsym rejects a zero-valued non-TLS symbol even when it
        # denotes the image base. Resolve linker bounds from the ELF table.
        return load_bias + offsets[name]

    def word(name):
        return ctypes.c_ulong.in_dll(library, name).value

    page = word("fixture_page_size")
    image_start, image_end = address("_text"), address("__bss_stop")
    init_start, init_end = address("__init_begin"), address("__init_end")
    require(image_start % page == 0, "ELF image base is not page aligned")
    require(image_start < init_start < init_end <= image_end, "invalid image/init range")
    require(init_start % page == init_end % page == 0, "init range is not page aligned")
    require(init_start <= address("_sinittext") < address("_einittext") < init_end,
            "missing real init text")
    require(init_start <= address("fixture_init_data") < init_end, "init data escaped")
    require(address("__start_rodata") <= address("fixture_rodata") < address("__end_rodata"),
            "large-model constants escaped rodata bounds")
    require(address("_sdata") <= address("fixture_data") < address("_edata"),
            "large-model data escaped data bounds")
    require(address("__start_ro_after_init") <= address("fixture_ro_after_init") <
            address("__end_ro_after_init"), "read-only-after-init data escaped bounds")
    for name in ("current_task", "cpu_current_top_of_stack"):
        require(address("__per_cpu_start") <= address(name) < address("__per_cpu_end"),
                f"per-CPU initializer escaped bounds: {name}")
    stack_start, stack_end = address("__start_init_stack"), address("__end_init_stack")
    require(stack_end - stack_start == word("fixture_thread_size"), "wrong init stack size")
    require(stack_end - address("__top_init_kernel_stack") == word("fixture_stack_padding"),
            "init stack top does not match the pinned Linux pt_regs layout")

    # These are synthetic callbacks emitted by real initcall macros. Calling
    # them verifies relocation/layout only; no Linux service is initialized.
    for begin, end, expected in (
        ("__initcall_start", "__initcall0_start", 11),
        ("__initcall6_start", "__initcall7_start", 22),
    ):
        slot = address(begin)
        require(address(end) - slot == 4, f"initcall table changed: {begin}")
        target = slot + ctypes.c_int32.from_address(slot).value
        require(init_start <= target < init_end, "initcall target escaped init range")
        require(ctypes.CFUNCTYPE(ctypes.c_int)(target)() == expected,
                f"wrong relocated initcall: {begin}")

    library.fixture_advance.restype = ctypes.c_uint
    require(library.fixture_advance() == 8, "initial native TLS value changed")
    results = []
    threads = [threading.Thread(target=lambda: results.append(library.fixture_advance()))
               for _ in range(2)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    require(results == [8, 8] and library.fixture_advance() == 9,
            "ELF TLS is not independent across native threads")

    headers = boot.task.run([arguments.readelf, "--program-headers", "--wide", output])
    first_load = True
    for line in headers.splitlines():
        fields = line.split()
        if fields and fields[0] == "LOAD":
            start, size = int(fields[2], 16), int(fields[5], 16)
            if first_load:
                require(start == offsets["_text"] and int(fields[1], 16) == 0,
                        "ELF headers are not included in the RAM image mapping")
                first_load = False
            require(offsets["_text"] <= start and start + size <= offsets["__bss_stop"],
                    "a PT_LOAD mapping escaped the RAM image bounds")
    dynamic = boot.task.run([arguments.readelf, "--dynamic", "--wide", output])
    require(not re.search(r"\bTEXTREL\b", dynamic), "layout requires text relocations")
    print("Hosted ELF layout/initcalls/TLS passed (not a Linux boot gate)")


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
    except (boot.BootBuildError, boot.task.TaskBuildError, OSError, ValueError) as error:
        print(f"Hosted ELF layout test: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
