#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Build the upstream Linux scheduler closure and its POSIX task port."""

import argparse
import importlib.util
import json
import pathlib
import re
import shutil
import subprocess
import sys
import types


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
MEMORY_SCRIPT = SCRIPT_DIR.parent / "memory/build_early_memory.py"
SPEC = importlib.util.spec_from_file_location("kobox_memory_build", MEMORY_SCRIPT)
memory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(memory)
provider = memory.provider

ROOT_SYMBOL = "kobox_linux_task_smp_boot"
DISPATCH_SYMBOL = "kobox_linux_task_dispatch"
SUPPORT_SOURCES = (
    "kobox/tests/gates/task_smp.c",
    "kobox/arch/x86_64/task.c",
    "kobox/memory/early_boot.c",
    "kobox/task/port.c",
    "kobox/task/time_port.c",
    "kobox/task/time_gate.c",
)
HOST_IMPORTS = {"_GLOBAL_OFFSET_TABLE_", "__tls_get_addr"}
PERCPU_DATA_IMPORTS = {
    "cea_exception_stacks": "arch/x86/mm/cpu_entry_area.o",
    "cpu_tlbstate": "arch/x86/mm/init.o",
    "cpu_tss_rw": "arch/x86/kernel/process.o",
    "dirty_throttle_leaks": "mm/page-writeback.o",
    "fpu_fpregs_owner_ctx": "arch/x86/kernel/fpu/core.o",
}
PERCPU_DATA_OBJECTS = frozenset(PERCPU_DATA_IMPORTS.values())
TASK_SOURCE_OBJECTS = memory.MEMORY_SOURCE_OBJECTS | frozenset({
    "arch/x86/entry/thunk.o",
    "arch/x86/kernel/cpu/aperfmperf.o",
    "arch/x86/kernel/cpu/cacheinfo.o",
    "arch/x86/kernel/cpu/common.o",
    "arch/x86/kernel/irq.o",
    "arch/x86/kernel/hw_breakpoint.o",
    "arch/x86/kernel/smpboot.o",
    "arch/x86/kernel/step.o",
    "arch/x86/kernel/time.o",
    "drivers/char/random.o",
    "drivers/cpufreq/cpufreq.o",
    "fs/file.o",
    "fs/exec.o",
    "fs/file_table.o",
    "fs/fs_struct.o",
    "fs/namei.o",
    "fs/namespace.o",
    "fs/pidfs.o",
    "init/init_task.o",
    "init/main.o",
    "kernel/cpu.o",
    "kernel/context_tracking.o",
    "kernel/cred.o",
    "kernel/events/core.o",
    "kernel/events/hw_breakpoint.o",
    "kernel/exit.o",
    "kernel/fork.o",
    "kernel/kthread.o",
    "kernel/module/main.o",
    "kernel/panic.o",
    "kernel/ksysfs.o",
    "kernel/irq/manage.o",
    "kernel/nsproxy.o",
    "kernel/pid.o",
    "kernel/pid_namespace.o",
    "kernel/rcu/tree.o",
    "kernel/rcu/update.o",
    "kernel/rcu/rcu_segcblist.o",
    "kernel/sched/build_policy.o",
    "kernel/sched/build_utility.o",
    "kernel/sched/core.o",
    "kernel/sched/clock.o",
    "kernel/sched/fair.o",
    "kernel/signal.o",
    "kernel/smp.o",
    "kernel/smpboot.o",
    "kernel/softirq.o",
    "kernel/stop_machine.o",
    "kernel/task_work.o",
    "kernel/time/hrtimer.o",
    "kernel/time/jiffies.o",
    "kernel/time/clockevents.o",
    "kernel/time/clocksource.o",
    "kernel/time/tick-common.o",
    "kernel/time/tick-broadcast.o",
    "kernel/time/tick-oneshot.o",
    "kernel/time/tick-sched.o",
    "kernel/irq_work.o",
    "kernel/time/sleep_timeout.o",
    "kernel/time/timer.o",
    "kernel/time/ntp.o",
    "kernel/time/time.o",
    "kernel/time/timekeeping.o",
    "kernel/time/vsyscall.o",
    "kernel/ucount.o",
    "kernel/unwind/deferred.o",
    "kernel/user.o",
    "kernel/user_namespace.o",
    "kernel/workqueue.o",
    "kernel/locking/percpu-rwsem.o",
    "kernel/locking/rtmutex_api.o",
    "kernel/locking/qspinlock.o",
    "kernel/locking/qrwlock.o",
    "lib/cpumask.o",
    "lib/ctype.o",
    "lib/crypto/blake2s.o",
    "lib/hexdump.o",
    "lib/idr.o",
    "lib/irq_regs.o",
    "lib/kasprintf.o",
    "lib/llist.o",
    "lib/lockref.o",
    "lib/percpu_counter.o",
    "lib/plist.o",
    "lib/timerqueue.o",
    "lib/vdso/datastore.o",
    "mm/mmap.o",
    "mm/vma_init.o",
})
LINKER_SYMBOLS = memory.LINKER_SYMBOLS
LOCAL_INIT_SYMBOLS = {"cpu_stop_init": "kernel/stop_machine.o"}
GATE_SYMBOLS = {
    "cpu_stop_init": "kernel/stop_machine.o",
    "kthreadd": "kernel/kthread.o",
    "sched_init": "kernel/sched/core.o",
    "schedule": "kernel/sched/core.o",
    "try_to_wake_up": "kernel/sched/core.o",
    "wake_up_new_task": "kernel/sched/core.o",
    "set_cpus_allowed_ptr": "kernel/sched/core.o",
    "schedule_tail": "kernel/sched/core.o",
    "do_task_dead": "kernel/sched/core.o",
    "do_exit": "kernel/exit.o",
    "kernel_thread": "kernel/fork.o",
    "user_mode_thread": "kernel/fork.o",
    "irq_enter": "kernel/softirq.o",
    "irq_exit": "kernel/softirq.o",
    "ct_idle_enter": "kernel/context_tracking.o",
    "ct_idle_exit": "kernel/context_tracking.o",
    "default_idle_call": "kernel/sched/build_policy.o",
    "clockevents_config_and_register": "kernel/time/clockevents.o",
    "__clocksource_register_scale": "kernel/time/clocksource.o",
    "hrtimer_interrupt": "kernel/time/hrtimer.o",
    "hrtimer_start_range_ns": "kernel/time/hrtimer.o",
    "hrtimer_cancel": "kernel/time/hrtimer.o",
    "mod_timer": "kernel/time/timer.o",
    "jiffies_64": "kernel/time/timer.o",
    "jiffies_seq": "kernel/time/jiffies.o",
    "tick_handle_periodic": "kernel/time/tick-common.o",
    "sched_tick": "kernel/sched/core.o",
}


