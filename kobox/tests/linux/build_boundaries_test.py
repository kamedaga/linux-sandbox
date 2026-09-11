#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise target selection and compile the foundation without test tooling."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def run(command, expected=None):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if expected:
        if result.returncode == 0 or expected not in result.stdout:
            raise RuntimeError(result.stdout)
    elif result.returncode:
        raise RuntimeError(result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="build-boundaries-", dir=args.artifacts) as directory:
        root = Path(directory)
        base = [args.cmake, "-S", str(args.source),
                "-DBUILD_TESTING=OFF", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                "-DCMAKE_C_COMPILER=" + args.cc]
        for index, (options, expected) in enumerate((
                (["-DKB2_OS_BACKEND=freebsd", "-DKB2_CPU_ARCH=x86_64"],
                 "Unsupported OS backend: freebsd"),
                (["-DKB2_OS_BACKEND=linux", "-DKB2_CPU_ARCH=aarch64"],
                 "Unsupported CPU architecture: aarch64"),
                (["-DKB2_OS_BACKEND=../linux", "-DKB2_CPU_ARCH=x86_64"],
                 "Invalid KB2_OS_BACKEND"),
                (["-DKB2_CORE_WITH_GATES=ON"],
                 "KB2_CORE_WITH_GATES requires BUILD_TESTING"))):
            run([*base, "-B", str(root / str(index)), *options], expected)
        build = root / "foundation"
        run([*base, "-B", str(build),
             "-DKB2_OS_BACKEND=linux", "-DKB2_CPU_ARCH=x86_64"])
        run([args.cmake, "--build", str(build), "--parallel", "2"])
        commands = json.loads((build / "compile_commands.json").read_text())
        sources = [Path(command["file"]).relative_to(args.source).as_posix()
                   for command in commands]
        for source in sources:
            if ("/tests/" in "/" + source or "/test/" in "/" + source or
                    "qemu" in source or "qtest" in source or
                    "_test." in source or "_fixture." in source):
                raise RuntimeError("test source in foundation: " + source)
        for required in ("host/posix/bootstrap.c", "host/posix/device_proxy.c",
                         "host/posix/device_channel.c", "mm/posix.c",
                         "task/posix_machine.c", "machine/domain.c",
                         "arch/x86_64/elf.c", "host/posix/vm_bootstrap.c"):
            if "linux-sandbox/kobox/" + required not in sources:
                raise RuntimeError("missing foundation source: " + required)
        cache = (build / "CMakeCache.txt").read_text()
        for forbidden in ("KOBOX_QEMU_", "KOBOX_MESA_", "KOBOX_VIRGL_"):
            if forbidden in cache:
                raise RuntimeError("test tooling configured without tests: " + forbidden)
        listing = run(["ctest", "--test-dir", str(build), "--show-only=json-v1"])
        if json.loads(listing)["tests"]:
            raise RuntimeError("test registered with BUILD_TESTING=OFF")
        checker = args.source / "linux-sandbox/kobox/cmake/boundaries.cmake"
        for name, definition, expected in (
                ("source", "add_library(production STATIC test/source.c)",
                 "Test source in production target"),
                ("dependency", "add_library(production STATIC source.c)\n"
                 "add_library(fixture STATIC source.c)\n"
                 "target_link_libraries(production PRIVATE fixture)",
                 "Non-production dependency of production")):
            source = root / ("negative-" + name)
            (source / "test").mkdir(parents=True)
            (source / "source.c").write_text("int boundary_probe;\n")
            (source / "test/source.c").write_text("int boundary_probe;\n")
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\n"
                "project(boundary_probe LANGUAGES C)\n" + definition + "\n"
                + f'include("{checker}")\n'
                + "kobox_check_production_targets(production)\n")
            run([args.cmake, "-S", str(source), "-B", str(source / "build"),
                 "-DCMAKE_C_COMPILER=" + args.cc], expected)
    print("Independent OS/architecture selection and test-free foundation build passed")


if __name__ == "__main__":
    main()
