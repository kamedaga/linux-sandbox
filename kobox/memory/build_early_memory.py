#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Build the boot-time Linux memory closure and its hosted architecture port."""

import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import types


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
PROVIDER_SCRIPT = SCRIPT_DIR.parent / "provider/build_shared_providers.py"
SPEC = importlib.util.spec_from_file_location("kobox_provider_build", PROVIDER_SCRIPT)
provider = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provider)

ROOT_SYMBOL = "kobox_linux_memory_early_boot"
SUPPORT_SOURCE = "kobox/memory/early_boot.c"
HOST_IMPORTS = {"_GLOBAL_OFFSET_TABLE_", "__tls_get_addr"}
MEMORY_SOURCE_OBJECTS = frozenset({
    "arch/x86/lib/clear_page_64.o",
    "arch/x86/lib/hweight.o",
    "arch/x86/lib/memcpy_64.o",
    "arch/x86/lib/memmove_64.o",
    "arch/x86/lib/memset_64.o",
    "arch/x86/mm/pat/set_memory.o",
    "arch/x86/mm/pgtable.o",
    "kernel/fork.o",
    "kernel/locking/mutex.o",
    "kernel/locking/rwsem.o",
    "kernel/locking/spinlock.o",
    "kernel/notifier.o",
    "kernel/sched/build_utility.o",
    "lib/bitmap.o",
    "lib/find_bit.o",
    "lib/math/reciprocal_div.o",
    "lib/radix-tree.o",
    "lib/rbtree.o",
    "lib/string.o",
    "lib/vsprintf.o",
    "lib/xarray.o",
    "mm/execmem.o",
    "mm/init-mm.o",
    "mm/memblock.o",
    "mm/memory.o",
    "mm/mm_init.o",
    "mm/mmzone.o",
    "mm/page_alloc.o",
    "mm/percpu.o",
    "mm/pgtable-generic.o",
    "mm/shrinker.o",
    "mm/slab_common.o",
    "mm/slub.o",
    "mm/sparse-vmemmap.o",
    "mm/sparse.o",
    "mm/util.o",
    "mm/vmalloc.o",
    "mm/vmstat.o",
})
PHASE_BOUNDARY_SYMBOLS = frozenset({
    "___ratelimit",
    "__local_bh_enable_ip",
    "__show_mem",
    "__virt_addr_valid",
    "add_taint",
    "call_rcu",
    "dump_page",
    "flush_tlb_all",
    "flush_work",
    "init_user_ns",
    "io_schedule_finish",
    "io_schedule_prepare",
    "irq_work_queue",
    "lru_add_drain_cpu",
    "mlock_drain_remote",
    "node_dirty_ok",
    "oom_lock",
    "osq_lock",
    "osq_unlock",
    "out_of_memory",
    "panic",
    "print_modules",
    "queue_work_on",
    "queued_read_lock_slowpath",
    "queued_spin_lock_slowpath",
    "queued_write_lock_slowpath",
    "refcount_warn_saturate",
    "sched_clock",
    "schedule_preempt_disabled",
    "schedule_timeout_uninterruptible",
    "synchronize_rcu",
    "system_percpu_wq",
    "try_to_free_pages",
    "wake_q_add",
    "wake_q_add_safe",
    "wake_up_q",
    "wakeup_kswapd",
    "zone_reclaimable_pages",
})
PHASE_BOUNDARY_DATA = frozenset({
    "init_user_ns",
    "oom_lock",
    "system_percpu_wq",
})
INITCALL_SECTIONS = (
    ".initcallearly.init",
    *(f".initcall{level}{suffix}.init"
      for level in (*range(8), "rootfs") for suffix in ("", "s")),
)
BOOT_METADATA_SECTIONS = (
    ".altinstructions",
    ".altinstr_replacement",
    ".smp_locks",
    "__ex_table",
    "__param",
    ".discard.addressable",
    ".discard.annotate_insn",
    ".export_symbol",
    "runtime_ptr_USER_PTR_MAX",
    "runtime_ptr_dentry_hashtable",
    "runtime_shift_d_hash_shift",
)
OBJECT_SECTION_SLICES = {
    "kernel/fork.o": (
        ".ltext.unlikely.mm_cache_init",
        ".rela.ltext.unlikely.mm_cache_init",
        ".lbss.mm_cachep",
        ".rodata.str1.1",
    ),
    "kernel/sched/build_utility.o": (
        ".ltext.__init_swait_queue_head",
        ".ltext.__init_waitqueue_head",
    ),
}
SECTION_LINKER_SYMBOLS = {
    "__alt_instructions",
    "__alt_instructions_end",
    "__initcall_start",
    "__initcall0_start",
    "__initcall1_start",
    "__initcall2_start",
    "__initcall3_start",
    "__initcall4_start",
    "__initcall5_start",
    "__initcallrootfs_start",
    "__initcall6_start",
    "__initcall7_start",
    "__initcall_end",
    "__con_initcall_start",
    "__con_initcall_end",
    "__setup_start",
    "__setup_end",
    "__smp_locks",
    "__smp_locks_end",
    "__start___ex_table",
    "__stop___ex_table",
    "__start___param",
    "__stop___param",
}
LINKER_SYMBOLS = SECTION_LINKER_SYMBOLS | {
    "__bss_stop",
    "__bss_start",
    "__brk_base",
    "__brk_limit",
    "__cpuidle_text_end",
    "__cpuidle_text_start",
    "__end_init_stack",
    "__end_of_kernel_reserve",
    "__end_rodata",
    "__end_rodata_hpage_align",
    "__init_begin",
    "__init_end",
    "__per_cpu_end",
    "__per_cpu_hot_end",
    "__per_cpu_hot_start",
    "__per_cpu_start",
    "__sched_class_highest",
    "__sched_class_lowest",
    "__start_init_stack",
    "__start_runtime_ptr_USER_PTR_MAX",
    "__stop_runtime_ptr_USER_PTR_MAX",
    "__start_runtime_ptr_dentry_hashtable",
    "__stop_runtime_ptr_dentry_hashtable",
    "__start_runtime_shift_d_hash_shift",
    "__stop_runtime_shift_d_hash_shift",
    "__start_rodata",
    "__top_init_kernel_stack",
    "__x86_cpu_dev_start",
    "__x86_cpu_dev_end",
    "_einittext",
    "_brk_end",
    "_edata",
    "_etext",
    "_end",
    "_sdata",
    "_sinittext",
    "_stext",
    "_text",
    "init_stack",
    "init_thread_union",
    "jiffies",
}
GATE_SYMBOLS = {
    "mm_core_init": "mm/mm_init.o",
    "free_area_init": "mm/mm_init.o",
    "memblock_add": "mm/memblock.o",
    "memblock_free_all": "mm/memblock.o",
    "__alloc_pages_noprof": "mm/page_alloc.o",
    "__kmalloc_noprof": "mm/slub.o",
    "__kmem_cache_create_args": "mm/slab_common.o",
    "kmem_cache_alloc_noprof": "mm/slub.o",
    "mm_cache_init": "kernel/fork.o",
    "pcpu_embed_first_chunk": "mm/percpu.o",
    "pcpu_alloc_noprof": "mm/percpu.o",
    "page_alloc_init_cpuhp": "mm/page_alloc.o",
    "radix_tree_init": "lib/radix-tree.o",
    "sparse_init": "mm/sparse.o",
    "vmap": "mm/vmalloc.o",
    "__init_swait_queue_head": "kernel/sched/build_utility.o",
    "__init_waitqueue_head": "kernel/sched/build_utility.o",
}


