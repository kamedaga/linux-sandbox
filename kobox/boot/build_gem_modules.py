#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Build the pinned native DRM/GEM module closure for the boot-rooted core."""

import argparse
import importlib.util
import json
import pathlib
import sys


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "kobox_boot_build", SCRIPT_DIR / "build_boot_runtime.py"
)
boot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot)

GEM_OBJECTS = (
    "drivers/i2c/i2c-core.o",
    "drivers/gpu/drm/drm_panel_orientation_quirks.o",
    "drivers/gpu/drm/drm.o", "drivers/gpu/drm/drm_shmem_helper.o",
)
VIRTIO_GPU_OBJECTS = (
    "drivers/virtio/virtio.o", "drivers/virtio/virtio_ring.o",
    "drivers/virtio/virtio_pci_modern_dev.o", "drivers/virtio/virtio_pci.o",
    "drivers/virtio/virtio_dma_buf.o", "drivers/gpu/drm/drm_kms_helper.o",
    "drivers/gpu/drm/virtio/virtio-gpu.o",
)
TEST_OBJECTS = ("kobox/gem/lifetime_test.o", "kobox/gem/resource_test.o",
                "kobox/gem/dma_consumer_test.o")
KOBOX_MODULE_OBJECTS = ("kobox/gem/drm_mapping.o",) + TEST_OBJECTS


def module_objects(config, virtio_gpu):
    required = ["CONFIG_MODULES=y", "CONFIG_DRM=m", "CONFIG_DRM_GEM_SHMEM_HELPER=m"]
    if virtio_gpu:
        required.extend(("CONFIG_VIRTIO=m", "CONFIG_VIRTIO_PCI=m",
                         "CONFIG_VIRTIO_PCI_LIB=m", "CONFIG_VIRTIO_DMA_SHARED_BUFFER=m",
                         "CONFIG_DRM_KMS_HELPER=m", "CONFIG_DRM_VIRTIO_GPU=m",
                         "CONFIG_DRM_VIRTIO_GPU_KMS=y"))
    for setting in required:
        if setting not in config.splitlines():
            raise boot.BootBuildError(f"native DRM modules require {setting}")
    return GEM_OBJECTS + (VIRTIO_GPU_OBJECTS if virtio_gpu else ())


def validate_core_identity(core_inputs, identity):
    if core_inputs.get("linux") != identity:
        raise boot.BootBuildError("GEM modules and boot core have different canonical inputs")


def ensure_module_manifests(build_dir, objects):
    for name in objects:
        object_path = build_dir / name
        if not object_path.is_file():
            raise boot.BootBuildError(f"native module object was not built: {name}")
        manifest = object_path.with_suffix(".mod")
        if not manifest.exists():
            # A direct Kbuild target emits the object but, unlike a compound
            # module target, does not emit its one-line modpost membership.
            manifest.write_text(name + "\n")


def validate_relocations(record):
    sections = {entry["Section"]["Index"]: entry["Section"]
                for entry in record["Sections"]}
    supported = {"R_X86_64_NONE", "R_X86_64_64", "R_X86_64_PC32",
                 "R_X86_64_PLT32", "R_X86_64_PC64"}
    count = 0
    for group in record["Relocations"]:
        target = sections[sections[group["SectionIndex"]]["Info"]]
        if not target["Flags"]["Value"] & 2:  # ELF SHF_ALLOC
            continue
        for entry in group["Relocs"]:
            relocation = entry["Relocation"]
            kind = relocation["Type"]["Name"]
            if kind not in supported:
                raise boot.BootBuildError(
                    f"hosted module relocation {kind} in {target['Name']['Name']} "
                    f"against {relocation['Symbol']['Name']} is not supported")
            count += 1
    return count