class TaskBuildError(Exception):
    """The real Linux task/SMP closure cannot be built."""


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
        raise TaskBuildError(
            f"command failed: {' '.join(str(item) for item in arguments)}: "
            f"{detail}"
        )
    return result.stdout


def compile_support(arguments, source_name):
    source = arguments.source_tree / source_name
    output = arguments.output_dir / ".metadata" / (
        source_name.replace("/", "-") + ".o"
    )
    command = [
        arguments.cc,
        "-fPIC",
        "-mcmodel=large",
        "-ffunction-sections",
        "-fdata-sections",
        "-fno-stack-protector",
        "-fmacro-prefix-map=" + str(arguments.source_tree) + "=linux",
        "-I" + str(arguments.protocol_include),
        *("-I" + str(path) for path in getattr(arguments, "extra_include_dirs", ())),
        "-I" + str(arguments.architecture_include),
        "-nostdinc",
        *provider.provider_include_flags(
            arguments.source_tree, arguments.provider_build_dir
        ).split(),
        "-include",
        str(arguments.source_tree / "include/linux/compiler_types.h"),
        "-D__KERNEL__",
        "-DKOBOX_TASK_PORT_PHASE=1",
        "-DKOBOX_HOSTED_RAM=1",
        *getattr(arguments, "extra_cflags", ()),
        "--target=x86_64-linux-gnu",
        "-std=gnu11",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-address-of-packed-member",
        "-Wno-gnu-variable-sized-type-not-at-end",
        "-Wno-sign-compare",
        # Match upstream Kbuild: VFS QSTR accepts char and unsigned-char names.
        "-Wno-pointer-sign",
        "-Wno-unused-parameter",
        "-fshort-wchar",
        "-funsigned-char",
        "-fno-common",
        "-mno-red-zone",
        # Match Kbuild: generated vector copies must not clobber task FP.
        "-mgeneral-regs-only",
        "-mstackrealign",
        "-c",
        source,
        "-o",
        output,
    ]
    run(command)
    return output


