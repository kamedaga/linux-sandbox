#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Build deterministic shared providers from a Linux closure inventory."""

import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
MANIFEST_SCRIPT = SCRIPT_DIR.parent / "manifest/generate_closure_inventory.py"
SPEC = importlib.util.spec_from_file_location("closure_inventory", MANIFEST_SCRIPT)
closure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(closure)

PROVIDER_INVENTORY_FORMAT = "kobox-linux-provider-inventory-dev"
PIC_FLAGS = (
    "-fPIC -mcmodel=large -ffunction-sections -fdata-sections "
    "-Wno-unused-but-set-variable -DKOBOX_PROVIDER=1"
)


class ProviderBuildError(Exception):
    """The declared shared-provider closure cannot be built."""


def collect_runtime_symbols(profile):
    result = {}

    for definition in profile["shared_providers"]:
        for source in definition.get("runtime_sources", []):
            path = source.get("path")
            exports = source.get("exports")
            if not isinstance(path, str) or not path.startswith("kobox/provider/"):
                raise ProviderBuildError("invalid provider runtime source path")
            if not isinstance(exports, list) or not exports:
                raise ProviderBuildError(
                    f"provider runtime source has no exports: {path}"
                )
            for symbol in exports:
                if not isinstance(symbol, str) or not re.fullmatch(
                    r"[A-Za-z0-9_]+", symbol
                ):
                    raise ProviderBuildError(
                        f"invalid provider runtime export: {symbol}"
                    )
                if symbol in result:
                    raise ProviderBuildError(
                        f"duplicate provider runtime export: {symbol}"
                    )
                result[symbol] = (definition["name"], path)
    return result


