#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import importlib.util
import pathlib
import unittest


SPEC = importlib.util.spec_from_file_location(
    "gem_build", pathlib.Path(__file__).with_name("build_gem_modules.py")
)
gem = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gem)


class GemModuleBuildTest(unittest.TestCase):
    def test_virtio_gpu_closure_requires_native_transport_and_kms(self):
        config = "\n".join((
            "CONFIG_MODULES=y", "CONFIG_DRM=m", "CONFIG_DRM_GEM_SHMEM_HELPER=m",
            "CONFIG_VIRTIO=m", "CONFIG_VIRTIO_PCI=m", "CONFIG_VIRTIO_PCI_LIB=m",
            "CONFIG_VIRTIO_DMA_SHARED_BUFFER=m", "CONFIG_DRM_KMS_HELPER=m",
            "CONFIG_DRM_VIRTIO_GPU=m", "CONFIG_DRM_VIRTIO_GPU_KMS=y"))
        self.assertEqual(gem.module_objects(config, False), gem.GEM_OBJECTS)
        self.assertEqual(gem.module_objects(config, True),
                         gem.GEM_OBJECTS + gem.VIRTIO_GPU_OBJECTS)
        for setting in config.splitlines():
            with self.subTest(setting=setting), self.assertRaises(gem.boot.BootBuildError):
                gem.module_objects(config.replace(setting, ""), True)

    def relocation_record(self, kind, allocated=True):
        return {"Sections": [
            {"Section": {"Index": 1, "Name": {"Name": ".text"},
                         "Flags": {"Value": 6 if allocated else 0}}},
            {"Section": {"Index": 2, "Info": 1}},
        ], "Relocations": [{"SectionIndex": 2, "Relocs": [{"Relocation": {
            "Type": {"Name": kind}, "Symbol": {"Name": "target"},
        }}]}]}

    def test_native_relocations_without_host_got_or_truncation(self):
        for kind in ("R_X86_64_64", "R_X86_64_PC32", "R_X86_64_PLT32",
                     "R_X86_64_PC64", "R_X86_64_NONE"):
            self.assertEqual(gem.validate_relocations(self.relocation_record(kind)), 1)
        for kind in ("R_X86_64_32", "R_X86_64_32S", "R_X86_64_GOT64",
                     "R_X86_64_REX_GOTPCRELX", "R_X86_64_GOTOFF64"):
            with self.subTest(kind=kind), self.assertRaises(gem.boot.BootBuildError):
                gem.validate_relocations(self.relocation_record(kind))

    def test_nonallocated_debug_relocations_are_not_runtime_addresses(self):
        self.assertEqual(gem.validate_relocations(
            self.relocation_record("R_X86_64_32", allocated=False)), 0)

    def test_matching_canonical_inputs(self):
        identity = {".config": "configuration", "vmlinux": "image", "vmlinux.a": "archive"}
        gem.validate_core_identity({"linux": dict(identity)}, identity)

    def test_missing_or_changed_core_identity_is_rejected(self):
        identity = {".config": "configuration", "vmlinux": "image", "vmlinux.a": "archive"}
        for record in ({}, {"linux": None}, {"linux": {}}, *(
                {"linux": {**identity, key: "different"}} for key in identity)):
            with self.subTest(record=record), self.assertRaises(gem.boot.BootBuildError):
                gem.validate_core_identity(record, identity)


if __name__ == "__main__":
    unittest.main()