def prepare_object(arguments, source_object, support_defined):
    source = memory.native_object(arguments, source_object)
    overlaps = memory.defined_symbols(source, arguments.nm) & support_defined
    destination = arguments.output_dir / ".objects" / source_object
    destination.parent.mkdir(parents=True, exist_ok=True)
    command = [arguments.objcopy]
    if source_object in PERCPU_DATA_OBJECTS:
        # Retain real Linux per-CPU initializers, not native switch/FPU code.
        command.append("--only-section=.data..percpu*")
    command.extend((
        "--remove-section=.con_initcall.init",
        "--remove-section=.init.setup",
    ))
    command.extend(
        f"--remove-section={section}" for section in memory.INITCALL_SECTIONS
    )
    command.extend(
        f"--remove-section={section}" for section in memory.BOOT_METADATA_SECTIONS
    )
    command.extend(
        f"--weaken-symbol={symbol}" for symbol in sorted(overlaps)
    )
    for symbol, owner in LOCAL_INIT_SYMBOLS.items():
        if owner == source_object:
            if symbol not in memory.defined_symbols(source, arguments.nm):
                raise TaskBuildError(f"missing upstream initcall: {symbol}")
            command.append(f"--globalize-symbol={symbol}")
    command.extend((source, destination))
    run(command)
    return destination


def link_shared(arguments, objects):
    version = arguments.output_dir / ".task.map"
    version.write_text(
        "KOBOX_LINUX_TASK_DEV {\n"
        "  global:\n"
        f"    {ROOT_SYMBOL};\n"
        f"    {DISPATCH_SYMBOL};\n"
        "  local:\n"
        "    *;\n"
        "};\n",
        encoding="utf-8",
    )
    base_script = (
        arguments.source_tree / "kobox/provider/provider.lds"
    ).read_text(encoding="utf-8")
    linker_script = arguments.output_dir / ".task.lds"
    linker_script.write_text(
        memory.isolated_linker_script(base_script),
        encoding="utf-8",
    )
    output = arguments.output_dir / "linux-task-smp-gate.so"
    run([
        arguments.ld,
        "-shared",
        "-Bsymbolic",
        "--gc-sections",
        "--unique",
        "--build-id=none",
        "--hash-style=gnu",
        "-z", "relro",
        "-z", "now",
        "--no-undefined-version",
        "-u", ROOT_SYMBOL,
        "-u", DISPATCH_SYMBOL,
        "-u", "jiffies_64",
        "--defsym=jiffies=jiffies_64",
        f"--version-script={version}",
        f"--script={linker_script}",
        "--soname=linux-task-smp-gate.so",
        "-o", output,
        *objects,
    ])
    memory.validate_isolated_image(arguments, output)
    return output


