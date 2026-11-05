#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production root selector with block-device lifetime faults.

Only Linux's device registry/open operations are mocked. This does not replace
the runtime build or an actual GPT/NVMe boot test.
"""
from pathlib import Path
import os
import subprocess
import sys

source = Path(__file__).resolve().parent
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
code = (source / "fs_mount.c").read_text()
start = code.index("static bool fs_disk_allowed(")
end = code.index("static int fs_make_node(", start)
(output / "fs_root_selector.inc").write_text(code[start:end])
subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                "-I", str(output), str(source / "fs_root_test.c"),
                "-o", str(output / "unit")], check=True)
subprocess.run([str(output / "unit")], check=True)
