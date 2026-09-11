#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import importlib.util
import pathlib
import tempfile
import types
import unittest


SCRIPT = pathlib.Path(__file__).with_name("build_shared_providers.py")
SPEC = importlib.util.spec_from_file_location("shared_provider_build", SCRIPT)
provider = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provider)


def test_profile():
    return {
        "provider_link": {
            "allowed_host_imports": ["host_call"],
            "linker_aliases": {},
            "linker_symbols": {"linker_value": "core.so"},
        },
        "shared_providers": [
            {
                "name": "core.so",
                "default": True,
                "source_prefixes": [],
                "dependencies": [],
                "runtime_sources": [{
                    "path": "kobox/provider/runtime.c",
                    "exports": ["runtime_call"],
                }],
                "link_exports": ["runtime_call"],
            },
            {
                "name": "device.so",
                "default": False,
                "source_prefixes": ["drivers/"],
                "dependencies": ["core.so"],
                "linux_runtime_sources": [{
                    "path": "kobox/provider/device.c",
                    "exports": ["device_runtime_call"],
                }],
                "link_exports": ["device_runtime_call"],
            },
        ],
    }


class SharedProviderBuildTest(unittest.TestCase):
    def test_hosted_kconfig_entry_is_selected_only_for_hosted_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            build, source = root / "build", root / "source"
            build.mkdir()
            source.mkdir()
            upstream = 'mainmenu "Linux"\nsource "init/Kconfig"\n'
            (source / "Kconfig").write_text(upstream)
            arguments = types.SimpleNamespace(
                make="make", source_tree=source, provider_build_dir=build,
                llvm="-18", cc="clang-18", ld="ld.lld", jobs=1,
            )
            config = build / ".config"
            for text, hosted in (("CONFIG_SMP=y\n", False),
                                 ("# CONFIG_KOBOX_HOSTED is not set\n", False),
                                 ("CONFIG_KOBOX_HOSTED=y\n", True)):
                config.write_text(text)
                command = provider.make_arguments(arguments, ("modules_prepare",))
                entry = build / ".kobox/Kconfig"
                self.assertEqual(f"KBUILD_KCONFIG={entry}" in command, hosted)
            self.assertEqual((source / "Kconfig").read_text(), upstream)
            self.assertEqual(entry.read_text(), upstream + '\nsource "kobox/Kconfig"\n')
            timestamp = entry.stat().st_mtime_ns
            provider.prepare_hosted_kconfig(source, build)
            self.assertEqual(entry.stat().st_mtime_ns, timestamp)

    def test_hosted_kconfig_rejects_in_tree_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            with self.assertRaisesRegex(provider.ProviderBuildError, "out-of-tree"):
                provider.prepare_hosted_kconfig(root, root)

    def test_runtime_exports_retain_their_source(self):
        self.assertEqual(provider.collect_runtime_symbols(test_profile()), {
            "runtime_call": ("core.so", "kobox/provider/runtime.c"),
            "device_runtime_call": (
                "device.so", "kobox/provider/device.c"
            ),
        })

    def test_duplicate_runtime_export_is_rejected(self):
        profile = test_profile()
        profile["shared_providers"][1]["runtime_sources"] = [{
            "path": "kobox/provider/device_runtime.c",
            "exports": ["runtime_call"],
        }]
        with self.assertRaisesRegex(
            provider.ProviderBuildError, "duplicate provider runtime export"
        ):
            provider.collect_runtime_symbols(profile)

    def test_link_export_must_be_a_runtime_export(self):
        profile = test_profile()
        profile["shared_providers"][0]["link_exports"] = ["missing"]
        with self.assertRaisesRegex(
            provider.ProviderBuildError,
            "link export is not a runtime export",
        ):
            provider.validate_link_exports(profile)

    def test_provider_order_places_dependencies_first(self):
        self.assertEqual(
            provider.provider_order(test_profile()),
            ["core.so", "device.so"],
        )

    def test_transitive_dependencies(self):
        profile = test_profile()
        profile["shared_providers"].append({
            "name": "drm.so",
            "default": False,
            "source_prefixes": ["drivers/gpu/"],
            "dependencies": ["device.so"],
        })
        self.assertEqual(
            provider.transitive_dependencies(profile)["drm.so"],
            {"core.so", "device.so"},
        )

    def test_host_import_is_explicit(self):
        resolved = provider.resolve_import(
            "host_call",
            False,
            "core.so",
            test_profile(),
            {},
            set(),
            {"core.so": set(), "device.so": {"core.so"}},
            {"runtime_call": ("core.so", "kobox/provider/runtime.c")},
            {},
        )
        self.assertEqual(resolved["provider"], "host")

    def test_dependency_runtime_export_is_resolved(self):
        resolved = provider.resolve_import(
            "runtime_call",
            False,
            "device.so",
            test_profile(),
            {},
            set(),
            {"core.so": set(), "device.so": {"core.so"}},
            {"runtime_call": ("core.so", "kobox/provider/runtime.c")},
            {},
        )
        self.assertEqual(resolved["provider"], "core.so")
        self.assertEqual(
            resolved["source_object"], "kobox/provider/runtime.c"
        )

    def test_upward_dependency_is_rejected(self):
        definitions = {
            "device_call": [{
                "source_object": "drivers/device.o",
                "symbol_type": "T",
            }]
        }
        with self.assertRaisesRegex(
            provider.ProviderBuildError, "provider dependency points upward"
        ):
            provider.resolve_import(
                "device_call",
                False,
                "core.so",
                test_profile(),
                definitions,
                set(),
                {"core.so": set(), "device.so": {"core.so"}},
                {"runtime_call": ("core.so", "kobox/provider/runtime.c")},
                {},
            )

    def test_unmodeled_linker_symbol_is_rejected(self):
        with self.assertRaisesRegex(
            provider.ProviderBuildError, "linker-defined symbol is not modeled"
        ):
            provider.resolve_import(
                "other_linker_value",
                False,
                "core.so",
                test_profile(),
                {},
                {"other_linker_value"},
                {"core.so": set(), "device.so": {"core.so"}},
                {"runtime_call": ("core.so", "kobox/provider/runtime.c")},
                {},
            )

    def test_missing_weak_import_remains_optional(self):
        resolved = provider.resolve_import(
            "optional_hook",
            True,
            "core.so",
            test_profile(),
            {},
            set(),
            {"core.so": set(), "device.so": {"core.so"}},
            {"runtime_call": ("core.so", "kobox/provider/runtime.c")},
            {},
        )
        self.assertEqual(resolved, {
            "name": "optional_hook",
            "provider": None,
            "optional": True,
        })

    def test_dynamic_symbols_drop_elf_versions(self):
        original = provider.run_command
        try:
            provider.run_command = lambda *unused, **unused_kw: (
                "needed@KOBOX_PROVIDER_DEV U\n"
                "optional@KOBOX_PROVIDER_DEV w\n"
            )
            self.assertEqual(provider.dynamic_undefined("unused", "nm"), [
                {"name": "needed", "weak": False},
                {"name": "optional", "weak": True},
            ])
            provider.run_command = lambda *unused, **unused_kw: (
                "provided@@KOBOX_PROVIDER_DEV T 0 1\n"
            )
            self.assertEqual(
                provider.dynamic_defined("unused", "nm"), {"provided"}
            )
        finally:
            provider.run_command = original

    def test_symbol_category(self):
        self.assertEqual(provider.symbol_category("T"), "function")
        self.assertEqual(provider.symbol_category("W"), "function")
        self.assertEqual(provider.symbol_category("D"), "data")
        self.assertEqual(provider.symbol_category("R"), "data")

    def test_non_pic_relocation_symbols(self):
        error = provider.ProviderBuildError(
            "relocation R_X86_64_PC32 cannot be used against symbol "
            "'needed'; recompile with -fPIC\n"
            "relocation R_X86_64_32S cannot be used against local symbol"
        )
        self.assertEqual(
            provider.non_pic_relocation_symbols(error), ["needed"]
        )

    def test_relocatable_link_roots_only_declared_symbols(self):
        self.assertEqual(
            provider.relocatable_link_arguments(
                "linked.o", {"z_root", "a_root"}, ["one.o", "two.o"]
            ),
            [
                "-r", "--gc-sections", "-o", "linked.o",
                "-u", "a_root", "-u", "z_root", "one.o", "two.o",
            ],
        )
        self.assertEqual(
            provider.relocatable_link_arguments(
                "linked.o", set(), ["one.o"], "prelink.lds"
            ),
            [
                "-r", "--gc-sections", "-o", "linked.o",
                "--script=prelink.lds", "one.o",
            ],
        )

    def test_source_objects_follow_canonical_archive_order(self):
        self.assertEqual(
            provider.ordered_source_objects(
                {"drivers/pci/pci-driver.o", "drivers/pci/probe.o", "extra.o"},
                {
                    "drivers/pci/probe.o": 10,
                    "drivers/pci/pci-driver.o": 11,
                },
            ),
            ["drivers/pci/probe.o", "drivers/pci/pci-driver.o", "extra.o"],
        )


if __name__ == "__main__":
    unittest.main()
