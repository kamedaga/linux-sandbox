#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Build hosted inputs from the whole canonical Linux boot core, not a closure.

This compiler stage deliberately does not certify a runtime gate. Its input
inventory must include every canonical built-in object, in upstream link order,
and no initcall or boot metadata is removed. Architecture integration and the
final shared-object/runtime checks consume these objects separately.
"""

import argparse
import importlib.util
import json
import pathlib
import shutil
import sys


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "kobox_task_build", SCRIPT_DIR.parent / "task/build_task_smp.py"
)
task = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(task)
memory = task.memory
provider = task.provider

# Reviewed architecture definitions supplied by the existing machine ports.
# Do not derive this allowlist from duplicate symbols: an accidental upper
# implementation must fail the build, not silently override upstream Linux.
MACHINE_DEFINITIONS = {
    "arch/x86/kernel/head64.o": (
        "__pgtable_l5_enabled", "page_offset_base", "pgdir_shift",
        "ptrs_per_p4d", "vmalloc_base", "vmemmap_base",
    ),
    "arch/x86/kernel/head_64.o": (
        "empty_zero_page", "init_top_pgt", "initial_code",
        "level2_kernel_pgt", "level3_kernel_pgt", "phys_base", "smpboot_control",
    ),
    "arch/x86/kernel/setup.o": ("boot_cpu_data", "max_pfn_mapped", "setup_arch"),
    "arch/x86/kernel/setup_percpu.o": (
        "__per_cpu_offset", "cpu_number", "pcpu_populate_pte", "setup_per_cpu_areas",
    ),
    "arch/x86/kernel/cpu/common.o": (
        "__max_threads_per_core", "__preempt_count", "current_task",
        "arch_cpu_finalize_init",
    ),
    "arch/x86/kernel/smpboot.o": ("__cpu_primary_thread_mask", "__max_smt_threads"),
    "arch/x86/kernel/process.o": (
        "arch_cpu_idle", "arch_release_task_struct", "copy_thread",
        "exit_thread",
    ),
    "arch/x86/kernel/process_64.o": (
        "x86_gsbase_read_cpu_inactive", "x86_gsbase_write_cpu_inactive",
    ),
    "arch/x86/kernel/fpu/core.o": ("fpu_thread_struct_whitelist",),
    "arch/x86/kernel/cpu/mtrr/generic.o": ("mtrr_type_lookup",),
    "arch/x86/kernel/fpu/init.o": ("fpu__init_cpu",),
    "arch/x86/kernel/alternative.o": ("text_poke_early",),
    "arch/x86/entry/entry_64.o": ("__switch_to_asm",),
    "arch/x86/mm/init.o": (
        "after_bootmem", "pfn_range_is_mapped", "free_init_pages", "execmem_arch_setup",
    ),
    "arch/x86/mm/init_64.o": (
        "__default_kernel_pte_mask", "__supported_pte_mask", "arch_mm_preinit",
        "arch_sync_kernel_mappings", "kernel_set_to_readonly", "mem_init",
        "vmemmap_populate", "vmemmap_populate_print_last", "mark_rodata_ro",
    ),
    "arch/x86/mm/tlb.o": (
        "__flush_tlb_all", "enter_lazy_tlb", "flush_tlb_kernel_range",
        "switch_mm", "switch_mm_irqs_off", "flush_tlb_mm_range", "arch_tlbbatch_flush",
    ),
    "arch/x86/mm/fault.o": ("pgd_lock",),
    "arch/x86/kernel/tsc.o": ("sched_clock", "sched_clock_noinstr"),
    "arch/x86/kernel/smp.o": ("smp_ops",),
    "arch/x86/kernel/time.o": ("time_init",),
    "arch/x86/kernel/traps.o": ("trap_init",),
    "arch/x86/kernel/irqinit.o": ("init_IRQ",),
    "arch/x86/kernel/irq_work.o": ("arch_irq_work_raise",),
}

MACHINE_LOCAL_EXPORTS = {
    "arch/x86/kernel/traps.o": ("handle_bug", "do_int3"),
    "arch/x86/mm/fault.o": ("do_user_addr_fault",),
}

WEAK_MACHINE_HOOKS = {
    "mm/execmem.o": ("execmem_arch_setup",),
    "init/main.o": ("trap_init",),
    "kernel/irq_work.o": ("arch_irq_work_raise",),
    "kernel/sched/build_policy.o": ("arch_cpu_idle", "arch_cpu_idle_exit"),
    "kernel/fork.o": ("arch_release_task_struct",),
    "mm/mm_init.o": ("arch_mm_preinit", "mem_init"),
    "mm/percpu.o": ("pcpu_populate_pte",),
    "mm/sparse.o": ("vmemmap_populate_print_last",),
    "kernel/sched/build_utility.o": ("sched_clock",),
    "kernel/time/timekeeping.o": ("read_persistent_wall_and_boot_offset",),
}

SOURCES_SPEC = importlib.util.spec_from_file_location(
    "boot_sources", SCRIPT_DIR / "sources.py")
sources = importlib.util.module_from_spec(SOURCES_SPEC)
SOURCES_SPEC.loader.exec_module(sources)

# Address formation and explicit hosted architecture/boot-end boundaries.
# No service initialization or initcall membership changes are permitted.
MACHINE_SOURCE_PATCHES = {
    "arch/x86/lib/iomem.c": "iomem-transactions.patch",
    "arch/x86/kernel/fpu/signal.c": "fpu-user-operand.patch",
    "arch/x86/entry/calling.h": "calling-pic.patch",
    "arch/x86/entry/entry_64.S": "entry_64-pic.patch",
    "arch/x86/kernel/head_64.S": "head_64-pic.patch",
    "arch/x86/kernel/traps.c": "traps-hosted-address.patch",
    "arch/x86/kernel/process_64.c": "register-dump-hosted.patch",
    "arch/x86/mm/pat/set_memory.c": "direct-map-publish.patch",
    "arch/x86/mm/physaddr.c": "physaddr-hosted.patch",
    "mm/vmalloc.c": "ioremap-publish.patch",
    "init/main.c": "main-hosted-init.patch",
}

UPSTREAM_STARTUP_ONLY = {
    "kobox_linux_memory_early_boot", "kobox_linux_task_smp_boot",
    "mm_core_init", "sched_init", "workqueue_init_early", "workqueue_init",
    "rcu_init", "srcu_init", "tick_init", "timers_init", "hrtimers_init",
    "softirq_init", "timekeeping_init", "sched_clock_init", "call_function_init",
    "pid_idr_init", "cred_init", "fork_init", "proc_caches_init", "cpu_stop_init",
    "rcu_scheduler_starting", "idle_threads_init", "kthreadd",
    "vfs_caches_init_early", "vfs_caches_init", "mnt_init", "shmem_init",
    "init_rootfs", "pagecache_init", "kmem_cache_init", "kmem_cache_init_late",
}

REQUIRED_MEMORY_CONFIG = (
    "CONFIG_MMU=y", "CONFIG_SHMEM=y", "CONFIG_TMPFS=y", "CONFIG_MEMFD_CREATE=y",
    "CONFIG_SLUB=y", "CONFIG_SPARSEMEM_VMEMMAP=y",
    "CONFIG_MTRR=y", "CONFIG_X86_PAT=y",
)

REQUIRED_DMA_CONFIG = (
    "CONFIG_IOMMU_SUPPORT=y", "CONFIG_IOMMU_API=y", "CONFIG_IOMMU_DMA=y",
    "CONFIG_IOMMU_IOVA=y", "CONFIG_IOMMU_DEFAULT_DMA_STRICT=y",
)

REQUIRED_IRQ_CONFIG = (
    "CONFIG_KOBOX_HOSTED=y", "CONFIG_IRQ_MSI_LIB=y",
    "CONFIG_PCI_MSI=y", "CONFIG_GENERIC_MSI_IRQ=y",
    "CONFIG_IRQ_DOMAIN_HIERARCHY=y", "CONFIG_SPARSE_IRQ=y",
)

REQUIRED_CLIENT_CONFIG = (
    "CONFIG_MULTIUSER=y", "CONFIG_FUTEX=y", "CONFIG_NET=y", "CONFIG_UNIX=y",
    "CONFIG_BINFMT_ELF=y",
)


class BootBuildError(Exception):
    """The canonical boot inputs cannot be preserved for the hosted runtime."""


def validate_config(config):
    task.validate_config(config)
    for required in REQUIRED_MEMORY_CONFIG:
        if required not in config.splitlines():
            raise BootBuildError(f"boot memory gate requires {required}")
    for required in REQUIRED_DMA_CONFIG:
        if required not in config.splitlines():
            raise BootBuildError(f"boot DMA port requires {required}")
    for required in REQUIRED_IRQ_CONFIG:
        if required not in config.splitlines():
            raise BootBuildError(f"boot IRQ port requires {required}")
    for required in REQUIRED_CLIENT_CONFIG:
        if required not in config.splitlines():
            raise BootBuildError(f"boot client port requires {required}")


def validate_source_order(objects):
    if not objects or len(objects) != len(set(objects)):
        raise BootBuildError("empty or duplicate canonical core object set")
    for name in objects:
        path = pathlib.PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or path.suffix != ".o":
            raise BootBuildError(f"invalid canonical source object: {name}")
    for required in (
        "init/main.o", "kernel/cpu.o", "kernel/smp.o", "kernel/softirq.o",
        "kernel/workqueue.o", "kernel/rcu/tree.o", "kernel/rcu/srcutree.o",
        "mm/shmem.o", "mm/filemap.o", "mm/vmscan.o", "fs/namespace.o",
        "fs/namei.o", "fs/open.o", "fs/read_write.o", "fs/file_table.o",
        "fs/file.o", "fs/inode.o", "fs/dcache.o", "fs/super.o",
        "mm/memory.o", "mm/mprotect.o", "mm/mmap.o", "kernel/fork.o",
        "kernel/cred.o", "kernel/groups.o",
        "fs/exec.o", "fs/binfmt_elf.o",
        "kernel/signal.o", "arch/x86/kernel/signal.o",
        "arch/x86/kernel/fpu/signal.o", "arch/x86/kernel/fpu/core.o",
        "kernel/futex/core.o", "kernel/futex/syscalls.o", "kernel/futex/pi.o",
        "kernel/futex/requeue.o", "kernel/futex/waitwake.o",
        "net/socket.o", "net/core/scm.o", "net/unix/af_unix.o",
        "net/unix/garbage.o",
        "kernel/kthread.o", "arch/x86/mm/fault.o", "arch/x86/mm/pgtable.o",
        "arch/x86/mm/tlb.o",
        "kernel/dma/mapping.o", "lib/scatterlist.o",
        "drivers/iommu/iommu.o", "drivers/iommu/dma-iommu.o", "drivers/iommu/iova.o",
        "kernel/irq/irqdomain.o", "kernel/irq/msi.o", "kernel/irq/manage.o",
        "kernel/irq/chip.o", "drivers/pci/msi/api.o", "drivers/pci/msi/msi.o",
        "drivers/pci/msi/irqdomain.o",
        "drivers/irqchip/irq-msi-lib.o",
    ):
        if required not in objects:
            raise BootBuildError(f"canonical boot core omits {required}")


def input_identity(arguments):
    return {
        name: memory.sha256(arguments.canonical_build_dir / name)
        for name in (".config", "vmlinux", "vmlinux.a")
    }


def prepare_build(arguments, identity):
    target = arguments.provider_build_dir
    marker = target / ".kobox-boot-source.json"
    if target == arguments.canonical_build_dir or target == arguments.source_tree:
        raise BootBuildError("hosted output must not replace an input tree")
    if target.exists():
        if not marker.is_file() or json.loads(marker.read_text()) != identity:
            raise BootBuildError("use a fresh build directory for different boot inputs")
    else:
        shutil.copytree(arguments.canonical_build_dir, target, symlinks=True)
        marker.write_text(json.dumps(identity, sort_keys=True) + "\n")


def section_records(path, objdump):
    records = {}
    for line in task.run([objdump, "--section-headers", path]).splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[0].isdigit():
            try:
                records[fields[1]] = int(fields[2], 16)
            except ValueError:
                continue
    return records


def boot_sections(records):
    return {
        name: size for name, size in records.items()
        if name.startswith(".initcall") or name in (
            ".con_initcall.init", ".init.setup", ".init.text", ".init.data",
            ".init.rodata", "__param",
        )
    }


def validate_initcalls(canonical, hosted, owner):
    def tables(records):
        return {name: size for name, size in records.items()
                if name.startswith(".initcall") or name == ".con_initcall.init"}

    if tables(canonical) != tables(hosted):
        raise BootBuildError(f"boot initcall tables changed: {owner}")


def compile_linker_script(arguments):
    source = arguments.source_tree / "kobox/boot/runtime.lds.S"
    output = arguments.output_dir / "runtime.lds"
    task.run([
        arguments.cc, "-E", "-P", "-x", "assembler-with-cpp", "-nostdinc",
        "-D__ASSEMBLY__", "-D__ASSEMBLER__", "-DLINKER_SCRIPT",
        *provider.provider_include_flags(
            arguments.source_tree, arguments.provider_build_dir
        ).split(),
        source, "-o", output,
    ])
    return output


def validate_machine_overrides(support_defined, definitions):
    records = []
    for symbol in sorted(support_defined & definitions.keys()):
        for definition in definitions[symbol]:
            owner = definition["source_object"]
            weak = definition["symbol_type"] in ("W", "V")
            if weak:
                allowed = WEAK_MACHINE_HOOKS.get(owner, ())
            else:
                allowed = MACHINE_DEFINITIONS.get(owner, ())
            if symbol not in allowed:
                raise BootBuildError(f"unreviewed upstream override: {owner}:{symbol}")
            records.append({"symbol": symbol, "source_object": owner, "weak": weak})
    return records


def validate_startup_ownership(symbols):
    forbidden = sorted(symbols & UPSTREAM_STARTUP_ONLY)
    if forbidden:
        raise BootBuildError(f"machine port bypasses upstream startup: {forbidden[0]}")


def machine_compile_commands(saved, owner, original, staged, output):
    """Replay Kbuild's compiler and objtool without evaluating a shell recipe."""
    try:
        return memory.kbuild_compile_commands(saved, owner, original, staged, output)
    except memory.MemoryBuildError as error:
        raise BootBuildError(str(error)) from error


