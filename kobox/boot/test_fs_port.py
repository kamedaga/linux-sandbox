#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare the hosted VFS port to native Linux on disposable ext4/tmpfs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import tempfile
import time

RESULT = re.compile(r"^FS_RESULT fs=(ext4|tmpfs) checks=(\d+) digest=(\d+) "
                    r"line=(\d+) actual=(-?\d+) expected=(-?\d+)\r?$", re.M)


def run(command, log=None):
    result = subprocess.run([str(arg) for arg in command], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=120)
    if log:
        log.write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {command}\n"
                           + result.stdout.decode(errors="replace"))
    return result.stdout.decode(errors="replace")


def image(path):
    # Never overwrite an existing result or copy a rootfs. This is a fresh
    # 32 MiB filesystem with no relation to any production storage device.
    with path.open("xb") as output:
        output.truncate(32 << 20)
    run(["mkfs.ext4", "-q", "-F", "-b", "4096", "-E",
         "lazy_itable_init=0,lazy_journal_init=0", path])


def parse(log):
    rows = {}
    for fs, checks, digest, line, actual, expected in RESULT.findall(log):
        if fs in rows or int(line) or int(actual) != int(expected):
            raise RuntimeError(f"failed or duplicate workload: {fs}")
        rows[fs] = {"checks": int(checks), "digest": int(digest)}
    if set(rows) != {"ext4", "tmpfs"} or any(r["checks"] < 100 for r in rows.values()):
        raise RuntimeError("missing workload results")
    return rows


def native(args, disk, fixtures):
    command = [args.qemu, "-machine", "q35", "-enable-kvm", "-cpu", "host",
               "-m", "512M", "-smp", "2", "-display", "none", "-monitor", "none",
               "-serial", "stdio", "-no-reboot", "-net", "none",
               "-cdrom", str(args.iso), "-boot", "order=d",
               "-drive", f"file=fat:ro:{fixtures},format=raw,if=virtio,readonly=on",
               "-drive", f"file={disk},format=raw,if=virtio,cache=none,aio=native"]
    (args.out / "native-command.json").write_text(json.dumps(command, indent=2) + "\n")
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()
    log = (args.out / "native.log").open("xb")

    def wait(marker, timeout=120):
        deadline = time.monotonic() + timeout
        while marker not in output:
            if b"\nFS_NATIVE_FAILED\r\n" in output:
                raise RuntimeError("native workload failed; see native.log")
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError(f"native guest failed waiting for {marker!r}")
            for key, _ in selector.select(1):
                data = os.read(key.fd, 65536)
                if not data:
                    raise RuntimeError("native console closed")
                output.extend(data)
                log.write(data)
                log.flush()

    def send(command):
        process.stdin.write((command + "\n").encode())
        process.stdin.flush()

    try:
        wait(b"localhost login:")
        send("root")
        wait(b"localhost:~#")
        send("(mkdir -p /bench /ext4 /tmpfs && mount -o ro /dev/vda1 /bench && "
             "mount -t ext4 /dev/vdb /ext4 && mount -t tmpfs tmpfs /tmpfs && "
             "uname -a && cat /etc/alpine-release && "
             "/bench/native-test ext4 /ext4 && /bench/native-test tmpfs /tmpfs && "
             "sync && umount /tmpfs && umount /ext4 && echo FS_NATIVE_OK) "
             "|| echo FS_NATIVE_FAILED")
        wait(b"\nFS_NATIVE_OK\r\n")
        send("poweroff")
        process.wait(timeout=30)
        return output.decode(errors="replace")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        selector.close()
        log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=Path)
    parser.add_argument("--host-test", required=True, type=Path)
    parser.add_argument("--native-test", required=True, type=Path)
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--new-run", action="store_true",
                        help="Keep a fresh run directory below --out (CTest)")
    args = parser.parse_args()
    for name in ("core", "host_test", "native_test", "iso", "out"):
        setattr(args, name, getattr(args, name).resolve())
    if args.new_run:
        args.out.mkdir(parents=True, exist_ok=True)
        args.out = Path(tempfile.mkdtemp(prefix="run-", dir=args.out))
    else:
        args.out.mkdir(parents=True, exist_ok=False)
    print(f"FS test artifacts: {args.out}", flush=True)
    fixtures = args.out / "fixtures"
    fixtures.mkdir()
    # Freeze executable inputs before launch; a concurrent rebuild must not
    # change mapped core pages or the hash associated with an executed test.
    shutil.copy2(args.core, args.out / "core.so")
    inputs = args.core.parent / "linux-boot-inputs.json"
    if inputs.is_file():
        shutil.copy2(inputs, args.out / inputs.name)
    args.core = args.out / "core.so"
    shutil.copy2(args.native_test, fixtures / "native-test")
    args.native_test = fixtures / "native-test"
    sources = args.out / "sources"
    sources.mkdir()
    script = Path(__file__).resolve().parent
    for name in ("fs_port.c", "fs_port.h", "fs_port_internal.h", "fs_service.c", "fs_service.h",
                 "fs_worker.c", "fs_worker.h", "fs_worker_gate.c", "fs_worker_gate.h",
                 "fs_bench.c", "fs_bench.h",
                 "fs_exec.c", "fs_exec.h",
                 "fs_mount.c", "fs_mount.h", "fs_port_gate.c", "fs_port_gate.h",
                 "fs_workload.c", "fs_workload.h", "fs_port_native.c", "boot_test.c",
                 "sources.py", "build_fs_port_test.sh", "test_fs_port.py"):
        shutil.copy2(script / name, sources / name)
    protocol = script.parents[2] / "protocol"
    for relative in ("schema/filesystem.json", "include/kobox2/filesystem.h",
                     "src/filesystem.c", "generated/include/kobox2/filesystem_layout.h"):
        destination = sources / "protocol" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(protocol / relative, destination)
    for name in ("storage.config", "storage_test.config"):
        shutil.copy2(script.parent / "manifest/profiles" / name, sources / name)
    for relative in ("mm/nofault.c", "memory/early_boot.c", "memory/port.h"):
        destination = sources / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(script.parent / relative, destination)
    hosted_disk = args.out / "hosted.img"
    native_disk = args.out / "native.img"
    image(hosted_disk)
    image(native_disk)
    hosted = run([args.host_test, args.core, "--fs-port", hosted_disk],
                 args.out / "hosted.log")
    if not re.search(r"FS_PORT status=0 stage=4 handles=20000 "
                     r"shared_reads=512 cpu_mask=3 close_races=1024 warnings=0", hosted):
        raise RuntimeError("hosted port gate did not complete")
    reference = native(args, native_disk, fixtures)
    results = {"hosted": parse(hosted), "native": parse(reference)}
    if results["hosted"] != results["native"]:
        raise RuntimeError(f"VFS results differ: {results}")
    for name, disk in (("hosted", hosted_disk), ("native", native_disk)):
        run(["e2fsck", "-f", "-n", disk], args.out / f"{name}-e2fsck.log")
        content = run(["debugfs", "-R", "cat /suite/b", disk])
        if not content.rstrip().endswith("B"):
            raise RuntimeError(f"missing persisted rename/write: {name}")
        content = run(["debugfs", "-R", "cat /suite/user", disk])
        if not content.rstrip().endswith("owner"):
            raise RuntimeError(f"missing persisted owner data: {name}")
    results["sha256"] = {
        name: hashlib.sha256(path.read_bytes()).hexdigest()
        for name, path in (("core", args.core), ("native_test", args.native_test),
                           ("iso", args.iso), ("hosted_img", hosted_disk),
                           ("native_img", native_disk))
    }
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
