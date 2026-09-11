#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verifier unit tests only; these never replace the real KMS display Gate."""

from contextlib import redirect_stdout
from io import StringIO
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from display import DisplayOracle
from gates.display import ScanoutEvidence
from qemu_trace import decode


def native_trace():
    lines = []
    for mode in range(4):
        width, height = (800, 600) if mode % 2 else (640, 480)
        resources = (mode * 2 + 2, mode * 2 + 3)
        for resource in resources:
            lines.append(f"virtio_gpu_cmd_res_create_3d res {resource:#x}, fmt 0x2, "
                         f"w {width}, h {height}, d 1\n")
        for frame in range(16):
            resource = resources[frame % 2]
            lines.append(f"virtio_gpu_cmd_set_scanout id 0, res {resource:#x}, "
                         f"w {width}, h {height}, x 0, y 0\n")
            lines.append(f"virtio_gpu_cmd_res_flush res {resource:#x}, "
                         f"w {width}, h {height}, x 0, y 0\n")
        lines.append("virtio_gpu_cmd_set_scanout id 0, res 0x0, w 0, h 0, x 0, y 0\n")
        for resource in resources:
            lines.append(f"virtio_gpu_cmd_res_unref res {resource:#x}\n")
    return lines


class VerifierTests(unittest.TestCase):
    def native(self, lines):
        evidence = ScanoutEvidence()
        for line in lines:
            event = decode(line)
            if event:
                evidence.observe(event)
        with redirect_stdout(StringIO()):
            evidence.verify()

    def test_native_lifecycle(self):
        self.native(native_trace())

    def test_no_scanout_is_not_display(self):
        with self.assertRaises(RuntimeError):
            self.native([])

    def test_non_3d_resource_rejected(self):
        with self.assertRaises(RuntimeError):
            self.native([line for line in native_trace() if "create_3d" not in line])

    def test_wrong_format_rejected(self):
        with self.assertRaises(RuntimeError):
            self.native([line.replace("fmt 0x2", "fmt 0x1") for line in native_trace()])

    def test_missing_flush_rejected(self):
        with self.assertRaises(RuntimeError):
            self.native([line for line in native_trace() if "res_flush" not in line])

    def test_active_release_rejected(self):
        with self.assertRaises(RuntimeError):
            self.native([line for line in native_trace() if "res 0x0," not in line])

    def test_resource_leak_rejected(self):
        with self.assertRaises(RuntimeError):
            self.native([line for line in native_trace() if "res_unref" not in line])

    def test_repeated_same_buffer_is_not_flip(self):
        with self.assertRaises(RuntimeError):
            self.native([line.replace("res 0x3,", "res 0x2,")
                         if "scanout" in line or "flush" in line else line
                         for line in native_trace()])

    def oracle(self, codes):
        oracle = object.__new__(DisplayOracle)
        oracle.result = 0
        oracle.output = "".join(
            f"KMS display: code={code} size={'800x600' if code // 16 % 2 else '640x480'} pixels=ok\n"
            for code in codes)
        return oracle

    def test_display_requires_first_frames(self):
        self.oracle(range(64)).verify()
        with self.assertRaises(RuntimeError):
            self.oracle(code for code in range(64) if code % 16).verify()

    def test_display_order(self):
        with self.assertRaises(RuntimeError):
            self.oracle(reversed(range(64))).verify()

    def test_oracle_failure_not_hidden_by_records(self):
        oracle = self.oracle(range(64))
        oracle.result = 1
        with self.assertRaises(RuntimeError):
            oracle.verify()

    def test_negative_control_requires_actual_bad_pixels(self):
        oracle = self.oracle([])
        oracle.result = 1
        with self.assertRaises(RuntimeError):
            oracle.verify_rejection()
        oracle.output = "KMS display oracle: seen=0000000000000000 mismatched=ffffffffffffffff\n"
        with redirect_stdout(StringIO()):
            oracle.verify_rejection()


if __name__ == "__main__":
    unittest.main()