def compile_machine_patches(arguments):
    root = arguments.output_dir / ".sources"
    patches, objects = [], {}
    for source_name, patch_name in MACHINE_SOURCE_PATCHES.items():
        original = arguments.source_tree / source_name
        staged = root / source_name
        patch = SCRIPT_DIR / "patches" / patch_name
        staged.parent.mkdir(parents=True, exist_ok=True)
        corrected = memory.stage_native_source(arguments, source_name)
        task.run(["patch", "--batch", "--fuzz=0", "--no-backup-if-mismatch",
                  "--output", staged, corrected, patch])
        patches.append({
            "source": source_name, "source_sha256": memory.sha256(original),
            "patch": "kobox/boot/patches/" + patch_name,
            "patch_sha256": memory.sha256(patch),
            "hosted_source_sha256": memory.sha256(staged),
        })
    for record in patches:
        source_name = record["source"]
        if pathlib.PurePosixPath(source_name).suffix not in (".S", ".c"):
            continue
        owner = str(pathlib.PurePosixPath(source_name).with_suffix(".o"))
        canonical = arguments.provider_build_dir / owner
        command_file = canonical.with_name("." + canonical.name + ".cmd")
        saved = command_file.read_text().splitlines()[0].split(" := ", 1)[1]
        output = arguments.output_dir / ".machine" / owner
        output.parent.mkdir(parents=True, exist_ok=True)
        commands = machine_compile_commands(
            saved, owner, arguments.source_tree / source_name, root / source_name, output
        )
        for command in commands:
            task.run(command, cwd=arguments.provider_build_dir)
        # The boundary patches must not remove native entry points or exports.
        def global_names(path):
            return {line.split()[0] for line in task.run([
                arguments.nm, "--extern-only", "--defined-only", "--format=posix", path
            ]).splitlines() if line.split()}
        if global_names(canonical) != global_names(output):
            raise BootBuildError(f"machine patch changed native definitions: {owner}")
        validate_initcalls(section_records(canonical, arguments.objdump),
                           section_records(output, arguments.objdump), owner)
        objects[owner] = output
        record.update({"source_object": owner, "object_sha256": memory.sha256(output)})
    return objects, patches


