#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Reject dependencies outside the intentionally small POSIX host surface."""

import argparse
import pathlib
import subprocess
import sys


ALLOWED_SYMBOLS = {
    # ELF relocation anchor emitted by GCC for this native backend, not an OS call.
    "_GLOBAL_OFFSET_TABLE_",
    # Native compiler stack protection; never allowed in common machine state.
    "__stack_chk_fail",
    "__errno_location",
    "__libc_current_sigrtmax",
    "__libc_current_sigrtmin",
    "clock_gettime",
    "close",
    "calloc",
    "free",
    # Linux backing seals and synchronous cross-process alias invalidation.
    "fcntl",
    "fstat",
    "ftruncate",
    "getpid",
    "memset",
    "memfd_create",
    "mmap",
    "mprotect",
    "munmap",
    "sigaction",
    "sigaddset",
    "sigdelset",
    "sigemptyset",
    # Terminal CPU halt masks all catchable signals and parks without locks.
    "sigfillset",
    "sigismember",
    "sigsuspend",
    "snprintf",
    "sysconf",
}
ALLOWED_PREFIXES = ("kobox_posix_", "kobox_machine_domain_", "pthread_")
FORBIDDEN_SUBSTRINGS = ("eventfd", "futex", "timerfd")


class SurfaceError(Exception):
    """The archive imports an operation outside the host boundary."""


def parse_undefined(text):
    symbols = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in ("U", "w", "v"):
            symbols.add(fields[0].split("@", 1)[0])
    return symbols


def validate_symbols(symbols):
    for symbol in sorted(symbols):
        if any(fragment in symbol for fragment in FORBIDDEN_SUBSTRINGS):
            raise SurfaceError(f"forbidden Linux primitive import: {symbol}")
        if symbol in ALLOWED_SYMBOLS or symbol.startswith(ALLOWED_PREFIXES):
            continue
        raise SurfaceError(f"undeclared host import: {symbol}")


def validate_machine_symbols(symbols):
    # Domain bookkeeping is self-contained C11: not even a hidden lock/TLS
    # runtime may be imported by its atomics on the fixed machine profile.
    if symbols:
        raise SurfaceError(f"common machine import: {', '.join(sorted(symbols))}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=pathlib.Path, required=True)
    parser.add_argument("--machine-archive", type=pathlib.Path, required=True)
    parser.add_argument("--nm", default="nm")
    arguments = parser.parse_args()
    try:
        result = subprocess.run(
            [
                arguments.nm, "--undefined-only", "--format=posix",
                str(arguments.archive),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode:
            raise SurfaceError(result.stderr.strip() or "nm failed")
        validate_symbols(parse_undefined(result.stdout))
        result = subprocess.run(
            [arguments.nm, "--undefined-only", "--format=posix",
             str(arguments.machine_archive)],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode:
            raise SurfaceError(result.stderr.strip() or "machine nm failed")
        validate_machine_symbols(parse_undefined(result.stdout))
    except (OSError, SurfaceError) as error:
        print(f"POSIX host surface: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