def build_boundary(arguments, definitions):
    assembly = arguments.output_dir / ".task-boundary.S"
    guard = arguments.output_dir / ".task-boundary.c"
    output = arguments.output_dir / "linux-task-smp-boundary.so"
    lines = [
        "/* Generated fail-closed boundary after task/SMP. */",
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
            f"\tleaq .Ltask_boundary_name_{index}(%rip), %rdi",
            "\tjmp kobox_task_boundary_function",
            f".size {symbol}, .-{symbol}",
            ".section .rodata",
            f".Ltask_boundary_name_{index}:",
            f"\t.asciz \"{symbol}\"",
            ".text",
        ))
    lines.extend((
        ".section .task_boundary_data,\"aw\",@nobits",
        ".balign 4096",
        ".globl kobox_task_boundary_data_start",
        ".hidden kobox_task_boundary_data_start",
        "kobox_task_boundary_data_start:",
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
        ".globl kobox_task_boundary_data_end",
        ".hidden kobox_task_boundary_data_end",
        "kobox_task_boundary_data_end:",
    ))
    assembly.write_text("\n".join(lines) + "\n", encoding="utf-8")
    guard.write_text(
        "#define _GNU_SOURCE\n"
        "#include <sys/mman.h>\n"
        "#include <execinfo.h>\n"
        "#include <unistd.h>\n"
        "extern char kobox_task_boundary_data_start[];\n"
        "extern char kobox_task_boundary_data_end[];\n"
        "__attribute__((visibility(\"hidden\"), noreturn))\n"
        "void kobox_task_boundary_function(const char *name)\n"
        "{\n"
        "\tstatic const char prefix[] = \"task/SMP boundary: \";\n"
        "\tsize_t length = 0;\n"
        "\twhile (name[length])\n"
        "\t\tlength++;\n"
        "\t(void)write(2, prefix, sizeof(prefix) - 1);\n"
        "\t(void)write(2, name, length);\n"
        "\t(void)write(2, \"\\n\", 1);\n"
        "\tvoid *frames[32];\n"
        "\tint count = backtrace(frames, 32);\n"
        "\tbacktrace_symbols_fd(frames, count, 2);\n"
        "\t_exit(126);\n"
        "}\n"
        "__attribute__((constructor)) static void protect_boundary_data(void)\n"
        "{\n"
        "\tsize_t size = (size_t)(kobox_task_boundary_data_end -\n"
        "\t\tkobox_task_boundary_data_start);\n"
        "\tif (size && mprotect(kobox_task_boundary_data_start, size,\n"
        "\t\tPROT_NONE) != 0)\n"
        "\t\t_exit(127);\n"
        "}\n",
        encoding="utf-8",
    )
    run([
        arguments.cc, "-shared", "-mstackrealign", "-Wl,-z,defs", "-Wl,--build-id=none",
        "-Wl,-soname,linux-task-smp-boundary.so", assembly, guard,
        "-o", output,
    ])
    return output


def validate_config(config):
    for required in ("CONFIG_SMP=y", "CONFIG_NR_CPUS=2", "CONFIG_PREEMPT=y",
                     "CONFIG_PREEMPT_COUNT=y", "CONFIG_HIGH_RES_TIMERS=y",
                     "CONFIG_CONTEXT_TRACKING_IDLE=y", "CONFIG_BUG=y",
                     "CONFIG_RCU_EQS_DEBUG=y"):
        if required not in config.splitlines():
            raise TaskBuildError(f"task gate requires {required}")
    if "CONFIG_PREEMPT_DYNAMIC=y" in config.splitlines():
        raise TaskBuildError("task gate requires static PREEMPT configuration")


def parse_percpu_symbols(symbol_table):
    return {
        line.split()[-1] for line in symbol_table.splitlines()
        if ".data..percpu" in line
    }


def validate_boundary_states(boundary, is_percpu):
    for symbol, definition in boundary.items():
        if is_percpu(symbol, definition["source_object"]):
            # A foreign symbol plus __per_cpu_offset can escape PROT_NONE.
            raise TaskBuildError(f"unresolved per-CPU state cannot be guarded: {symbol}")