def run_command(arguments, cwd=None, env=None):
    result = subprocess.run(
        [str(item) for item in arguments],
        cwd=cwd,
        env=env,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ProviderBuildError(
            f"command failed: {' '.join(str(item) for item in arguments)}: "
            f"{detail}"
        )
    return result.stdout


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProviderBuildError(f"cannot read {path}: {error}") from error


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tool_version(tool):
    output = run_command([tool, "--version"])
    return output.splitlines()[0].strip()


def provider_order(profile):
    names = {item["name"] for item in profile["shared_providers"]}
    edges = {
        (item["name"], dependency)
        for item in profile["shared_providers"]
        for dependency in item.get("dependencies", [])
    }
    return closure.topological_order(names, edges, set(), names)


def transitive_dependencies(profile):
    direct = {
        item["name"]: set(item.get("dependencies", []))
        for item in profile["shared_providers"]
    }
    result = {}
    for name in direct:
        found = set()
        pending = list(direct[name])
        while pending:
            dependency = pending.pop()
            if dependency in found:
                continue
            found.add(dependency)
            pending.extend(direct[dependency])
        result[name] = found
    return result


def validate_inputs(profile, inventory):
    closure.validate_profile(profile)
    if inventory.get("format") != closure.INVENTORY_FORMAT:
        raise ProviderBuildError("unsupported closure inventory format")
    if inventory.get("profile") != profile.get("name"):
        raise ProviderBuildError("closure inventory names another profile")
    if inventory.get("linux", {}).get("config_sha256") != profile.get(
        "config_sha256"
    ):
        raise ProviderBuildError("closure inventory config differs from profile")

    link = profile.get("provider_link")
    if not isinstance(link, dict):
        raise ProviderBuildError("profile has no provider_link declaration")
    required_link_fields = (
        "allowed_host_imports",
        "linker_aliases",
        "linker_symbols",
    )
    for field in required_link_fields:
        if field not in link:
            raise ProviderBuildError(f"provider_link is missing {field}")

    declared = {item["name"] for item in profile["shared_providers"]}
    unknown_linker_owners = set(link["linker_symbols"].values()) - declared
    if unknown_linker_owners:
        raise ProviderBuildError(
            "linker symbol names an unknown provider: "
            f"{sorted(unknown_linker_owners)[0]}"
        )
    inventoried = {item["name"] for item in inventory["shared_providers"]}
    if declared != inventoried:
        raise ProviderBuildError("profile and inventory providers differ")
    provider_order(profile)


def provider_include_flags(source_tree, provider_build_dir):
    paths = (
        source_tree / "kobox/provider/include",
        source_tree / "arch/x86/include",
        provider_build_dir / "arch/x86/include/generated",
        source_tree / "include",
        provider_build_dir / "include",
        source_tree / "arch/x86/include/uapi",
        provider_build_dir / "arch/x86/include/generated/uapi",
        source_tree / "include/uapi",
        provider_build_dir / "include/generated/uapi",
    )
    flags = [f"-I{path}" for path in paths]
    flags.extend((
        f"-include {source_tree / 'include/linux/compiler-version.h'}",
        f"-include {source_tree / 'include/linux/kconfig.h'}",
    ))
    return " ".join(flags)


def make_arguments(arguments, targets, include_overlay=False):
    result = [
        arguments.make,
        "-C",
        arguments.source_tree,
        f"O={arguments.provider_build_dir}",
        "ARCH=x86_64",
        f"LLVM={arguments.llvm}",
        f"CC={arguments.cc}",
        f"LD={arguments.ld}",
    ]
    if include_overlay:
        result.extend((
            "LINUXINCLUDE=" + provider_include_flags(
                arguments.source_tree, arguments.provider_build_dir
            ),
            f"KCFLAGS={PIC_FLAGS}",
        ))
    result.extend((f"-j{arguments.jobs}", *targets))
    return result


def prepare_provider_build(arguments, canonical_config):
    build_dir = arguments.provider_build_dir
    config = build_dir / ".config"
    build_dir.mkdir(parents=True, exist_ok=True)
    if config.exists():
        if config.read_bytes() != canonical_config.read_bytes():
            raise ProviderBuildError(
                f"provider build config differs from {canonical_config}"
            )
        return
    if not arguments.prepare:
        raise ProviderBuildError(
            f"provider build is not prepared: {config}; pass --prepare"
        )
    shutil.copyfile(canonical_config, config)
    run_command(make_arguments(
        arguments, ("olddefconfig", "prepare", "modules_prepare")
    ))
    if config.read_bytes() != canonical_config.read_bytes():
        raise ProviderBuildError("preparation changed the pinned kernel config")


def parse_archive_definitions(build_dir, nm):
    output = run_command([
        nm,
        "-A",
        "--extern-only",
        "--defined-only",
        "--format=posix",
        build_dir / "vmlinux.a",
    ])
    return closure.parse_vmlinux_archive_symbols(output, build_dir)


def parse_linked_symbols(build_dir, nm):
    output = run_command([
        nm,
        "--extern-only",
        "--defined-only",
        "--format=posix",
        build_dir / "vmlinux",
    ])
    return closure.parse_defined_symbols(output)


def dynamic_undefined(path, nm):
    output = run_command([
        nm, "-D", "--undefined-only", "--format=posix", path
    ])
    symbols = closure.parse_undefined_symbols(output)
    for symbol in symbols:
        symbol["name"] = symbol["name"].split("@", 1)[0]
    return symbols


def dynamic_defined(path, nm):
    output = run_command([
        nm, "-D", "--defined-only", "--format=posix", path
    ])
    return {
        symbol.split("@", 1)[0]
        for symbol in closure.parse_defined_symbols(output)
    }


def non_pic_relocation_symbols(error):
    return sorted(set(re.findall(
        r"relocation R_X86_64_[A-Z0-9_]+ cannot be used against symbol "
        r"'([^']+)'",
        str(error),
    )))


def defined_symbol_records(path, nm):
    output = run_command([
        nm,
        "--extern-only",
        "--defined-only",
        "--print-size",
        "--format=posix",
        path,
    ])
    records = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 4:
            continue
        name, symbol_type, _, size = fields[:4]
        records[name] = {
            "type": symbol_type,
            "size": int(size, 16),
        }
    return records


def symbol_category(symbol_type):
    if symbol_type.upper() in ("T", "W"):
        return "function"
    return "data"


def validate_support_abi(
    profile,
    definitions,
    canonical_build_dir,
    support_object_by_source,
    nm,
):
    support_records = {
        source: defined_symbol_records(path, nm)
        for source, path in support_object_by_source.items()
    }
    canonical_records = {}
    for definition in profile["shared_providers"]:
        for symbol, support_source in definition.get("support_symbols", {}).items():
            selected = definition_for_symbol(symbol, definitions, set())
            source_object = selected["source_object"]
            if source_object == "vmlinux-linker-defined":
                raise ProviderBuildError(
                    f"support symbol replaces a linker symbol: {symbol}"
                )
            if source_object not in canonical_records:
                canonical_records[source_object] = defined_symbol_records(
                    canonical_build_dir / source_object, nm
                )
            expected = canonical_records[source_object].get(symbol)
            actual = support_records[support_source].get(symbol)
            if expected is None or actual is None:
                raise ProviderBuildError(
                    f"support symbol definition is missing: {symbol}"
                )
            expected_category = symbol_category(expected["type"])
            actual_category = symbol_category(actual["type"])
            if expected_category != actual_category:
                raise ProviderBuildError(
                    f"support symbol kind differs: {symbol}: "
                    f"expected {expected_category}, found {actual_category}"
                )
            if expected_category == "data" and expected["size"] != actual["size"]:
                raise ProviderBuildError(
                    f"support symbol size differs: {symbol}: "
                    f"expected {expected['size']}, found {actual['size']}"
                )


def compile_linux_objects(arguments, source_objects):
    targets = sorted(
        item for item in source_objects if item != "vmlinux-linker-defined"
    )
    missing_sources = [
        item for item in targets
        if not (arguments.source_tree / item).with_suffix(".c").exists()
        and not (arguments.source_tree / item).with_suffix(".S").exists()
        and not (arguments.source_tree / item).with_suffix(".s").exists()
    ]
    # Composite Kbuild objects do not necessarily have a same-named source.
    missing_sources = [
        item for item in missing_sources
        if not (arguments.canonical_build_dir / item).is_file()
    ]
    if missing_sources:
        raise ProviderBuildError(
            f"source object is unavailable: {missing_sources[0]}"
        )
    if targets:
        run_command(make_arguments(
            arguments, tuple(targets), include_overlay=True
        ))
    missing_outputs = [
        item for item in targets
        if not (arguments.provider_build_dir / item).is_file()
    ]
    if missing_outputs:
        raise ProviderBuildError(
            f"Kbuild did not produce {missing_outputs[0]}"
        )


def compile_runtime_source(arguments, output_dir, source_name):
    source = arguments.source_tree / source_name
    if not source.is_file():
        raise ProviderBuildError(
            f"provider runtime source is missing: {source_name}"
        )
    encoded_name = source_name.replace("/", "-")
    output = output_dir / f".metadata/{encoded_name}.o"
    output.parent.mkdir(parents=True, exist_ok=True)
    run_command([
        arguments.cc,
        "-std=gnu11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fPIC",
        "-mcmodel=large",
        "-ffunction-sections",
        "-fdata-sections",
        "-fno-stack-protector",
        "-fmacro-prefix-map=" + str(arguments.source_tree) + "=linux",
        "-I",
        arguments.protocol_include,
        "-c",
        source,
        "-o",
        output,
    ])
    return output


def compile_support_source(arguments, output_dir, source_name):
    source = arguments.source_tree / source_name
    if not source.is_file():
        raise ProviderBuildError(f"provider support source is missing: {source_name}")
    encoded_name = source_name.replace("/", "-")
    output = output_dir / f".metadata/{encoded_name}.o"
    command = [
        arguments.cc,
        "-fPIC",
        "-mcmodel=large",
        "-ffunction-sections",
        "-fdata-sections",
        "-fno-stack-protector",
        "-fmacro-prefix-map=" + str(arguments.source_tree) + "=linux",
    ]
    if source.suffix == ".S":
        command.extend((
            "-I" + str(arguments.provider_build_dir / "include/generated"),
            "-x",
            "assembler-with-cpp",
        ))
    else:
        command.extend((
            "-nostdinc",
            *provider_include_flags(
                arguments.source_tree, arguments.provider_build_dir
            ).split(),
            "-include",
            str(arguments.source_tree / "include/linux/compiler_types.h"),
            "-D__KERNEL__",
            "--target=x86_64-linux-gnu",
            "-std=gnu11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-address-of-packed-member",
            "-Wno-gnu-variable-sized-type-not-at-end",
            "-Wno-sign-compare",
            "-Wno-unused-parameter",
            "-fshort-wchar",
            "-funsigned-char",
            "-fno-common",
            "-mno-red-zone",
        ))
    command.extend(("-c", source, "-o", output))
    run_command(command)
    return output


def prepare_linux_link_objects(
    arguments,
    output_dir,
    provider_name,
    source_objects,
    overridden_symbols,
    link_symbols,
):
    paths = {}
    encoded_provider = provider_name.replace("/", "-")
    root = output_dir / f".metadata/objects/{encoded_provider}"

    for source_object in sorted(source_objects):
        if source_object == "vmlinux-linker-defined":
            continue
        source = arguments.provider_build_dir / source_object
        overrides = overridden_symbols.get(source_object, set())
        records = defined_symbol_records(source, arguments.nm)
        local_symbols = set(records) - link_symbols - overrides
        if not overrides and not local_symbols:
            paths[source_object] = source
            continue
        destination = root / source_object
        destination.parent.mkdir(parents=True, exist_ok=True)
        command = [arguments.objcopy]
        command.extend(
            f"--weaken-symbol={symbol}" for symbol in sorted(overrides)
        )
        command.extend(
            f"--localize-symbol={symbol}" for symbol in sorted(local_symbols)
        )
        command.extend((source, destination))
        run_command(command)
        paths[source_object] = destination
    return paths


def version_script(path, exports):
    invalid = [name for name in exports if not re.fullmatch(r"[A-Za-z0-9_]+", name)]
    if invalid:
        raise ProviderBuildError(f"invalid provider export name: {invalid[0]}")
    lines = ["KOBOX_PROVIDER_DEV {", "  global:"]
    lines.extend(f"    {name};" for name in sorted(exports))
    lines.extend(("  local:", "    *;", "};", ""))
    path.write_text("\n".join(lines), encoding="utf-8")


def response_argument(value):
    value = str(value)
    if "\n" in value or "\r" in value:
        raise ProviderBuildError("link path contains a newline")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def relocatable_gc_arguments(output, roots, objects):
    arguments = ["-r", "--gc-sections", "-o", output]
    for symbol in sorted(roots):
        arguments.extend(("-u", symbol))
    arguments.extend(objects)
    return arguments


def link_provider(
    arguments,
    definition,
    source_objects,
    exports,
    link_symbols,
    support_objects,
    provider_paths,
    linux_object_paths,
):
    relative = pathlib.Path(definition["name"])
    output = arguments.output_dir / relative
    metadata_name = definition["name"].replace("/", "-")
    metadata = arguments.output_dir / ".metadata"
    output.parent.mkdir(parents=True, exist_ok=True)
    metadata.mkdir(parents=True, exist_ok=True)
    map_path = metadata / f"{metadata_name}.map"
    prelink_path = metadata / f"{metadata_name}.linked.o"
    prelink_response_path = metadata / f"{metadata_name}.linked.rsp"
    response_path = metadata / f"{metadata_name}.rsp"
    version_script(map_path, exports)

    prelink_objects = [
        linux_object_paths[item]
        for item in sorted(source_objects)
        if item != "vmlinux-linker-defined"
    ]
    prelink_objects.extend(support_objects)
    prelink_arguments = relocatable_gc_arguments(
        prelink_path, link_symbols, prelink_objects
    )
    prelink_response_path.write_text(
        "\n".join(response_argument(item) for item in prelink_arguments) + "\n",
        encoding="utf-8",
    )
    run_command([arguments.ld, f"@{prelink_response_path}"])

    link_arguments = [
        "-shared",
        "-Bsymbolic",
        "--gc-sections",
        "--build-id=none",
        "--hash-style=gnu",
        "-z",
        "relro",
        "-z",
        "now",
        "--no-undefined-version",
        f"--soname={relative.name}",
        f"--version-script={map_path}",
        "-o",
        output,
    ]
    if definition.get("linker_script"):
        link_arguments.insert(
            link_arguments.index("--no-undefined-version"),
            f"--script={arguments.source_tree / definition['linker_script']}",
        )
    for alias, target in sorted(
        arguments.profile["provider_link"]["linker_aliases"].items()
    ):
        if alias in exports:
            link_arguments.append(f"--defsym={alias}={target}")
    link_arguments.append(prelink_path)
    link_arguments.extend(
        provider_paths[item]
        for item in definition.get("dependencies", [])
    )
    response_path.write_text(
        "\n".join(response_argument(item) for item in link_arguments) + "\n",
        encoding="utf-8",
    )
    run_command([arguments.ld, f"@{response_path}"])
    return output


def definition_for_symbol(symbol, definitions, linked_symbols):
    try:
        return closure.select_vmlinux_definition(
            symbol, definitions, linked_symbols
        )
    except closure.ClosureError as error:
        raise ProviderBuildError(str(error)) from error


def resolve_import(
    symbol,
    weak,
    consumer,
    profile,
    definitions,
    linked_symbols,
    dependency_closure,
    runtime_symbols,
    support_symbols,
):
    link = profile["provider_link"]
    if symbol in link["allowed_host_imports"]:
        return {"name": symbol, "provider": "host", "optional": weak}

    support = support_symbols.get(symbol)
    runtime = runtime_symbols.get(symbol)
    if support is not None:
        owner, source_object = support
    elif runtime is not None:
        owner, source_object = runtime
    else:
        candidates = definitions.get(symbol, [])
        if not candidates and symbol not in linked_symbols:
            if weak:
                return {"name": symbol, "provider": None, "optional": True}
            raise ProviderBuildError(
                f"strong provider import has no Linux definition: "
                f"{consumer}: {symbol}"
            )
        definition = definition_for_symbol(symbol, definitions, linked_symbols)
        source_object = definition["source_object"]
        if source_object == "vmlinux-linker-defined":
            owner = link["linker_symbols"].get(symbol)
            if owner is None:
                raise ProviderBuildError(
                    f"linker-defined symbol is not modeled: {consumer}: {symbol}"
                )
        else:
            owner = closure.classify_provider(
                source_object, profile["shared_providers"]
            )

    if owner != consumer and owner not in dependency_closure[consumer]:
        raise ProviderBuildError(
            f"provider dependency points upward: {consumer}: {symbol}: {owner}: "
            f"{source_object}"
        )
    return {
        "name": symbol,
        "provider": owner,
        "optional": weak,
        "source_object": source_object,
    }


def elf_type(path, readelf):
    header = run_command([readelf, "-h", path])
    match = re.search(r"^\s*Type:\s+(\S+)", header, re.MULTILINE)
    if not match:
        raise ProviderBuildError(f"cannot read ELF type: {path}")
    return match.group(1)


def dynamic_tags(path, readelf, tag):
    dynamic = run_command([readelf, "-d", path])
    pattern = re.compile(rf"\({re.escape(tag)}\).*\[(.*?)\]")
    return pattern.findall(dynamic)


def encode_provider_inventory(value):
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def verbose(arguments, message):
    if arguments.verbose:
        print(message, file=sys.stderr)


def build(arguments):
    profile = arguments.profile
    inventory = arguments.inventory
    validate_inputs(profile, inventory)

    protocol_header = arguments.protocol_include / "kobox2/memory_arena_layout.h"
    if not protocol_header.is_file():
        raise ProviderBuildError(
            f"generated protocol include is missing: {protocol_header}"
        )

    canonical_config = arguments.canonical_build_dir / ".config"
    for required in (
        canonical_config,
        arguments.canonical_build_dir / "vmlinux.a",
        arguments.canonical_build_dir / "vmlinux",
    ):
        if not required.is_file():
            raise ProviderBuildError(f"canonical build input is missing: {required}")
    if sha256_file(canonical_config) != profile["config_sha256"]:
        raise ProviderBuildError("canonical build config digest differs from profile")
    prepare_provider_build(arguments, canonical_config)

    compiler_version = tool_version(arguments.cc)
    linker_version = tool_version(arguments.ld)
    expected = profile["toolchain"]
    if expected["compiler_version"] not in compiler_version:
        raise ProviderBuildError(
            f"compiler version differs from profile: {compiler_version}"
        )
    if expected["linker_version"] not in linker_version:
        raise ProviderBuildError(
            f"linker version differs from profile: {linker_version}"
        )

    definitions = parse_archive_definitions(
        arguments.canonical_build_dir, arguments.nm
    )
    linked_symbols = parse_linked_symbols(
        arguments.canonical_build_dir, arguments.nm
    )
    definitions_by_name = {
        item["name"]: item for item in profile["shared_providers"]
    }
    inventory_by_name = {
        item["name"]: item for item in inventory["shared_providers"]
    }
    order = provider_order(profile)
    dependency_closure = transitive_dependencies(profile)
    support_symbols = {
        symbol: (definition["name"], source)
        for definition in profile["shared_providers"]
        for symbol, source in definition.get("support_symbols", {}).items()
    }
    runtime_symbols = collect_runtime_symbols(profile)
    overlap = set(runtime_symbols) & set(support_symbols)
    if overlap:
        raise ProviderBuildError(
            f"provider runtime and support symbol overlap: {sorted(overlap)[0]}"
        )
    source_objects = {
        name: {
            item["source_object"]
            for item in inventory_by_name[name]["required_exports"]
            if item["name"] not in support_symbols
            and item["name"] not in runtime_symbols
        }
        for name in order
    }
    exports = {
        name: {
            item["name"] for item in inventory_by_name[name]["required_exports"]
        } | {
            symbol
            for source in definitions_by_name[name].get("runtime_sources", [])
            for symbol in source.get("exports", [])
        }
        for name in order
    }
    link_symbols = {
        name: set(exports[name])
        for name in order
    }
    for alias, target in profile["provider_link"]["linker_aliases"].items():
        if not any(alias in exports[name] for name in order):
            continue
        definition = definition_for_symbol(target, definitions, linked_symbols)
        owner = closure.classify_provider(
            definition["source_object"], profile["shared_providers"]
        )
        link_symbols[owner].add(target)
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    runtime_objects = {
        name: [
            compile_runtime_source(
                arguments, arguments.output_dir, source["path"]
            )
            for source in definitions_by_name[name].get("runtime_sources", [])
        ]
        for name in order
    }
    linux_support_objects = {
        name: [
            compile_support_source(arguments, arguments.output_dir, source)
            for source in definitions_by_name[name].get("support_sources", [])
        ]
        for name in order
    }
    support_objects = {
        name: runtime_objects[name] + linux_support_objects[name]
        for name in order
    }
    support_object_by_source = {}
    for name in order:
        support_object_by_source.update(zip(
            (source["path"] for source in
             definitions_by_name[name].get("runtime_sources", [])),
            runtime_objects[name],
        ))
        support_object_by_source.update(zip(
            definitions_by_name[name].get("support_sources", []),
            linux_support_objects[name],
        ))
    validate_support_abi(
        profile,
        definitions,
        arguments.canonical_build_dir,
        support_object_by_source,
        arguments.nm,
    )
    overridden_symbols = {}
    for symbol in support_symbols:
        definition = definition_for_symbol(symbol, definitions, linked_symbols)
        overridden_symbols.setdefault(definition["source_object"], set()).add(
            symbol
        )
    provider_paths = {}
    final_imports = {}
    for iteration in range(1, arguments.max_iterations + 1):
        print(f"provider closure iteration {iteration}", file=sys.stderr)
        compile_linux_objects(
            arguments, set().union(*source_objects.values())
        )
        linux_object_paths = {
            name: prepare_linux_link_objects(
                arguments,
                arguments.output_dir,
                name,
                source_objects[name],
                overridden_symbols,
                link_symbols[name],
            )
            for name in order
        }
        changed = False
        restart = False
        round_imports = {}
        for name in order:
            try:
                path = link_provider(
                    arguments,
                    definitions_by_name[name],
                    source_objects[name],
                    exports[name],
                    link_symbols[name],
                    support_objects[name],
                    provider_paths,
                    linux_object_paths[name],
                )
            except ProviderBuildError as error:
                recovered = False
                for symbol in non_pic_relocation_symbols(error):
                    resolved = resolve_import(
                        symbol,
                        False,
                        name,
                        profile,
                        definitions,
                        linked_symbols,
                        dependency_closure,
                        runtime_symbols,
                        support_symbols,
                    )
                    owner = resolved["provider"]
                    if owner in (None, "host"):
                        continue
                    if symbol not in link_symbols[owner]:
                        link_symbols[owner].add(symbol)
                        verbose(
                            arguments,
                            f"  bind symbol {owner}: {symbol}",
                        )
                        recovered = True
                    source_object = resolved["source_object"]
                    if (source_object != "vmlinux-linker-defined" and
                        not source_object.startswith("kobox/provider/") and
                        source_object not in source_objects[owner]):
                        source_objects[owner].add(source_object)
                        verbose(
                            arguments,
                            f"  object {owner}: {source_object} "
                            f"for {symbol}",
                        )
                        recovered = True
                    if owner != name and symbol not in exports[owner]:
                        exports[owner].add(symbol)
                        recovered = True
                if not recovered:
                    raise
                changed = True
                restart = True
                break
            provider_paths[name] = path
            imports = []
            for undefined in dynamic_undefined(path, arguments.nm):
                resolved = resolve_import(
                    undefined["name"],
                    undefined["weak"],
                    name,
                    profile,
                    definitions,
                    linked_symbols,
                    dependency_closure,
                    runtime_symbols,
                    support_symbols,
                )
                imports.append(resolved)
                owner = resolved["provider"]
                if owner in (None, "host"):
                    continue
                if resolved["name"] not in link_symbols[owner]:
                    link_symbols[owner].add(resolved["name"])
                    verbose(
                        arguments,
                        f"  link symbol {owner}: {resolved['name']}",
                    )
                    changed = True
                source_object = resolved["source_object"]
                if source_object.startswith("kobox/provider/"):
                    if resolved["name"] not in exports[owner]:
                        exports[owner].add(resolved["name"])
                        verbose(
                            arguments,
                            f"  export {owner}: {resolved['name']}",
                        )
                        changed = True
                elif source_object not in source_objects[owner]:
                    if not source_object.startswith("kobox/provider/"):
                        source_objects[owner].add(source_object)
                        verbose(
                            arguments,
                            f"  object {owner}: {source_object} "
                            f"for {resolved['name']}",
                        )
                        changed = True
                if owner != name and resolved["name"] not in exports[owner]:
                    exports[owner].add(resolved["name"])
                    verbose(
                        arguments,
                        f"  export {owner}: {resolved['name']} for {name}",
                    )
                    changed = True
            round_imports[name] = sorted(imports, key=lambda item: item["name"])
        if restart:
            continue
        if not changed:
            final_imports = round_imports
            break
    else:
        raise ProviderBuildError(
            f"provider closure did not converge after {arguments.max_iterations} iterations"
        )

    providers = []
    for name in order:
        path = provider_paths[name]
        if elf_type(path, arguments.readelf) != "DYN":
            raise ProviderBuildError(f"provider is not ET_DYN: {name}")
        if dynamic_tags(path, arguments.readelf, "TEXTREL"):
            raise ProviderBuildError(f"provider has text relocations: {name}")
        sonames = dynamic_tags(path, arguments.readelf, "SONAME")
        if sonames != [path.name]:
            raise ProviderBuildError(f"provider has wrong SONAME: {name}: {sonames}")
        needed = set(dynamic_tags(path, arguments.readelf, "NEEDED"))
        expected_needed = {
            pathlib.Path(item).name
            for item in definitions_by_name[name].get("dependencies", [])
        }
        if needed != expected_needed:
            raise ProviderBuildError(
                f"provider dependencies differ: {name}: "
                f"expected {sorted(expected_needed)}, found {sorted(needed)}"
            )
        actual_exports = dynamic_defined(path, arguments.nm)
        missing = exports[name] - actual_exports
        extra = actual_exports - exports[name]
        if missing or extra:
            raise ProviderBuildError(
                f"provider dynamic exports differ: {name}: "
                f"missing {sorted(missing)}, extra {sorted(extra)}"
            )
        providers.append({
            "name": name,
            "soname": path.name,
            "dependencies": sorted(
                definitions_by_name[name].get("dependencies", [])
            ),
            "source_objects": sorted(source_objects[name]),
            "support_objects": sorted(
                source["path"]
                for source in definitions_by_name[name].get(
                    "runtime_sources", []
                )
            ) + sorted(definitions_by_name[name].get("support_sources", [])),
            "exports": sorted(exports[name]),
            "imports": final_imports[name],
            "content_size": path.stat().st_size,
            "sha256": sha256_file(path),
        })

    result = {
        "format": PROVIDER_INVENTORY_FORMAT,
        "profile": profile["name"],
        "linux": inventory["linux"],
        "toolchain": {
            "compiler": compiler_version,
            "linker": linker_version,
        },
        "load_order": order,
        "cleanup_order": list(reversed(order)),
        "providers": providers,
        "summary": {
            "provider_count": len(providers),
            "source_object_count": sum(
                len(item["source_objects"]) for item in providers
            ),
            "export_count": sum(len(item["exports"]) for item in providers),
            "import_count": sum(len(item["imports"]) for item in providers),
        },
    }
    encoded = encode_provider_inventory(result)
    if arguments.check:
        if arguments.check.read_text(encoding="utf-8") != encoded:
            raise ProviderBuildError(
                f"provider inventory differs from {arguments.check}"
            )
    elif arguments.output_inventory:
        arguments.output_inventory.parent.mkdir(parents=True, exist_ok=True)
        arguments.output_inventory.write_text(encoded, encoding="utf-8")
    else:
        sys.stdout.write(encoded)


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-tree", type=pathlib.Path, required=True)
    parser.add_argument("--canonical-build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--provider-build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--inventory", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--protocol-include", type=pathlib.Path, required=True)
    parser.add_argument("--output-inventory", type=pathlib.Path)
    parser.add_argument("--check", type=pathlib.Path)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--max-iterations", type=int, default=64)
    parser.add_argument("--cc", default="clang-18")
    parser.add_argument("--ld", default="ld.lld")
    parser.add_argument("--llvm", default="-18")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--objcopy", default="objcopy")
    parser.add_argument("--make", default="make")
    arguments = parser.parse_args()
    if arguments.output_inventory and arguments.check:
        parser.error("--output-inventory and --check are mutually exclusive")
    if arguments.jobs < 1 or arguments.max_iterations < 1:
        parser.error("--jobs and --max-iterations must be positive")
    arguments.source_tree = arguments.source_tree.resolve()
    arguments.canonical_build_dir = arguments.canonical_build_dir.resolve()
    arguments.provider_build_dir = arguments.provider_build_dir.resolve()
    arguments.output_dir = arguments.output_dir.resolve()
    arguments.protocol_include = arguments.protocol_include.resolve()
    arguments.profile = read_json(arguments.profile.resolve())
    arguments.inventory = read_json(arguments.inventory.resolve())
    if arguments.output_inventory:
        arguments.output_inventory = arguments.output_inventory.resolve()
    if arguments.check:
        arguments.check = arguments.check.resolve()
    return arguments


def main():
    try:
        build(parse_arguments())
    except (ProviderBuildError, closure.ClosureError, OSError) as error:
        print(f"shared provider build: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
