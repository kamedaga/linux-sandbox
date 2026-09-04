#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import copy
import importlib.util
import json
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).with_name("generate_closure_inventory.py")
SPEC = importlib.util.spec_from_file_location("closure_inventory", SCRIPT)
closure_inventory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(closure_inventory)


class ClosureInventoryTest(unittest.TestCase):
    def test_parse_config(self):
        values = closure_inventory.parse_config(
            "CONFIG_SMP=y\n# CONFIG_DEBUG_FS is not set\nCONFIG_DRM=m\n"
        )
        self.assertEqual(values["CONFIG_SMP"], "y")
        self.assertEqual(values["CONFIG_DEBUG_FS"], "n")
        self.assertEqual(values["CONFIG_DRM"], "m")

    def test_parse_symvers(self):
        symbols = closure_inventory.parse_symvers(
            "0x0\talpha\tvmlinux\tEXPORT_SYMBOL\t\n"
            "0x0\tbeta\tdrivers/example\tEXPORT_SYMBOL_GPL\tDMA_BUF\n"
        )
        self.assertEqual(symbols["alpha"]["owner"], "vmlinux")
        self.assertEqual(symbols["beta"]["namespace"], "DMA_BUF")

    def test_parse_undefined_symbols(self):
        symbols = closure_inventory.parse_undefined_symbols(
            "required U\noptional w\ndefined T 0 4\n"
        )
        self.assertEqual(symbols, [
            {"name": "optional", "weak": True},
            {"name": "required", "weak": False},
        ])

    def test_resolve_export_rejects_missing_strong_provider(self):
        with self.assertRaisesRegex(
            closure_inventory.ClosureError,
            "strong symbol has no export: driver.ko: required",
        ):
            closure_inventory.resolve_export(
                "driver.ko", {"name": "required", "weak": False}, {},
            )

    def test_resolve_export_accepts_missing_weak_provider(self):
        self.assertIsNone(closure_inventory.resolve_export(
            "driver.ko", {"name": "optional", "weak": True}, {},
        ))

    def test_validate_import_namespace_rejects_missing_namespace(self):
        with self.assertRaisesRegex(
            closure_inventory.ClosureError,
            "missing imported namespace DMA_BUF: driver.ko: dma_buf_export",
        ):
            closure_inventory.validate_import_namespace(
                "driver.ko", "dma_buf_export", "DMA_BUF", [],
            )

    def test_parse_softdeps(self):
        before, after = closure_inventory.parse_softdeps([
            "pre: alpha beta post: gamma"
        ])
        self.assertEqual(before, ["alpha", "beta"])
        self.assertEqual(after, ["gamma"])

    def test_profile_is_boot_rooted_and_modules_only(self):
        profile_path = SCRIPT.parent / "profiles/virtio_gpu_virgl.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        closure_inventory.validate_profile(profile)
        runtime = profile["kernel_runtime"]
        self.assertEqual(runtime["root_symbol"], "start_kernel")
        self.assertEqual(runtime["driver_closure"], "modules-only")
        self.assertEqual(runtime["upper_api_overrides"], [])
        self.assertTrue(all(root.endswith(".ko") for root in profile["roots"]))
        self.assertNotIn("shared_providers", profile)

    def test_profile_rejects_upper_api_override(self):
        profile_path = SCRIPT.parent / "profiles/virtio_gpu_virgl.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        profile = copy.deepcopy(profile)
        profile["kernel_runtime"]["upper_api_overrides"] = ["schedule"]
        with self.assertRaisesRegex(
            closure_inventory.ClosureError,
            "upper API overrides must be empty",
        ):
            closure_inventory.validate_profile(profile)

    def test_profile_rejects_incomplete_device_resources(self):
        profile_path = SCRIPT.parent / "profiles/virtio_gpu_virgl.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        profile = copy.deepcopy(profile)
        profile["resource_slots"].pop()
        with self.assertRaisesRegex(
            closure_inventory.ClosureError,
            "incomplete device resource slot set",
        ):
            closure_inventory.validate_profile(profile)

    def test_topological_order_contains_only_input_modules(self):
        nodes = {"driver.ko", "transport.ko", "core.ko"}
        order = closure_inventory.topological_order(
            nodes,
            {("driver.ko", "transport.ko"), ("transport.ko", "core.ko")},
            set(),
        )
        self.assertEqual(order, ["core.ko", "transport.ko", "driver.ko"])

    def test_topological_order_rejects_cycle(self):
        with self.assertRaises(closure_inventory.ClosureError):
            closure_inventory.topological_order(
                {"a.ko", "b.ko"},
                {("a.ko", "b.ko"), ("b.ko", "a.ko")},
                set(),
            )


if __name__ == "__main__":
    unittest.main()