def compile_module_exports(arguments, source_names, support):
    """Let native modpost generate exports for the already-linked arch port.

    The partial link is metadata input only; the final core retains separate
    canonical objects and their native boot/initcall layout.
    """
    root = arguments.output_dir / ".module-exports"
    root.mkdir(parents=True, exist_ok=True)
    selected = [path for name, path in zip(source_names, support)
                if name in ("kobox/task/port.c", "kobox/task/user.c", "kobox/memory/early_boot.c",
                            "kobox/arch/x86_64/registers.c",
                            "kobox/memory/mmio.c",
                            "kobox/mm/port.c", "kobox/mm/uaccess.c",
                            "kobox/boot/module_exports.c",
                            "kobox/boot/resource_port.c")]
    task.run([arguments.ld, "-r", "-o", root / "vmlinux.o", *selected])
    task.run([arguments.provider_build_dir / "scripts/mod/modpost", "-M", "-E",
              "-o", "core-module.symvers", "vmlinux.o"], cwd=root)
    exports = [task.compile_support(arguments, str(root / ".vmlinux.export.c"))]
    native = root / "native"
    native.mkdir(parents=True, exist_ok=True)
    shutil.copy2(arguments.provider_build_dir / "vmlinux.o", native / "vmlinux.o")
    task.run([arguments.provider_build_dir / "scripts/mod/modpost", "-M", "-E",
              "-o", "native.symvers", "vmlinux.o"], cwd=native)
    exports.append(task.compile_support(arguments, str(native / ".vmlinux.export.c")))
    return exports