class MemoryBuildError(Exception):
    """The real Linux memory closure cannot be built."""


def run(arguments, cwd=None):
    result = subprocess.run(
        [str(item) for item in arguments],
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise MemoryBuildError(
            f"command failed: {' '.join(str(item) for item in arguments)}: "
            f"{detail}"
        )
    return result.stdout


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def undefined_symbols(path, nm, dynamic=False):
    command = [nm]
    if dynamic:
        command.append("-D")
    command.extend(("--undefined-only", "--format=posix", path))
    result = set()
    for line in run(command).splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in ("U", "w", "v"):
            result.add(fields[0].split("@", 1)[0])
    return result


def defined_symbols(path, nm):
    result = set()
    for line in run([
        nm, "--defined-only", "--format=posix", path
    ]).splitlines():
        fields = line.split()
        if len(fields) >= 2:
            result.add(fields[0])
    return result


def defined_symbol_records(path, nm):
    records = {}
    for line in run([
        nm, "--extern-only", "--defined-only", "--print-size",
        "--format=posix", path,
    ]).splitlines():
        fields = line.split()
        if len(fields) >= 4:
            records[fields[0]] = {
                "type": fields[1],
                "size": int(fields[3], 16),
            }
    return records


def is_function_record(record):
    return record["type"].upper() in ("T", "W")


def selected_definition(symbol, definitions, linked_symbols):
    try:
        return provider.definition_for_symbol(symbol, definitions, linked_symbols)
    except provider.ProviderBuildError as error:
        raise MemoryBuildError(str(error)) from error


def kbuild_compile_commands(saved, owner, original, staged, output):
    """Replay native compiler/objtool commands without shell evaluation."""
    tokens = shlex.split(saved)
    if tokens.count(";") != 1:
        raise MemoryBuildError(f"unexpected Kbuild compiler recipe: {owner}")
    separator = tokens.index(";")
    compiler, objtool = tokens[:separator], tokens[separator + 1:]
    if (compiler.count(str(original)) != 1 or compiler.count(owner) != 1 or
            not objtool or objtool[0] != "./tools/objtool/objtool" or
            objtool[-1] != owner):
        raise MemoryBuildError(f"unexpected Kbuild compiler inputs: {owner}")
    return [[str(output) if item == owner else
             str(staged) if item == str(original) else
             "-Wp,-MMD," + str(output.with_suffix(".d")) if item.startswith("-Wp,-MMD,") else
             item for item in command] for command in (compiler, objtool)]


def stage_native_source(arguments, source_name):
    original = arguments.source_tree / source_name
    if source_name != "arch/x86/mm/pat/set_memory.c":
        return original
    patch = SCRIPT_DIR / "patches/ancestor-rw.patch"
    staged = arguments.output_dir / ".native-sources" / source_name
    staged.parent.mkdir(parents=True, exist_ok=True)
    run(["patch", "--batch", "--fuzz=0", "--no-backup-if-mismatch",
         "--output", staged, original, patch])
    if not hasattr(arguments, "native_source_patches"):
        arguments.native_source_patches = {}
    arguments.native_source_patches[source_name] = {
        "source": source_name, "source_sha256": sha256(original),
        "patch": "kobox/memory/patches/ancestor-rw.patch",
        "patch_sha256": sha256(patch), "hosted_source_sha256": sha256(staged),
    }
    return staged


def native_object(arguments, source_object):
    source = arguments.provider_build_dir / source_object
    if source_object != "arch/x86/mm/pat/set_memory.o":
        return source
    source_name = str(pathlib.PurePosixPath(source_object).with_suffix(".c"))
    staged = stage_native_source(arguments, source_name)
    output = arguments.output_dir / ".native-objects" / source_object
    output.parent.mkdir(parents=True, exist_ok=True)
    saved = source.with_name("." + source.name + ".cmd").read_text().splitlines()[0]
    for command in kbuild_compile_commands(saved.split(" := ", 1)[1], source_object,
            arguments.source_tree / source_name, staged, output):
        run(command, cwd=arguments.provider_build_dir)
    if defined_symbols(source, arguments.nm) != defined_symbols(output, arguments.nm):
        raise MemoryBuildError(f"native patch changed definitions: {source_object}")
    arguments.native_source_patches[source_name]["object_sha256"] = sha256(output)
    return output


def prepare_object(arguments, source_object, support_defined):
    source = native_object(arguments, source_object)
    overlaps = defined_symbols(source, arguments.nm) & support_defined
    destination = arguments.output_dir / ".objects" / source_object
    destination.parent.mkdir(parents=True, exist_ok=True)
    command = [arguments.objcopy]
    section_slice = OBJECT_SECTION_SLICES.get(source_object)
    if section_slice:
        command.extend(
            f"--only-section={section}" for section in section_slice
        )
        command.extend((source, destination))
        run(command)
        return destination
    command.extend((
        "--remove-section=.con_initcall.init",
        "--remove-section=.init.setup",
    ))
    command.extend(
        f"--remove-section={section}" for section in INITCALL_SECTIONS
    )
    command.extend(
        f"--remove-section={section}" for section in BOOT_METADATA_SECTIONS
    )
    command.extend(f"--weaken-symbol={symbol}" for symbol in sorted(overlaps))
    command.extend((source, destination))
    run(command)
    return destination


def link_shared(arguments, objects):
    version = arguments.output_dir / ".memory.map"
    version.write_text(
        "KOBOX_LINUX_MEMORY_DEV {\n"
        "  global:\n"
        f"    {ROOT_SYMBOL};\n"
        "  local:\n"
        "    *;\n"
        "};\n",
        encoding="utf-8",
    )
    base_linker_script = (
        arguments.source_tree / "kobox/provider/provider.lds"
    ).read_text(encoding="utf-8")
    split_marker = "\t\t*(.ltext.*)\n"
    if base_linker_script.count(split_marker) != 1:
        raise MemoryBuildError("provider linker script has no ltext split point")
    memory_linker_script = arguments.output_dir / ".memory.lds"
    memory_linker_script.write_text(
        base_linker_script.replace(split_marker, "").replace(
            "_etext = ADDR(.ltext) + SIZEOF(.ltext);",
            "_etext = ADDR(.data..percpu);",
        ),
        encoding="utf-8",
    )
    output = arguments.output_dir / "linux-early-memory-gate.so"
    command = [
        arguments.ld,
        "-shared",
        "-Bsymbolic",
        "--gc-sections",
        "--unique",
        "--build-id=none",
        "--hash-style=gnu",
        "-z",
        "relro",
        "-z",
        "now",
        "--no-undefined-version",
        "-u",
        ROOT_SYMBOL,
        "-u",
        "jiffies_64",
        "--defsym=jiffies=jiffies_64",
        f"--version-script={version}",
        f"--script={memory_linker_script}",
        "--soname=linux-early-memory-gate.so",
        "-o",
        output,
        *objects,
    ]
    run(command)
    return output


def build_phase_boundary(arguments, definitions):
    source = arguments.output_dir / ".memory-boundary.S"
    guard_source = arguments.output_dir / ".memory-boundary.c"
    output = arguments.output_dir / "linux-early-memory-boundary.so"
    lines = [
        "/* Generated fail-closed boundary for phases after early memory. */",
        ".section .note.GNU-stack,\"\",@progbits",
        ".text",
    ]
    for index, (symbol, definition) in enumerate(sorted(definitions.items())):
        if definition["type"] != "function":
            continue
        lines.extend((
            ".p2align 4",
            f".globl {symbol}",
            f".type {symbol},@function",
            f"{symbol}:",
            f"\tleaq .Lboundary_name_{index}(%rip), %rdi",
            "\tjmp kobox_phase_boundary_function",
            f".size {symbol}, .-{symbol}",
            ".section .rodata",
            f".Lboundary_name_{index}:",
            f"\t.asciz \"{symbol}\"",
            ".text",
        ))
    lines.extend((
        ".section .phase_boundary_data,\"aw\",@nobits",
        ".balign 4096",
        ".globl kobox_phase_boundary_data_start",
        ".hidden kobox_phase_boundary_data_start",
        "kobox_phase_boundary_data_start:",
    ))
    for symbol, definition in sorted(definitions.items()):
        if definition["type"] != "data":
            continue
        size = max(1, definition["size"])
        lines.extend((
            ".balign 64",
            f".globl {symbol}",
            f".type {symbol},@object",
            f"{symbol}:",
            f"\t.zero {size}",
            f".size {symbol}, {size}",
        ))
    lines.extend((
        ".balign 4096",
        ".globl kobox_phase_boundary_data_end",
        ".hidden kobox_phase_boundary_data_end",
        "kobox_phase_boundary_data_end:",
    ))
    source.write_text("\n".join(lines) + "\n", encoding="utf-8")
    guard_source.write_text(
        "#define _GNU_SOURCE\n"
        "#include <sys/mman.h>\n"
        "#include <unistd.h>\n"
        "extern char kobox_phase_boundary_data_start[];\n"
        "extern char kobox_phase_boundary_data_end[];\n"
        "__attribute__((visibility(\"hidden\"), noreturn))\n"
        "void kobox_phase_boundary_function(const char *name)\n"
        "{\n"
        "\tstatic const char prefix[] = \"early-memory boundary: \";\n"
        "\tsize_t length = 0;\n"
        "\twhile (name[length])\n"
        "\t\tlength++;\n"
        "\t(void)write(2, prefix, sizeof(prefix) - 1);\n"
        "\t(void)write(2, name, length);\n"
        "\t(void)write(2, \"\\n\", 1);\n"
        "\t_exit(126);\n"
        "}\n"
        "__attribute__((constructor)) static void protect_boundary_data(void)\n"
        "{\n"
        "\tsize_t size = (size_t)(kobox_phase_boundary_data_end -\n"
        "\t\tkobox_phase_boundary_data_start);\n"
        "\tif (size && mprotect(kobox_phase_boundary_data_start, size,\n"
        "\t\tPROT_NONE) != 0)\n"
        "\t\t_exit(127);\n"
        "}\n",
        encoding="utf-8",
    )
    run([
        arguments.cc, "-shared", "-Wl,-z,defs",
        "-Wl,--build-id=none", "-Wl,-soname,linux-early-memory-boundary.so",
        source, guard_source, "-o", output,
    ])
    return output


def compile_linux_objects(arguments, source_objects, *, build_targets=None,
                          external_module=None):
    namespace = types.SimpleNamespace(
        make=arguments.make,
        source_tree=arguments.source_tree,
        provider_build_dir=arguments.provider_build_dir,
        llvm=arguments.llvm,
        cc=arguments.cc,
        ld=arguments.ld,
        jobs=getattr(arguments, "jobs", 1),
        architecture=getattr(arguments, "architecture", "x86_64"),
    )
    outputs = sorted(
        item for item in source_objects if item != "vmlinux-linker-defined"
    )
    targets = list(build_targets) if build_targets is not None else outputs
    if not targets:
        return
    architecture_include = getattr(arguments, "architecture_include", None)
    extra_includes = getattr(arguments, "extra_include_dirs", ())
    overlay_identity = None
    if architecture_include:
        digest = hashlib.sha256()
        for root in [*extra_includes, architecture_include]:
            for header in sorted(root.rglob("*.h")):
                digest.update(str(header.relative_to(root)).encode())
                digest.update(b"\0")
                digest.update(header.read_bytes())
        overlay_identity = digest.hexdigest()
    command = provider.make_arguments(namespace, tuple(targets), include_overlay=True)
    if external_module is not None:
        command.insert(-len(targets), f"M={arguments.source_tree / external_module}")
        command.insert(-len(targets), f"MO={arguments.provider_build_dir / external_module}")
    if getattr(arguments, "kernel_release", None):
        command.insert(-len(targets), f"KERNELRELEASE={arguments.kernel_release}")
    for index, item in enumerate(command):
        if str(item).startswith("LINUXINCLUDE=") and getattr(
            arguments, "architecture_include", None
        ):
            command[index] = str(item).replace(
                "LINUXINCLUDE=",
                "LINUXINCLUDE=" + " ".join(
                    f"-I{path}" for path in [*extra_includes, arguments.architecture_include]
                ) + " ", 1
            )
        if str(item).startswith("KCFLAGS="):
            command[index] = (
                str(item) + " -DKOBOX_PROVIDER_FUNCTION_SECTIONS=1"
                " -fvisibility=hidden -DKOBOX_HOSTED_RAM=1"
            )
            # A newly added overlay has no dependency in an older .o.cmd yet.
            if overlay_identity:
                command[index] += f" -DKOBOX_ARCH_OVERLAY_ID=kobox_{overlay_identity}"
            if getattr(arguments, "extra_cflags", None):
                command[index] += " " + " ".join(arguments.extra_cflags)
    objtool_command = (
        arguments.canonical_build_dir / "tools/objtool/.weak.o.cmd"
    )
    if objtool_command.is_file():
        text = objtool_command.read_text(encoding="utf-8")
        include_paths = re.findall(r"(?:^|\s)-I([^\s]+)", text)
        host_include = next(
            (path for path in include_paths if (pathlib.Path(path) / "gelf.h").is_file()),
            None,
        )
        if host_include:
            insertion = -len(targets)
            command.insert(insertion, f"HOSTCFLAGS=-I{host_include}")
            host_usr = pathlib.Path(host_include).parent
            host_libraries = sorted(host_usr.glob("lib*/**/libelf.so"))
            if host_libraries:
                library = host_libraries[0].parent
                command.insert(
                    insertion,
                    f"HOSTLDFLAGS=-L{library} -Wl,-rpath,{library}",
                )
    try:
        base_command = command[:-len(targets)] if targets else command
        for target in targets:
            provider.run_command([*base_command, target])
    except provider.ProviderBuildError as error:
        raise MemoryBuildError(str(error)) from error
    missing = [
        item for item in outputs
        if not (arguments.provider_build_dir / item).is_file()
    ]
    if missing:
        raise MemoryBuildError(f"Kbuild did not produce {missing[0]}")


def build(arguments):
    for required in (
        arguments.canonical_build_dir / ".config",
        arguments.canonical_build_dir / "vmlinux",
        arguments.canonical_build_dir / "vmlinux.a",
    ):
        if not required.is_file():
            raise MemoryBuildError(f"missing canonical Linux input: {required}")
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    (arguments.output_dir / ".metadata").mkdir(parents=True, exist_ok=True)

    provider_identity = {
        "config_sha256": sha256(arguments.canonical_build_dir / ".config"),
        "vmlinux_sha256": sha256(arguments.canonical_build_dir / "vmlinux"),
    }
    identity_path = arguments.provider_build_dir / ".kobox-memory-source.json"
    existing_identity = None
    if identity_path.is_file():
        try:
            existing_identity = json.loads(identity_path.read_text(
                encoding="utf-8"
            ))
        except (json.JSONDecodeError, OSError):
            existing_identity = None
    if (existing_identity != provider_identity or not
            (arguments.provider_build_dir / "tools/objtool/objtool").is_file()):
        if arguments.provider_build_dir.exists():
            shutil.rmtree(arguments.provider_build_dir)
        shutil.copytree(
            arguments.canonical_build_dir,
            arguments.provider_build_dir,
            symlinks=True,
        )
        identity_path.write_text(
            json.dumps(provider_identity, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    if (arguments.provider_build_dir / ".config").read_bytes() != (
        arguments.canonical_build_dir / ".config"
    ).read_bytes():
        raise MemoryBuildError("hosted build config differs from canonical Linux")

    support_namespace = types.SimpleNamespace(
        cc=arguments.cc,
        source_tree=arguments.source_tree,
        provider_build_dir=arguments.provider_build_dir,
        protocol_include=arguments.protocol_include,
    )
    try:
        support = provider.compile_support_source(
            support_namespace, arguments.output_dir, SUPPORT_SOURCE
        )
        definitions = provider.parse_archive_definitions(
            arguments.canonical_build_dir, arguments.nm
        )
        linked_symbols = provider.parse_linked_symbols(
            arguments.canonical_build_dir, arguments.nm
        )
        object_order = provider.canonical_object_order(
            arguments.canonical_build_dir, arguments.ar
        )
    except provider.ProviderBuildError as error:
        raise MemoryBuildError(str(error)) from error

    support_defined = defined_symbols(support, arguments.nm)
    source_objects = set()
    boundary_definitions = {}
    canonical_records = {}

    def add_boundary(symbol, definition):
        source_object = definition["source_object"]
        if source_object not in canonical_records:
            canonical_records[source_object] = defined_symbol_records(
                arguments.canonical_build_dir / source_object, arguments.nm
            )
        record = canonical_records[source_object].get(symbol)
        if record is None:
            raise MemoryBuildError(
                f"cannot classify phase boundary symbol: {symbol}"
            )
        boundary_definitions[symbol] = {
            "source_object": source_object,
            "type": "function" if is_function_record(record) else "data",
            "size": record["size"],
        }

    initial_symbols = undefined_symbols(support, arguments.nm)
    for symbol in sorted(initial_symbols - HOST_IMPORTS - LINKER_SYMBOLS):
        definition = selected_definition(symbol, definitions, linked_symbols)
        source_object = definition["source_object"]
        if source_object == "vmlinux-linker-defined":
            raise MemoryBuildError(f"unmodeled linker symbol: {symbol}")
        if source_object not in MEMORY_SOURCE_OBJECTS:
            raise MemoryBuildError(
                f"early-memory root escaped its object set: {symbol} from "
                f"{source_object}"
            )
        if source_object not in source_objects:
            print(f"  add {source_object} for {symbol}", file=sys.stderr)
            source_objects.add(source_object)
    link_objects = None
    compiled_objects = set()
    for iteration in range(1, arguments.max_iterations + 1):
        print(f"Linux memory closure iteration {iteration}", file=sys.stderr)
        compile_linux_objects(arguments, source_objects - compiled_objects)
        compiled_objects.update(source_objects)
        ordered = provider.ordered_source_objects(source_objects, object_order)
        objects = [support]
        objects.extend(
            prepare_object(arguments, item, support_defined) for item in ordered
        )
        link_objects = objects
        try:
            output = link_shared(arguments, link_objects)
        except MemoryBuildError:
            raise
        else:
            unresolved = undefined_symbols(output, arguments.nm, dynamic=True)
        changed = False
        for symbol in sorted(unresolved - HOST_IMPORTS - LINKER_SYMBOLS):
            definition = selected_definition(symbol, definitions, linked_symbols)
            source_object = definition["source_object"]
            if source_object == "vmlinux-linker-defined":
                raise MemoryBuildError(f"unmodeled linker symbol: {symbol}")
            if source_object not in MEMORY_SOURCE_OBJECTS:
                add_boundary(symbol, definition)
                continue
            if source_object not in source_objects:
                print(f"  add {source_object} for {symbol}", file=sys.stderr)
                source_objects.add(source_object)
                changed = True
        if not changed:
            break
    else:
        raise MemoryBuildError("Linux memory closure did not converge")

    if link_objects is None:
        raise MemoryBuildError("Linux memory closure was not linked")
    output = link_shared(arguments, link_objects)
    final_undefined = undefined_symbols(output, arguments.nm, dynamic=True)
    live_boundary_symbols = (
        final_undefined - HOST_IMPORTS - LINKER_SYMBOLS
    )
    for symbol in sorted(live_boundary_symbols):
        if symbol not in boundary_definitions:
            definition = selected_definition(symbol, definitions, linked_symbols)
            add_boundary(symbol, definition)
    if live_boundary_symbols != PHASE_BOUNDARY_SYMBOLS:
        added = sorted(live_boundary_symbols - PHASE_BOUNDARY_SYMBOLS)
        removed = sorted(PHASE_BOUNDARY_SYMBOLS - live_boundary_symbols)
        raise MemoryBuildError(
            f"early-memory phase boundary drift: added={added}, "
            f"removed={removed}"
        )
    actual_boundary_data = {
        symbol for symbol in live_boundary_symbols
        if boundary_definitions[symbol]["type"] == "data"
    }
    if actual_boundary_data != PHASE_BOUNDARY_DATA:
        raise MemoryBuildError("early-memory phase boundary kind changed")
    unexpected = final_undefined - HOST_IMPORTS - set(boundary_definitions)
    if unexpected:
        raise MemoryBuildError(
            f"unexpected host import: {sorted(unexpected)[0]}"
        )
    if provider.elf_type(output, arguments.readelf) != "DYN":
        raise MemoryBuildError("Linux memory core is not a shared object")
    boundary = build_phase_boundary(arguments, {
        symbol: boundary_definitions[symbol]
        for symbol in live_boundary_symbols
    })

    output_defined = defined_symbols(output, arguments.nm)
    for symbol, expected_object in GATE_SYMBOLS.items():
        definition = selected_definition(symbol, definitions, linked_symbols)
        if definition["source_object"] != expected_object:
            raise MemoryBuildError(
                f"gate symbol source differs: {symbol}: "
                f"{definition['source_object']}"
            )
        if expected_object not in source_objects:
            raise MemoryBuildError(f"gate symbol is not linked: {symbol}")
        if symbol in support_defined or symbol not in output_defined:
            raise MemoryBuildError(
                f"gate symbol is not retained from Linux: {symbol}"
            )

    inventory = {
        "format": "kobox-linux-early-memory-dev",
        "linux": {
            "config_sha256": sha256(arguments.canonical_build_dir / ".config"),
            "vmlinux_sha256": sha256(arguments.canonical_build_dir / "vmlinux"),
        },
        "artifact": {
            "path": output.name,
            "sha256": sha256(output),
            "source_objects": provider.ordered_source_objects(
                source_objects, object_order
            ),
        },
        "phase_boundary": {
            "path": boundary.name,
            "sha256": sha256(boundary),
            "imports": {
                symbol: boundary_definitions[symbol]
                for symbol in sorted(live_boundary_symbols)
            },
        },
        "gate_symbols": GATE_SYMBOLS,
        "host_imports": sorted(HOST_IMPORTS),
        "hosted_architecture_symbols": sorted(support_defined),
        "source_patches": getattr(arguments, "native_source_patches", {}),
    }
    arguments.inventory.write_text(
        json.dumps(inventory, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-tree", type=pathlib.Path, required=True)
    parser.add_argument("--canonical-build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--provider-build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--protocol-include", type=pathlib.Path, required=True)
    parser.add_argument("--inventory", type=pathlib.Path, required=True)
    parser.add_argument("--max-iterations", type=int, default=128)
    parser.add_argument("--cc", default="clang-18")
    parser.add_argument("--ld", default="ld.lld")
    parser.add_argument("--llvm", default="-18")
    parser.add_argument("--nm", default="llvm-nm-18")
    parser.add_argument("--ar", default="llvm-ar-18")
    parser.add_argument("--readelf", default="llvm-readelf-18")
    parser.add_argument("--objcopy", default="llvm-objcopy-18")
    parser.add_argument("--make", default="make")
    arguments = parser.parse_args()
    if arguments.max_iterations < 1:
        parser.error("--max-iterations must be positive")
    for name in (
        "source_tree",
        "canonical_build_dir",
        "provider_build_dir",
        "output_dir",
        "protocol_include",
        "inventory",
    ):
        setattr(arguments, name, getattr(arguments, name).resolve())
    return arguments


def main():
    try:
        build(parse_arguments())
    except (MemoryBuildError, OSError) as error:
        print(f"Linux early memory build: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