def build(arguments):
    arguments.architecture_include = arguments.source_tree / "kobox/task/include"
    config = (arguments.canonical_build_dir / ".config").read_text()
    validate_config(config)
    for required in (
        arguments.canonical_build_dir / ".config",
        arguments.canonical_build_dir / "vmlinux",
        arguments.canonical_build_dir / "vmlinux.a",
    ):
        if not required.is_file():
            raise TaskBuildError(f"missing canonical Linux input: {required}")
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    (arguments.output_dir / ".metadata").mkdir(parents=True, exist_ok=True)

    identity = {
        "config_sha256": memory.sha256(arguments.canonical_build_dir / ".config"),
        "vmlinux_sha256": memory.sha256(arguments.canonical_build_dir / "vmlinux"),
    }
    identity_path = arguments.provider_build_dir / ".kobox-task-source.json"
    existing = None
    if identity_path.is_file():
        try:
            existing = json.loads(identity_path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            existing = None
    if existing != identity or not (
        arguments.provider_build_dir / "tools/objtool/objtool"
    ).is_file():
        if arguments.provider_build_dir.exists():
            shutil.rmtree(arguments.provider_build_dir)
        shutil.copytree(
            arguments.canonical_build_dir,
            arguments.provider_build_dir,
            symlinks=True,
        )
        identity_path.write_text(
            json.dumps(identity, sort_keys=True) + "\n", encoding="utf-8"
        )

    support = [compile_support(arguments, item) for item in SUPPORT_SOURCES]
    support_defined = set().union(*(
        memory.defined_symbols(item, arguments.nm) for item in support
    ))
    try:
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
        raise TaskBuildError(str(error)) from error

    source_objects = set()
    boundary_definitions = {}
    canonical_records = {}
    canonical_percpu = {}

    def is_percpu_definition(symbol, owner):
        if owner not in canonical_percpu:
            canonical_percpu[owner] = parse_percpu_symbols(run([
                    arguments.objdump, "--syms",
                    arguments.canonical_build_dir / owner,
                ]))
        return symbol in canonical_percpu[owner]

    def supplies_definition(symbol, owner):
        if owner in TASK_SOURCE_OBJECTS:
            return True
        if PERCPU_DATA_IMPORTS.get(symbol) == owner:
            if not is_percpu_definition(symbol, owner):
                raise TaskBuildError(f"canonical per-CPU definition differs: {symbol}")
            return True
        return False

    def definition_for(symbol):
        if symbol in LOCAL_INIT_SYMBOLS:
            owner = LOCAL_INIT_SYMBOLS[symbol]
            records = run([
                arguments.nm, "--defined-only", "--format=posix",
                arguments.canonical_build_dir / owner,
            ]).splitlines()
            if not any(line.split()[:2] == [symbol, "t"] for line in records):
                raise TaskBuildError(f"canonical initcall differs: {symbol}")
            return {"source_object": owner}
        try:
            return memory.selected_definition(symbol, definitions, linked_symbols)
        except memory.MemoryBuildError as error:
            raise TaskBuildError(str(error)) from error

    def add_boundary(symbol, definition):
        source_object = definition["source_object"]
        if source_object not in canonical_records:
            canonical_records[source_object] = memory.defined_symbol_records(
                arguments.canonical_build_dir / source_object, arguments.nm
            )
        record = canonical_records[source_object].get(symbol)
        if record is None:
            raise TaskBuildError(f"cannot classify phase boundary: {symbol}")
        boundary_definitions[symbol] = {
            "source_object": source_object,
            "type": "function" if memory.is_function_record(record) else "data",
            "size": record["size"],
        }

    initial = set().union(*(
        memory.undefined_symbols(item, arguments.nm) for item in support
    ))
    for symbol in sorted(
        initial - HOST_IMPORTS - LINKER_SYMBOLS - support_defined
    ):
        definition = definition_for(symbol)
        source_object = definition["source_object"]
        if source_object == "vmlinux-linker-defined":
            raise TaskBuildError(f"unmodeled linker symbol: {symbol}")
        if not supplies_definition(symbol, source_object):
            add_boundary(symbol, definition)
        else:
            source_objects.add(source_object)

    compiled = set()
    prepared = {}
    link_objects = None
    for iteration in range(1, arguments.max_iterations + 1):
        print(f"Linux task/SMP closure iteration {iteration}", file=sys.stderr)
        memory.compile_linux_objects(arguments, source_objects - compiled)
        compiled.update(source_objects)
        ordered = provider.ordered_source_objects(source_objects, object_order)
        for item in ordered:
            if item not in prepared:
                prepared[item] = prepare_object(arguments, item, support_defined)
        link_objects = [*support, *(prepared[item] for item in ordered)]
        output = link_shared(arguments, link_objects)
        unresolved = memory.undefined_symbols(output, arguments.nm, dynamic=True)
        changed = False
        for symbol in sorted(unresolved - HOST_IMPORTS - LINKER_SYMBOLS):
            definition = definition_for(symbol)
            source_object = definition["source_object"]
            if source_object == "vmlinux-linker-defined":
                raise TaskBuildError(f"unmodeled linker symbol: {symbol}")
            if not supplies_definition(symbol, source_object):
                add_boundary(symbol, definition)
            elif source_object not in source_objects:
                print(f"  add {source_object} for {symbol}", file=sys.stderr)
                source_objects.add(source_object)
                changed = True
        if not changed:
            break
    else:
        raise TaskBuildError("Linux task/SMP closure did not converge")
    if link_objects is None:
        raise TaskBuildError("Linux task/SMP closure was not linked")

    output = link_shared(arguments, link_objects)
    unresolved = memory.undefined_symbols(output, arguments.nm, dynamic=True)
    live_boundary = unresolved - HOST_IMPORTS - LINKER_SYMBOLS
    for symbol in sorted(live_boundary):
        if symbol not in boundary_definitions:
            add_boundary(symbol, definition_for(symbol))
    validate_boundary_states({
        symbol: boundary_definitions[symbol] for symbol in live_boundary
    }, is_percpu_definition)
    unexpected = unresolved - HOST_IMPORTS - set(boundary_definitions)
    if unexpected:
        raise TaskBuildError(f"unexpected host import: {sorted(unexpected)[0]}")
    boundary = build_boundary(arguments, {
        symbol: boundary_definitions[symbol] for symbol in live_boundary
    })

    output_defined = memory.defined_symbols(output, arguments.nm)
    for symbol, expected_object in GATE_SYMBOLS.items():
        definition = definition_for(symbol)
        if definition["source_object"] != expected_object:
            raise TaskBuildError(
                f"gate symbol source differs: {symbol}: "
                f"{definition['source_object']}"
            )
        if expected_object not in source_objects or symbol in support_defined or (
            symbol not in output_defined
        ):
            raise TaskBuildError(f"gate symbol is not retained from Linux: {symbol}")

    arguments.inventory.write_text(json.dumps({
        "format": "kobox-linux-task-smp-dev",
        "linux": identity,
        "artifact": {
            "path": output.name,
            "sha256": memory.sha256(output),
            "source_objects": provider.ordered_source_objects(
                source_objects, object_order
            ),
            "percpu_data_only_objects": sorted(source_objects & PERCPU_DATA_OBJECTS),
        },
        "phase_boundary": {
            "path": boundary.name,
            "sha256": memory.sha256(boundary),
            "imports": {
                symbol: boundary_definitions[symbol]
                for symbol in sorted(live_boundary)
            },
        },
        "gate_symbols": GATE_SYMBOLS,
        "host_imports": sorted(HOST_IMPORTS),
        "hosted_architecture_symbols": sorted(support_defined),
        "source_patches": getattr(arguments, "native_source_patches", {}),
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")


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
    parser.add_argument("--objdump", default="llvm-objdump-18")
    parser.add_argument("--make", default="make")
    arguments = parser.parse_args()
    for name in (
        "source_tree", "canonical_build_dir", "provider_build_dir",
        "output_dir", "protocol_include", "inventory",
    ):
        setattr(arguments, name, getattr(arguments, name).resolve())
    return arguments


def main():
    try:
        build(parse_arguments())
    except (TaskBuildError, memory.MemoryBuildError, OSError) as error:
        print(f"Linux task/SMP build: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