def link_runtime(arguments):
    """Strict development link; no unresolved-symbol boundary DSO or stubs."""
    arguments.protocol_include = arguments.source_tree.parent / "protocol/generated/include"
    (arguments.output_dir / ".metadata").mkdir(parents=True, exist_ok=True)
    source_names = sources.support_sources(arguments.with_gates)
    support = [task.compile_support(arguments, source) for source in source_names]
    support.extend(compile_module_exports(arguments, source_names, support))
    support_defined = set().union(*(
        memory.defined_symbols(path, arguments.nm) for path in support
    ))
    support_undefined = set().union(*(
        memory.undefined_symbols(path, arguments.nm) for path in support
    ))
    validate_startup_ownership(support_defined | support_undefined)
    definitions = provider.parse_archive_definitions(arguments.provider_build_dir, arguments.nm)
    overrides = validate_machine_overrides(support_defined, definitions)
    machine_objects, source_patches = compile_machine_patches(arguments)
    objects = []
    local_exports = []
    for owner in provider.canonical_object_order(arguments.provider_build_dir, arguments.ar):
        source = machine_objects.get(owner, arguments.provider_build_dir / owner)
        names = [item["symbol"] for item in overrides
                 if item["source_object"] == owner and not item["weak"]]
        exported = MACHINE_LOCAL_EXPORTS.get(owner, ())
        if exported:
            local = {fields[0] for line in task.run([
                arguments.nm, "--defined-only", "--format=posix", source
            ]).splitlines() if len(fields := line.split()) >= 2 and fields[1] == "t"}
            for name in exported:
                if name not in local or name not in support_undefined:
                    raise BootBuildError(f"native machine helper changed: {owner}:{name}")
                local_exports.append({"source_object": owner, "symbol": name})
        if names or exported:
            destination = arguments.output_dir / ".objects" / owner
            destination.parent.mkdir(parents=True, exist_ok=True)
            task.run([
                arguments.objcopy, *(f"--weaken-symbol={name}" for name in names),
                *(f"--globalize-symbol={name}" for name in exported),
                source, destination,
            ])
            objects.append(destination)
        else:
            objects.append(source)
    # Keep each input separate until the final link. Relinking a merged
    # vmlinux.o would coalesce init text across unrelated source owners.
    response = arguments.output_dir / ".metadata/boot-objects.rsp"
    response.write_text("\n".join(json.dumps(str(path), ensure_ascii=False)
                                  for path in [*objects, *support]) + "\n")
    (arguments.output_dir / "machine-bindings.json").write_text(
        json.dumps({"stage": "link-inputs-not-runtime-certified", "overrides": overrides,
                    "source_patches": source_patches,
                    "native_source_patches": getattr(arguments, "native_source_patches", {}),
                    "local_exports": local_exports,
                    "support_sources": list(source_names),
                    "with_gates": arguments.with_gates,
                    "os_backend": arguments.os_backend, "cpu_arch": arguments.cpu_arch},
                   indent=2, sort_keys=True) + "\n"
    )
    task.run([
        arguments.cc, "-shared", "-nostdlib", "--ld-path=" + arguments.ld,
        "-Wl,-Bsymbolic,-z,defs,-z,now,--build-id=none,--error-limit=20",
        "-Wl,--script=" + str(arguments.output_dir / "runtime.lds"),
        "-Wl,--soname=linux-boot-runtime.so",
        "-o", arguments.output_dir / "linux-boot-runtime.so",
        "@" + str(response),
    ])
    print(task.run([
        sys.executable, SCRIPT_DIR / "inspect_core.py",
        "--core", arguments.output_dir / "linux-boot-runtime.so",
        "--inputs", arguments.output_dir / "linux-boot-inputs.json",
        "--nm", arguments.nm,
    ]).strip(), flush=True)


