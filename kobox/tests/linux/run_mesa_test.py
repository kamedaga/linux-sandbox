#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run real Mesa through hosted Linux; stage only explicit ELF dependencies."""

import argparse
from contextlib import nullcontext
import json
import os
import re
import subprocess

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from display import DisplayOracle
from gates.mesa import MesaEvidence, MesaOptions
from qemu_trace import decode


def dynamic(path):
    return subprocess.check_output(["readelf", "-d", str(path)], text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("test", "core", "qemu", "modules", "bootstrap", "client",
                 "mesa-build", "sysroot"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--host-driver")
    parser.add_argument("--kms", action="store_true")
    parser.add_argument("--multi", action="store_true")
    parser.add_argument("--recovery", action="store_true")
    parser.add_argument("--expect-shared-mismatch", action="store_true")
    parser.add_argument("--display-observer", type=Path)
    parser.add_argument("--expect-display-mismatch", action="store_true")
    parser.add_argument("--draw", action="store_true",
                        help="Require shader/texture/PBO/fence and repeated pixel evidence")
    parser.add_argument("--expect-pixel-mismatch", action="store_true",
                        help="Negative control: a deliberately wrong shader must fail pixel checking")
    args = parser.parse_args()
    if args.recovery and not args.multi:
        parser.error("--recovery requires --multi")
    if args.multi and (not args.kms or args.draw or args.expect_display_mismatch):
        parser.error("--multi requires --kms and its own shared-pixel control")
    if args.expect_shared_mismatch and (not args.multi or args.recovery):
        parser.error("--expect-shared-mismatch requires --multi without --recovery")
    if args.kms and (not args.display_observer or args.draw):
        parser.error("--kms requires --display-observer and is separate from --draw")
    if args.expect_display_mismatch and not args.kms:
        parser.error("--expect-display-mismatch requires --kms")
    if args.expect_pixel_mismatch and not args.draw:
        parser.error("the pixel oracle control requires --draw")
    mesa = args.mesa_build.resolve()
    options = json.loads((mesa / "meson-info/intro-buildoptions.json").read_text())
    drivers = next(option["value"] for option in options
                   if option["name"] == "gallium-drivers")
    if drivers != ["virgl"]:
        parser.error("client Mesa must contain only the virgl driver")
    roots = [args.sysroot / "usr/lib/x86_64-linux-gnu",
             Path("/usr/lib/x86_64-linux-gnu"), Path("/lib/x86_64-linux-gnu")]
    files = {}
    pending = [args.client.resolve()]

    def add(target, source):
        source = source.resolve(strict=True)
        if target in files:
            if files[target] != source:
                raise ValueError(f"conflicting dependency {target}")
            return
        files[target] = source
        pending.append(source)

    libraries = [mesa / "src/egl/libEGL.so.1.0.0",
                 mesa / "src/gbm/libgbm.so.1.0.0"]
    gallium = list((mesa / "src/gallium/targets/dri").glob("libgallium-*.so"))
    if len(gallium) != 1:
        parser.error("expected one built libgallium")
    for library in libraries + gallium:
        soname = re.search(r"\(SONAME\).*\[(.*?)\]", dynamic(library))
        if not soname:
            raise ValueError(f"missing SONAME: {library}")
        add("/lib/x86_64-linux-gnu/" + soname[1], library)
    add("/usr/lib/x86_64-linux-gnu/gbm/dri_gbm.so",
        mesa / "src/gbm/backends/dri/dri_gbm.so")
    program = subprocess.check_output(["readelf", "-l", str(args.client)], text=True)
    interpreter = re.search(r"Requesting program interpreter: (.*?)\]", program)
    if not interpreter:
        parser.error("client must use a real dynamic loader (PT_INTERP)")
    add(interpreter[1], Path(interpreter[1]))
    visited = set()
    while pending:
        source = pending.pop()
        if source in visited:
            continue
        visited.add(source)
        for name in re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic(source)):
            if "/" in name:
                raise ValueError(f"non-soname dependency: {name}")
            target = "/lib/x86_64-linux-gnu/" + name
            if target in files:
                continue
            match = next((root / name for root in roots if (root / name).is_file()), None)
            if match is None:
                raise FileNotFoundError(name)
            add(target, match)
    command = [str(path.resolve()) for path in
               (args.test, args.core, args.qemu, args.modules, args.bootstrap, args.client)]
    for target, source in sorted(files.items()):
        print(f"client file: {target} <- {source}", flush=True)
        command += ["--client-file", target, str(source)]
    command.append("--virgl-revoke" if args.recovery else
                   "--virgl-shared" if args.multi else "--virgl")
    environment = os.environ.copy()
    if args.host_driver:
        environment["GALLIUM_DRIVER"] = args.host_driver
    # This environment belongs to QEMU only; client exec has a separate envp.
    environment["SDL_VIDEODRIVER"] = "x11"
    observer = None
    if args.kms:
        observer = DisplayOracle(args.display_observer, args.test.resolve().parent,
                                 environment, args.qemu)
        command[2] = str(Path(__file__).with_name("qemu_display_test.py").resolve())
    evidence = MesaEvidence(MesaOptions(**{
        name: getattr(args, name) for name in MesaOptions.__dataclass_fields__}))
    with (observer if observer else nullcontext()), subprocess.Popen(
            command, env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True) as process:
        for line in process.stdout:
            print(line, end="", flush=True)
            evidence.observe(line, decode(line))
        result = process.wait()
    return evidence.verify(result, observer.evidence() if observer else None)


if __name__ == "__main__":
    raise SystemExit(main())
