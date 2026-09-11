#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Audit the pre-boot common libraries, separately from the native backends."""

import argparse
from pathlib import Path
import re
import subprocess


def verify(nm, archives):
    # Portable byte/string operations and the independently shared protocol.
    # In particular, allocation, TLS, native errno and process/FD operations
    # are not implicit dependencies of these common compilation units.
    allowed = {"memcmp", "memcpy", "memset", "strcmp"}
    for archive in archives:
        output = subprocess.check_output(
            [nm, "--undefined-only", "--format=posix", str(archive)], text=True
        )
        unexpected = []
        for line in output.splitlines():
            fields = line.split()
            if len(fields) < 2 or fields[1] not in {"U", "w", "v"}:
                continue
            name = fields[0]
            if name not in allowed and not re.fullmatch(r"kb2_[a-z0-9_]+", name):
                unexpected.append(name)
        if unexpected:
            raise RuntimeError(f"{archive}: unexpected common imports: {unexpected}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--archive", type=Path, action="append", required=True)
    arguments = parser.parse_args()
    verify(arguments.nm, arguments.archive)
    print("Common bootstrap/package/codec/registry imports contain no native OS operations")


if __name__ == "__main__":
    main()
