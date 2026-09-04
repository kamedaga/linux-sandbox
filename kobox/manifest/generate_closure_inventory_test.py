#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import importlib.util
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

    def test_parse_vmlinux_archive_symbol_uses_build_directory(self):
        definitions = closure_inventory.parse_vmlinux_archive_symbols(
            "/build/vmlinux.a[init/main.o]: start_kernel T 0 4\n",
            pathlib.Path("/build"),
        )
        self.assertEqual(definitions["start_kernel"], [{
            "source_object": "init/main.o",
            "symbol_type": "T",
        }])

    def test_cli_accepts_separate_core_build_directory(self):
        import subprocess
        import sys

        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--help"],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("--core-build-dir", result.stdout)

    def test_resolve_export_rejects_missing_strong_provider(self):
        with self.assertRaisesRegex(
            closure_inventory.ClosureError,
            "strong symbol has no export: driver.ko: required",
        ):
            closure_inventory.resolve_export(
                "driver.ko",
                {"name": "required", "weak": False},
                {},
            )

    def test_resolve_export_accepts_missing_weak_provider(self):
        self.assertIsNone(closure_inventory.resolve_export(
            "driver.ko",
            {"name": "optional", "weak": True},
            {},
        ))

    def test_resolve_module_dependency_rejects_missing_provider(self):
        with self.assertRaisesRegex(
            closure_inventory.ClosureError,
            "module dependency is unavailable: virtio",
        ):
            closure_inventory.resolve_module_dependency("virtio", {})

    def test_provider_runtime_exports_are_owned_by_shared_provider(self):
        profile = {
            "shared_providers": [{
                "name": "core.so",
                "runtime_sources": [{
                    "path": "kobox/provider/runtime.c",
                    "exports": ["provider_entry"],
                }],
            }],
        }
        self.assertEqual(
            closure_inventory.provider_runtime_exports(profile)[
                "provider_entry"
            ],
            {
                "owner": "core.so",
                "export": "KBOX_PROVIDER_EXPORT",
                "namespace": "",
                "source_object": "kobox/provider/runtime.c",
            },
        )

    def test_validate_import_namespace_rejects_missing_namespace(self):
        with self.assertRaisesRegex(
            closure_inventory.ClosureError,
            "missing imported namespace DMA_BUF: driver.ko: dma_buf_export",
        ):
            closure_inventory.validate_import_namespace(
                "driver.ko",
                "dma_buf_export",
                "DMA_BUF",
                [],
            )

    def test_parse_softdeps(self):
        before, after = closure_inventory.parse_softdeps([
            "pre: alpha beta post: gamma"
        ])
        self.assertEqual(before, ["alpha", "beta"])
        self.assertEqual(after, ["gamma"])

    def test_classify_provider_uses_longest_prefix(self):
        providers = [
            {
                "name": "core.so",
                "default": True,
                "source_prefixes": [],
            },
            {
                "name": "device.so",
                "default": False,
                "source_prefixes": ["drivers/"],
            },
            {
                "name": "drm.so",
                "default": False,
                "source_prefixes": ["drivers/dma-buf/"],
            },
        ]
        self.assertEqual(
            closure_inventory.classify_provider(
                "drivers/dma-buf/dma-fence.o", providers
            ),
            "drm.so",
        )
        self.assertEqual(
            closure_inventory.classify_provider("kernel/sched/core.o", providers),
            "core.so",
        )

    def test_virtio_profile_keeps_generic_device_model_in_core(self):
        import json

        profile_path = SCRIPT.parent / "profiles/virtio_gpu_virgl.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        providers = profile["shared_providers"]
        self.assertEqual(
            closure_inventory.classify_provider(
                "drivers/base/devres.o", providers
            ),
            "core/primitive.so",
        )
        self.assertEqual(
            closure_inventory.classify_provider(
                "arch/x86/video/video-common.o", providers
            ),
            "drm.so",
        )
        self.assertEqual(
            closure_inventory.classify_provider(
                "kernel/dma/mapping.o", providers
            ),
            "core/primitive.so",
        )
        self.assertEqual(
            closure_inventory.classify_provider(
                "kernel/irq/msi.o", providers
            ),
            "device-pci.so",
        )
        self.assertEqual(
            closure_inventory.classify_provider(
                "kernel/irq/irqdomain.o", providers
            ),
            "device-pci.so",
        )
        self.assertEqual(
            closure_inventory.provider_symbol_overrides(profile)[
                "pci_iounmap"
            ],
            ("device-pci.so", "kobox/provider/device_pci_bridge.c"),
        )

        closure_inventory.validate_profile(profile)
        self.assertEqual(
            [slot["schema"] for slot in profile["resource_slots"]],
            [
                "kobox2.pci-function",
                "kobox2.dma-domain",
                "kobox2.irq-endpoint",
            ],
        )

    def test_virtio_profile_rejects_incomplete_device_resources(self):
        import copy
        import json

        profile_path = SCRIPT.parent / "profiles/virtio_gpu_virgl.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        profile = copy.deepcopy(profile)
        profile["resource_slots"].pop()
        with self.assertRaisesRegex(
            closure_inventory.ClosureError,
            "incomplete device resource slot set",
        ):
            closure_inventory.validate_profile(profile)

    def test_topological_order_prefers_shared_providers(self):
        nodes = {"core.so", "drm.so", "driver.ko", "transport.ko"}
        dependencies = {
            ("drm.so", "core.so"),
            ("driver.ko", "drm.so"),
        }
        order = closure_inventory.topological_order(
            nodes,
            dependencies,
            set(),
            {"core.so", "drm.so"},
        )
        self.assertEqual(order[:2], ["core.so", "drm.so"])
        self.assertLess(order.index("drm.so"), order.index("driver.ko"))

    def test_topological_order_rejects_cycle(self):
        with self.assertRaises(closure_inventory.ClosureError):
            closure_inventory.topological_order(
                {"a", "b"},
                {("a", "b"), ("b", "a")},
                set(),
            )

    def test_linker_defined_vmlinux_symbol(self):
        definition = closure_inventory.select_vmlinux_definition(
            "alias", {}, {"alias"}
        )
        self.assertEqual(definition["source_object"], "vmlinux-linker-defined")


if __name__ == "__main__":
    unittest.main()
