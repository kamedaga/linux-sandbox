#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import copy
import importlib.util
import json
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).with_name("generate_boot_core_inventory.py")
SPEC = importlib.util.spec_from_file_location("boot_core_inventory", SCRIPT)
boot_core_inventory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_core_inventory)


class BootCoreInventoryTest(unittest.TestCase):
    def test_parse_sections(self):
        sections = boot_core_inventory.parse_sections(
            "  [17] .init.text PROGBITS ffffffff81641000 841000 027337 "
            "00 AX 0 0 16\n"
        )
        self.assertEqual(sections[".init.text"]["address"], 0xffffffff81641000)
        self.assertEqual(sections[".init.text"]["size"], 0x27337)

    def test_parse_symbols(self):
        symbols = boot_core_inventory.parse_symbols(
            "start_kernel T ffffffff81641220 230\n"
        )
        self.assertEqual(symbols["start_kernel"]["type"], "T")
        self.assertEqual(symbols["start_kernel"]["address"], 0xffffffff81641220)
        self.assertEqual(symbols["start_kernel"]["size"], 0x230)

    def test_parse_archive_definition(self):
        definitions = boot_core_inventory.parse_archive_definitions(
            "/build/vmlinux.a[init/main.o]: start_kernel T 220 230\n",
            pathlib.Path("/build"),
        )
        self.assertEqual(definitions["start_kernel"], [{
            "source_object": "init/main.o",
            "symbol_type": "T",
        }])

    def test_profile_requires_zero_upper_api_overrides(self):
        profile_path = SCRIPT.parent / "profiles/virtio_gpu_virgl.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        boot_core_inventory.validate_profile(profile)
        modified = copy.deepcopy(profile)
        modified["kernel_runtime"]["upper_api_overrides"] = ["schedule"]
        with self.assertRaisesRegex(
            boot_core_inventory.BootCoreError,
            "upper API override count is not zero",
        ):
            boot_core_inventory.validate_profile(modified)

    def test_profile_retains_required_linux_boundaries(self):
        profile_path = SCRIPT.parent / "profiles/virtio_gpu_virgl.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        boundaries = profile["kernel_runtime"]["required_boundaries"]
        self.assertIn(["__initcall_start", "__initcall_end"], boundaries)
        self.assertIn(["__per_cpu_start", "__per_cpu_end"], boundaries)
        self.assertIn(
            ["__sched_class_highest", "__sched_class_lowest"], boundaries
        )
        self.assertEqual(
            profile["kernel_runtime"]["required_initcall_order"][0],
            "__initcall_start",
        )
        self.assertEqual(
            profile["kernel_runtime"]["required_scheduler_classes"],
            [
                "stop_sched_class", "dl_sched_class", "rt_sched_class",
                "fair_sched_class", "idle_sched_class",
            ],
        )

    def test_linux_source_change_filter_excludes_only_kobox_metadata(self):
        original = boot_core_inventory.run_command
        outputs = iter([
            "kernel/sched/core.c\nkobox/runtime/port.c\nREADME.md\n",
            "kobox/new.c\n",
        ])
        try:
            boot_core_inventory.run_command = lambda unused: next(outputs)
            self.assertEqual(
                boot_core_inventory.linux_source_changes(
                    pathlib.Path("/source"), "commit"
                ),
                ["kernel/sched/core.c"],
            )
        finally:
            boot_core_inventory.run_command = original


if __name__ == "__main__":
    unittest.main()
