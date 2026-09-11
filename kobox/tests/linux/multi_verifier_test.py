#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Negative checks of trace assertions, not a substitute for real rendering."""

from contextlib import redirect_stdout
from io import StringIO
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from gates.multi import MultiEvidence
from qemu_trace import decode


def pair(complete=True):
    lines = [f"virtio_gpu_cmd_ctx_create ctx {context:#x}, name client\n"
             for context in (1, 2)]
    lines += [f"virtio_gpu_cmd_ctx_res_attach ctx {context:#x}, res {resource:#x}\n"
              for context in (1, 2) for resource in (3, 4)]
    lines += [f"virtio_gpu_cmd_ctx_submit ctx {context:#x}, size 100\n"
              for _ in range(32) for context in (1, 2)]
    if complete:
        lines += [f"virtio_gpu_cmd_ctx_destroy ctx {context:#x}\n" for context in (1, 2)]
    return lines


def evidence(lines):
    result = MultiEvidence()
    for line in lines:
        event = decode(line)
        if event:
            result.observe(event)
    return result


class VerifierTests(unittest.TestCase):
    def test_two_pairs(self):
        with redirect_stdout(StringIO()):
            evidence(pair() * 2).verify()

    def test_no_work(self):
        with self.assertRaises(RuntimeError):
            evidence([]).verify()

    def test_context_leak(self):
        with self.assertRaises(RuntimeError):
            evidence(pair() + pair(False)).verify()

    def test_no_shared_attachment(self):
        with self.assertRaises(RuntimeError):
            evidence(line for line in pair() * 2 if "ctx_res_attach" not in line).verify()

    def test_one_way_share(self):
        with self.assertRaises(RuntimeError):
            evidence(line for line in pair() * 2 if "res 0x4" not in line).verify()

    def test_only_one_submitter(self):
        with self.assertRaises(RuntimeError):
            evidence(line for line in pair() * 2 if "ctx_submit ctx 0x2" not in line).verify()

    def test_sequential_contexts(self):
        lines = pair()
        sequential = [line for context in (1, 2) for line in lines if f"ctx {context:#x}" in line]
        with self.assertRaises(RuntimeError):
            evidence(sequential * 2).verify()

    def test_death_requires_live_shared_clients(self):
        evidence(pair(False)).verify_killed()
        with self.assertRaises(RuntimeError):
            evidence(pair()).verify_killed()
        with self.assertRaises(RuntimeError):
            evidence([]).verify_killed()


if __name__ == "__main__":
    unittest.main()
