# SPDX-License-Identifier: GPL-2.0-only
"""Unit checks of portable verdicts; not a replacement for the rendering Gate."""

import ast
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import unittest

from gates.display import DisplayEvidence
from gates.mesa import MesaEvidence, MesaOptions


class MesaTests(unittest.TestCase):
    def test_invalid_modes_cannot_silently_reduce_the_gate(self):
        for options in ({"recovery": True}, {"multi": True},
                        {"kms": True, "draw": True},
                        {"expect_shared_mismatch": True},
                        {"expect_display_mismatch": True},
                        {"expect_pixel_mismatch": True}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                MesaOptions(**options)

    def context(self):
        evidence = MesaEvidence(MesaOptions())
        for context in (41, 73):
            for event in (("context_create", context, "client"),
                          ("context_submit", context, 100),
                          ("context_destroy", context)):
                evidence.observe("", event)
        return evidence

    def test_device_events_without_native_launcher(self):
        with redirect_stdout(StringIO()):
            self.assertEqual(self.context().verify(0), 0)

    def test_workload_report_cannot_replace_device_observation(self):
        with self.assertRaises(RuntimeError):
            MesaEvidence(MesaOptions()).verify(0)

    def test_native_failure_is_not_hidden_by_valid_evidence(self):
        self.assertEqual(self.context().verify(7), 7)

    def drawing(self):
        evidence = MesaEvidence(MesaOptions(draw=True))
        for cpu in range(2):
            evidence.observe("", ("context_create", cpu, "client"))
            for frame in range(128):
                evidence.observe("", ("context_submit", cpu, 100))
                for phase in range(2):
                    fence = cpu * 256 + frame * 2 + phase
                    evidence.observe("", ("fence_submit", fence))
                    evidence.observe("", ("fence_complete", fence))
            evidence.observe("Mesa draw: frames=128 pixels=551296 fences=256 waits=256 "
                             "reused=125 slots=3 uploads=128 readbacks=128 guards=ok\n")
            evidence.observe("", ("context_destroy", cpu))
        return evidence

    def test_drawing_and_missing_fence(self):
        evidence = self.drawing()
        with redirect_stdout(StringIO()):
            self.assertEqual(evidence.verify(0), 0)
            evidence.completed_fences.remove(1)
            with self.assertRaises(RuntimeError):
                evidence.verify(0)

    def test_pixel_control_requires_cleanup(self):
        evidence = MesaEvidence(MesaOptions(draw=True, expect_pixel_mismatch=True))
        evidence.observe("", ("context_submit", 1, 100))
        evidence.observe("", ("context_destroy", 1))
        evidence.observe("Mesa pixel mismatch: frame=0 x=17 y=23 wrong\n")
        evidence.observe("Mesa failed draw: GL resources and context released\n")
        with self.assertRaises(RuntimeError):
            evidence.verify(10)
        evidence.observe("Native virtio: cleanup=0 drained=1 warnings=0\n")
        with redirect_stdout(StringIO()):
            self.assertEqual(evidence.verify(10), 0)

    def test_display_requires_completed_observation(self):
        records = [(code, *((800, 600) if code // 16 % 2 else (640, 480)))
                   for code in range(64)]
        DisplayEvidence(0, records, True, False).verify()
        for result in (None, 1, -9):
            with self.assertRaises(RuntimeError):
                DisplayEvidence(result, records, True, False).verify()

    def test_common_python_has_no_native_or_qemu_imports(self):
        allowed = {"dataclasses", "re", "display", "multi"}
        for path in Path(__file__).parent.glob("*.py"):
            if path.name.endswith("_test.py"):
                continue
            tree = ast.parse(path.read_text())
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    for name in node.names:
                        self.assertIn(name.name, allowed, path.name)
                elif isinstance(node, ast.ImportFrom):
                    self.assertIn(node.module, allowed, path.name)
            self.assertNotIn("virtio_gpu_cmd_", path.read_text(), path.name)


if __name__ == "__main__":
    unittest.main()