def build_inputs(arguments):
    config = (arguments.canonical_build_dir / ".config").read_text()
    validate_config(config)
    identity = input_identity(arguments)
    objects = provider.canonical_object_order(
        arguments.canonical_build_dir, arguments.ar
    )
    validate_source_order(objects)
    prepare_build(arguments, identity)
    arguments.architecture_include = arguments.source_tree / "kobox/task/include"
    arguments.extra_include_dirs = [arguments.source_tree / "kobox/boot/include"]
    arguments.architecture = "x86"
    arguments.kernel_release = (
        arguments.canonical_build_dir / "include/config/kernel.release"
    ).read_text().strip()
    arguments.extra_cflags = ["-DKOBOX_BOOT_RUNTIME=1"]
    if arguments.with_gates:
        arguments.extra_cflags.append("-DKOBOX_RUNTIME_GATES=1")

    print(f"Compiling all {len(objects)} canonical boot objects", flush=True)
    # vmlinux_o is the upstream full-tree target, including lib-y inputs.
    # Explicit .a/.o targets activate Kbuild's single-file mode, which can
    # reuse stale archives without descending into their owning directories.
    memory.compile_linux_objects(
        arguments, ["vmlinux.a", "vmlinux.o"], build_targets=["vmlinux_o"]
    )
    if provider.canonical_object_order(arguments.provider_build_dir, arguments.ar) != objects:
        raise BootBuildError("Kbuild changed canonical core membership or input order")
    if (arguments.provider_build_dir / ".config").read_bytes() != (
        arguments.canonical_build_dir / ".config"
    ).read_bytes():
        raise BootBuildError("Kbuild changed the canonical boot configuration")
    records = []
    for owner in objects:
        original = boot_sections(section_records(
            arguments.canonical_build_dir / owner, arguments.objdump
        ))
        hosted = boot_sections(section_records(
            arguments.provider_build_dir / owner, arguments.objdump
        ))
        validate_initcalls(original, hosted, owner)
        records.append({
            "source_object": owner,
            "sha256": memory.sha256(arguments.provider_build_dir / owner),
            "canonical_boot_sections": original,
            "hosted_boot_sections": hosted,
        })
    if not any(record["hosted_boot_sections"].get(".init.text") for record in records):
        raise BootBuildError("hosted boot core has lost its init text")
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    linker_script = compile_linker_script(arguments)
    manifest = {
        "format": "kobox-linux-boot-inputs-dev",
        "stage": "compiled-inputs-not-runtime-certified",
        "root_symbol": "start_kernel",
        "with_gates": arguments.with_gates,
        "os_backend": arguments.os_backend,
        "cpu_arch": arguments.cpu_arch,
        "root_source": "init/main.o",
        "linux": identity,
        "required_memory_config": list(REQUIRED_MEMORY_CONFIG),
        "extra_cflags": arguments.extra_cflags,
        "linker_script": {
            "source": "kobox/boot/runtime.lds.S",
            "source_sha256": memory.sha256(
                arguments.source_tree / "kobox/boot/runtime.lds.S"
            ),
            "preprocessed_sha256": memory.sha256(linker_script),
        },
        "objects": records,
    }
    (arguments.output_dir / "linux-boot-inputs.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    if arguments.link:
        link_runtime(arguments)


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-tree", "canonical-build-dir", "provider-build-dir", "output-dir"):
        parser.add_argument("--" + name, required=True, type=pathlib.Path)
    parser.add_argument("--os-backend", choices=("linux",), default="linux")
    parser.add_argument("--cpu-arch", choices=("x86_64",), default="x86_64")
    parser.add_argument("--with-gates", action="store_true",
                        help="Link test workloads; omitted for the production core")
    parser.add_argument("--cc", default="clang-18")
    parser.add_argument("--ld", default="ld.lld")
    parser.add_argument("--llvm", default="-18")
    parser.add_argument("--ar", default="llvm-ar-18")
    parser.add_argument("--objdump", default="llvm-objdump-18")
    parser.add_argument("--nm", default="llvm-nm-18")
    parser.add_argument("--objcopy", default="llvm-objcopy-18")
    parser.add_argument("--link", action="store_true", help="attempt the strict runtime link")
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=4)
    arguments = parser.parse_args()
    for name in ("source_tree", "canonical_build_dir", "provider_build_dir", "output_dir"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    return arguments


def main():
    try:
        build_inputs(parse_arguments())
    except (BootBuildError, task.TaskBuildError, memory.MemoryBuildError,
            provider.ProviderBuildError, OSError, ValueError) as error:
        print(f"Linux hosted boot build: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