def build(arguments):
    config = (arguments.canonical_build_dir / ".config").read_text()
    boot.validate_config(config)
    native_objects = module_objects(config, arguments.virtio_gpu)
    identity = boot.input_identity(arguments)
    validate_core_identity(json.loads(
        (arguments.core_dir / "linux-boot-inputs.json").read_text()), identity)
    boot.prepare_build(arguments, identity)
    if (arguments.provider_build_dir / ".config").read_text() != config:
        raise boot.BootBuildError("GEM module build configuration changed")
    arguments.architecture = "x86"
    arguments.architecture_include = arguments.source_tree / "kobox/task/include"
    arguments.extra_include_dirs = [arguments.source_tree / "kobox/boot/include",
                                   arguments.source_tree.parent / "protocol/generated/include"]
    arguments.kernel_release = (
        arguments.canonical_build_dir / "include/config/kernel.release"
    ).read_text().strip()
    # Native modules and the hosted core are within signed PC32 reach (the
    # execmem arch port enforces this). PIE plus direct external data access
    # emits PC-relative references, with no DSO GOT or low-address assumption
    # for Linux's explicitly named data/per-CPU sections.
    arguments.extra_cflags = ["-DKOBOX_BOOT_RUNTIME=1", "-fPIE",
                             "-mcmodel=small", "-fdirect-access-external-data",
                             "-fno-jump-tables", "-include",
                             str(arguments.source_tree /
                                 "kobox/boot/include/kobox/module_visibility.h")]
    objects = native_objects + KOBOX_MODULE_OBJECTS
    boot.memory.compile_linux_objects(arguments, native_objects,
                                      build_targets=[*native_objects, "modules_prepare"])
    boot.memory.compile_linux_objects(arguments, KOBOX_MODULE_OBJECTS,
                                      build_targets=[pathlib.PurePosixPath(name).name
                                                     for name in KOBOX_MODULE_OBJECTS],
                                      external_module="kobox/gem")
    ensure_module_manifests(arguments.provider_build_dir, objects)
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    exports = arguments.core_dir / ".module-exports/core-module.symvers"
    if not exports.is_file():
        raise boot.BootBuildError("build the boot core's native port exports first")
    # Upstream modpost enforces imports/licenses/namespaces and creates native
    # __this_module metadata. Missing imports are errors, never warning-only.
    boot.task.run([
        arguments.provider_build_dir / "scripts/mod/modpost", "-M", "-E",
        "-i", arguments.canonical_build_dir / "vmlinux.symvers",
        "-i", exports, "-o", "gem.symvers",
        *(arguments.provider_build_dir / name for name in objects),
    ], cwd=arguments.provider_build_dir)
    arguments.protocol_include = arguments.source_tree.parent / "protocol/generated/include"
    (arguments.output_dir / ".metadata").mkdir(parents=True, exist_ok=True)
    base_flags = arguments.extra_cflags
    records = []
    for name in objects:
        source = arguments.provider_build_dir / name
        module_name = source.stem.replace("-", "_")
        arguments.extra_cflags = [*base_flags, "-DMODULE",
            f'-DKBUILD_MODNAME="{module_name}"',
            f'-DKBUILD_BASENAME="{module_name}"',
            f"-D__KBUILD_MODNAME=kmod_{module_name}"]
        metadata = boot.task.compile_support(arguments, str(source.with_suffix(".mod.c")))
        common = boot.task.compile_support(arguments, "scripts/module-common.c")
        destination = (arguments.output_dir / name).with_suffix(".ko")
        destination.parent.mkdir(parents=True, exist_ok=True)
        boot.task.run([arguments.ld, "-r", "-T",
                       arguments.provider_build_dir / "scripts/module.lds",
                       "-o", destination, source, metadata, common])
        relocation_count = validate_relocations(json.loads(boot.task.run([
            "llvm-readobj" + arguments.llvm, "--elf-output-style=JSON",
            "--sections", "--relocations", destination,
        ]))[0])
        records.append({"path": str(pathlib.PurePosixPath(name).with_suffix(".ko")),
                        "allocated_relocations": relocation_count,
                        "sha256": boot.memory.sha256(destination),
                        "object_sha256": boot.memory.sha256(source)})
    (arguments.output_dir / "gem-module-inputs.json").write_text(
        json.dumps({"stage": "modules-not-runtime-certified", "linux": identity,
                    "hosted_cflags": base_flags,
                    "core_sha256": boot.memory.sha256(
                        arguments.core_dir / "linux-boot-runtime.so"),
                    "source_patches": [],
                    "modules": records}, indent=2, sort_keys=True) + "\n"
    )
    print("Native DRM modules built with strict modpost; device/runtime Gate pending")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-tree", "canonical-build-dir", "provider-build-dir", "output-dir", "core-dir"):
        parser.add_argument("--" + name, required=True, type=pathlib.Path)
    parser.add_argument("--cc", default="clang-18")
    parser.add_argument("--ld", default="ld.lld")
    parser.add_argument("--llvm", default="-18")
    parser.add_argument("--ar", default="llvm-ar-18")
    parser.add_argument("--nm", default="llvm-nm-18")
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--virtio-gpu", action="store_true",
                        help="also build the real virtio-pci/virtio-gpu and KMS module closure")
    arguments = parser.parse_args()
    for name in ("source_tree", "canonical_build_dir", "provider_build_dir", "output_dir", "core_dir"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    try:
        build(arguments)
    except (boot.BootBuildError, boot.task.TaskBuildError, boot.memory.MemoryBuildError,
            boot.provider.ProviderBuildError, OSError, ValueError) as error:
        print(f"Linux GEM module build: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
