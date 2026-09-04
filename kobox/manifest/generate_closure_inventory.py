#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Generate a deterministic, modules-only Linux driver closure inventory."""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from collections import defaultdict, deque


INVENTORY_FORMAT = "kobox-linux-driver-closure-inventory-dev"
PROFILE_FORMAT = "kobox-linux-runtime-profile-dev"


class ClosureError(Exception):
    """A build or profile cannot form a complete driver closure."""


def run_command(arguments):
    result = subprocess.run(
        arguments,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ClosureError(f"command failed: {' '.join(arguments)}: {detail}")
    return result.stdout


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_config(text):
    values = {}
    value_pattern = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
    disabled_pattern = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")
    for line in text.splitlines():
        match = value_pattern.match(line)
        if match:
            values[match.group(1)] = match.group(2)
            continue
        match = disabled_pattern.match(line)
        if match:
            values[match.group(1)] = "n"
    return values


def parse_baseline(text):
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ClosureError(f"malformed baseline line: {line}")
        name, value = line.split("=", 1)
        values[name] = value
    return values


def parse_symvers(text):
    symbols = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        fields = line.split("\t")
        if len(fields) < 4:
            raise ClosureError(f"malformed Module.symvers line {line_number}")
        while len(fields) < 5:
            fields.append("")
        _, symbol, owner, export_kind, namespace = fields[:5]
        if symbol in symbols:
            raise ClosureError(f"duplicate exported symbol: {symbol}")
        symbols[symbol] = {
            "owner": owner,
            "export": export_kind,
            "namespace": namespace,
        }
    return symbols


def parse_undefined_symbols(text):
    symbols = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[1] not in ("U", "w", "v"):
            continue
        symbols.append({
            "name": fields[0],
            "weak": fields[1] in ("w", "v"),
        })
    return sorted(symbols, key=lambda item: item["name"])


def parse_defined_symbols(text):
    symbols = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] not in ("U", "w", "v"):
            symbols.add(fields[0])
    return symbols


def parse_vmlinux_archive_symbols(text, build_dir):
    pattern = re.compile(r"^.*\[(.*)\]: (\S+) ([A-Za-z?])(?: |$)")
    definitions = defaultdict(list)
    build_dir = build_dir.resolve()
    for line in text.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        member = pathlib.Path(match.group(1))
        if not member.is_absolute():
            member = build_dir / member
        try:
            member = member.resolve().relative_to(build_dir)
        except ValueError as error:
            raise ClosureError(
                f"vmlinux archive member escapes build tree: {member}"
            ) from error
        definitions[match.group(2)].append({
            "source_object": member.as_posix(),
            "symbol_type": match.group(3),
        })
    return definitions


def select_vmlinux_definition(symbol, definitions, linked_symbols):
    candidates = definitions.get(symbol, [])
    strong = [
        item for item in candidates
        if item["symbol_type"].isupper() and
        item["symbol_type"] not in ("W", "V")
    ]
    if len(strong) == 1:
        return strong[0]
    if not strong and len(candidates) == 1:
        return candidates[0]
    if not candidates and symbol in linked_symbols:
        return {"source_object": "vmlinux-linker-defined", "symbol_type": "A"}
    if not candidates:
        raise ClosureError(f"no source object defines vmlinux export: {symbol}")
    paths = ", ".join(sorted(item["source_object"] for item in candidates))
    raise ClosureError(f"ambiguous vmlinux definition for {symbol}: {paths}")


def classify_provider(source_object, shared_providers):
    matches = []
    default = None
    for provider in shared_providers:
        if provider.get("default"):
            if default is not None:
                raise ClosureError("profile has more than one default provider")
            default = provider["name"]
        for prefix in provider.get("source_prefixes", []):
            if source_object.startswith(prefix):
                matches.append((len(prefix), provider["name"]))
    if matches:
        matches.sort(reverse=True)
        if len(matches) > 1 and matches[0][0] == matches[1][0]:
            raise ClosureError(f"ambiguous shared provider for {source_object}")
        return matches[0][1]
    if default is None:
        raise ClosureError(f"no shared provider owns {source_object}")
    return default


def resolve_export(module_path, undefined, symvers):
    exported = symvers.get(undefined["name"])
    if exported is not None or undefined["weak"]:
        return exported
    raise ClosureError(
        f"strong symbol has no export: {module_path}: {undefined['name']}"
    )


def validate_import_namespace(module_path, symbol, namespace,
                              imported_namespaces):
    if namespace and namespace not in imported_namespaces:
        raise ClosureError(
            f"missing imported namespace {namespace}: {module_path}: {symbol}"
        )


def parse_softdeps(lines):
    before = []
    after = []
    destination = None
    for line in lines:
        for token in line.split():
            if token == "pre:":
                destination = before
            elif token == "post:":
                destination = after
            elif destination is None:
                raise ClosureError(f"malformed soft dependency: {line}")
            else:
                destination.append(token)
    return sorted(set(before)), sorted(set(after))


def normalize_module_name(name):
    return name.replace("-", "_")


def resolve_module_dependency(name, module_names):
    provider = module_names.get(name)
    if provider is None:
        raise ClosureError(f"module dependency is unavailable: {name}")
    return provider


def module_path_from_order(path):
    if not path.endswith(".o"):
        raise ClosureError(f"modules.order entry is not an object: {path}")
    return f"{path[:-2]}.ko"


def topological_order(nodes, dependency_edges, ordering_edges,
                      preferred_nodes=None):
    preferred_nodes = preferred_nodes or set()

    def sort_key(node):
        return (0 if node in preferred_nodes else 1, node)

    successors = {node: set() for node in nodes}
    indegree = {node: 0 for node in nodes}
    for consumer, provider in dependency_edges:
        if consumer not in nodes or provider not in nodes:
            raise ClosureError("dependency names an unknown module")
        if consumer not in successors[provider]:
            successors[provider].add(consumer)
            indegree[consumer] += 1
    for before, after in ordering_edges:
        if before not in nodes or after not in nodes:
            raise ClosureError("ordering constraint names an unknown module")
        if after not in successors[before]:
            successors[before].add(after)
            indegree[after] += 1

    ready = sorted(
        (node for node, count in indegree.items() if count == 0),
        key=sort_key,
    )
    order = []
    while ready:
        node = ready.pop(0)
        order.append(node)
        for successor in sorted(successors[node]):
            indegree[successor] -= 1
            if indegree[successor] == 0:
                ready.append(successor)
                ready.sort(key=sort_key)
    if len(order) != len(nodes):
        blocked = sorted(node for node, count in indegree.items() if count)
        raise ClosureError(f"closure dependency cycle: {', '.join(blocked)}")
    return order


def validate_profile(profile):
    if profile.get("format") != PROFILE_FORMAT:
        raise ClosureError("unsupported closure profile format")
    required = (
        "name", "linux_tag", "linux_commit", "kernel_release",
        "config_sha256", "toolchain", "kernel_runtime", "roots",
        "explicit_dependencies", "resource_slots", "required_config",
    )
    for field in required:
        if field not in profile:
            raise ClosureError(f"profile is missing {field}")
    runtime = profile["kernel_runtime"]
    expected_runtime = {
        "name": "linux-runtime",
        "artifact": "vmlinux",
        "root_symbol": "start_kernel",
        "root_source": "init/main.o",
        "linker_script": "arch/x86/kernel/vmlinux.lds.S",
        "driver_closure": "modules-only",
        "process_lifecycle": "one-boot",
        "shutdown": "quiesce-modules-then-exit",
    }
    for field, expected in expected_runtime.items():
        if runtime.get(field) != expected:
            raise ClosureError(f"invalid kernel runtime {field}")
    if runtime.get("upper_api_overrides") != []:
        raise ClosureError("Linux upper API overrides must be empty")
    if not profile["roots"] or any(
        not isinstance(root, str) or not root.endswith(".ko")
        for root in profile["roots"]
    ):
        raise ClosureError("profile roots must be Linux modules")

    expected_slots = (
        (1, "pci-function", "kobox2.pci-function", "device", 7, 1, True),
        (2, "dma-domain", "kobox2.dma-domain", "device", 5, 1, False),
        (3, "irq-endpoint", "kobox2.irq-endpoint", "notification", 1, 2048, False),
    )
    slots = profile["resource_slots"]
    if not isinstance(slots, list) or len(slots) != len(expected_slots):
        raise ClosureError("profile has an incomplete device resource slot set")
    for slot, expected in zip(slots, expected_slots):
        identity = (
            slot.get("slot_id"), slot.get("name"), slot.get("schema"),
            slot.get("resource_type"), slot.get("required_rights"),
            slot.get("maximum_count"), slot.get("reset_required"),
        )
        if identity != expected or slot.get("minimum_count") != 1:
            raise ClosureError("profile device resource slot identity mismatch")
        if slot.get("consumers") != [runtime["name"]]:
            raise ClosureError("device resources must terminate at Linux runtime")


def modinfo_values(module, field, modinfo):
    output = run_command([modinfo, "-F", field, str(module)])
    return [line.strip() for line in output.splitlines() if line.strip()]


def read_modules(build_dir, modules_order, nm, modinfo):
    records = {}
    names = {}
    for entry in modules_order.read_text(encoding="utf-8").splitlines():
        if not entry:
            continue
        relative_path = module_path_from_order(entry)
        module_path = build_dir / relative_path
        if not module_path.is_file():
            raise ClosureError(f"module artifact is missing: {relative_path}")
        module_names = modinfo_values(module_path, "name", modinfo)
        if len(module_names) != 1:
            raise ClosureError(f"module has no unique name: {relative_path}")
        module_name = normalize_module_name(module_names[0])
        if module_name in names:
            raise ClosureError(f"duplicate module name: {module_name}")
        dependencies = []
        for value in modinfo_values(module_path, "depends", modinfo):
            dependencies.extend(item for item in value.split(",") if item)
        soft_pre, soft_post = parse_softdeps(
            modinfo_values(module_path, "softdep", modinfo)
        )
        records[relative_path] = {
            "path": relative_path,
            "name": module_name,
            "content_size": module_path.stat().st_size,
            "sha256": sha256_file(module_path),
            "license": modinfo_values(module_path, "license", modinfo),
            "vermagic": modinfo_values(module_path, "vermagic", modinfo),
            "aliases": sorted(modinfo_values(module_path, "alias", modinfo)),
            "import_namespaces": sorted(
                modinfo_values(module_path, "import_ns", modinfo)
            ),
            "modinfo_dependencies": sorted(
                {normalize_module_name(item) for item in dependencies}
            ),
            "softdep_pre": soft_pre,
            "softdep_post": soft_post,
            "undefined": parse_undefined_symbols(run_command([
                nm, "--undefined-only", "--format=posix", str(module_path),
            ])),
        }
        names[module_name] = relative_path
    return records, names


def owner_module_path(owner, records):
    candidate = f"{owner}.ko"
    return candidate if candidate in records else None


def add_edge(edge_reasons, consumer, provider, reason, symbol=None):
    edge = edge_reasons[(consumer, provider)]
    edge["kinds"].add(reason)
    if symbol is not None:
        edge["symbols"].add(symbol)


def generate_inventory(source_tree, build_dir, profile, nm="nm", modinfo="modinfo",
                       core_build_dir=None):
    source_tree = source_tree.resolve()
    build_dir = build_dir.resolve()
    core_build_dir = (core_build_dir or build_dir).resolve()
    validate_profile(profile)

    paths = {
        "config": build_dir / ".config",
        "symvers": build_dir / "Module.symvers",
        "modules_order": build_dir / "modules.order",
        "release": build_dir / "include/config/kernel.release",
        "vmlinux": core_build_dir / profile["kernel_runtime"]["artifact"],
        "core_config": core_build_dir / ".config",
        "core_release": core_build_dir / "include/config/kernel.release",
        "baseline": source_tree / "kobox/upstream-baseline.env",
    }
    for path in paths.values():
        if not path.is_file():
            raise ClosureError(f"required build input is missing: {path}")

    baseline = parse_baseline(paths["baseline"].read_text(encoding="utf-8"))
    if (baseline.get("LINUX_TAG") != profile["linux_tag"] or
            baseline.get("LINUX_COMMIT") != profile["linux_commit"]):
        raise ClosureError("profile does not match the pinned upstream baseline")
    kernel_release = paths["release"].read_text(encoding="utf-8").strip()
    if kernel_release != profile["kernel_release"]:
        raise ClosureError("kernel release mismatch")
    config_text = paths["config"].read_text(encoding="utf-8")
    config_sha256 = hashlib.sha256(config_text.encode()).hexdigest()
    if config_sha256 != profile["config_sha256"]:
        raise ClosureError("config digest mismatch")
    if sha256_file(paths["core_config"]) != config_sha256:
        raise ClosureError("module and core build config digests differ")
    if paths["core_release"].read_text(encoding="utf-8").strip() != kernel_release:
        raise ClosureError("module and core kernel releases differ")
    config = parse_config(config_text)
    for name, expected in sorted(profile["required_config"].items()):
        if config.get(name, "n") != expected:
            raise ClosureError(f"config mismatch for {name}")

    records, module_names = read_modules(
        build_dir, paths["modules_order"], nm, modinfo
    )
    symvers = parse_symvers(paths["symvers"].read_text(encoding="utf-8"))
    linked_symbols = parse_defined_symbols(run_command([
        nm, "--extern-only", "--defined-only", "--format=posix",
        str(paths["vmlinux"]),
    ]))

    roots = set(profile["roots"])
    for root in roots:
        if root not in records:
            raise ClosureError(f"root module is missing: {root}")
    explicit = profile["explicit_dependencies"]
    for dependency in explicit:
        if (dependency.get("consumer") not in records or
                dependency.get("provider") not in records):
            raise ClosureError("explicit dependency module is missing")
        if not dependency.get("kind") or not dependency.get("reason"):
            raise ClosureError("explicit dependency lacks kind or reason")

    selected = set(roots)
    pending = deque(sorted(roots))
    edge_reasons = defaultdict(lambda: {"kinds": set(), "symbols": set()})
    ordering_reasons = defaultdict(set)
    while pending:
        path = pending.popleft()
        record = records[path]
        dependencies = [
            (name, "modinfo") for name in record["modinfo_dependencies"]
        ] + [(name, "softdep-pre") for name in record["softdep_pre"]]
        for name, kind in dependencies:
            provider = resolve_module_dependency(name, module_names)
            add_edge(edge_reasons, path, provider, kind)
            if provider not in selected:
                selected.add(provider)
                pending.append(provider)
        for name in record["softdep_post"]:
            after = resolve_module_dependency(name, module_names)
            ordering_reasons[(path, after)].add("softdep-post")
            if after not in selected:
                selected.add(after)
                pending.append(after)
        for item in record["undefined"]:
            exported = resolve_export(path, item, symvers)
            if exported is None:
                continue
            provider = owner_module_path(exported["owner"], records)
            if provider is not None and provider != path:
                add_edge(edge_reasons, path, provider, "symbol", item["name"])
                if provider not in selected:
                    selected.add(provider)
                    pending.append(provider)
        for dependency in explicit:
            if dependency["consumer"] == path:
                provider = dependency["provider"]
                add_edge(edge_reasons, path, provider, dependency["kind"])
                if provider not in selected:
                    selected.add(provider)
                    pending.append(provider)

    runtime_requirements = defaultdict(set)
    module_requirements = defaultdict(lambda: defaultdict(set))
    module_output = []
    runtime_name = profile["kernel_runtime"]["name"]
    for path in sorted(selected):
        record = records[path]
        imports = []
        for item in record["undefined"]:
            exported = resolve_export(path, item, symvers)
            if exported is None:
                imports.append({
                    "name": item["name"], "optional": True, "provider": None,
                })
                continue
            validate_import_namespace(
                path, item["name"], exported["namespace"],
                record["import_namespaces"],
            )
            provider = owner_module_path(exported["owner"], records)
            if exported["owner"] == "vmlinux":
                if item["name"] not in linked_symbols:
                    raise ClosureError(
                        f"vmlinux export is absent from runtime: {item['name']}"
                    )
                provider = runtime_name
                runtime_requirements[item["name"]].add(path)
            elif provider is None:
                raise ClosureError(
                    f"export provider was not built as a module: "
                    f"{exported['owner']}: {item['name']}"
                )
            elif provider != path:
                module_requirements[provider][item["name"]].add(path)
            imports.append({
                "name": item["name"],
                "optional": item["weak"],
                "provider": provider,
                "export": exported["export"],
                "namespace": exported["namespace"],
            })
        module_output.append({
            key: value for key, value in record.items() if key != "undefined"
        } | {"root": path in roots, "imports": imports})

    for module in module_output:
        module["required_exports"] = [
            {"name": name, "consumers": sorted(consumers)}
            for name, consumers in sorted(
                module_requirements[module["path"]].items()
            )
        ]

    dependency_edges = set(edge_reasons)
    ordering_edges = set(ordering_reasons)
    load_order = topological_order(selected, dependency_edges, ordering_edges)
    if any(not artifact.endswith(".ko") for artifact in load_order):
        raise ClosureError("driver closure contains a non-module artifact")

    dependencies_output = [
        {
            "consumer": consumer,
            "provider": provider,
            "kinds": sorted(reason["kinds"]),
            "symbol_count": len(reason["symbols"]),
        }
        for (consumer, provider), reason in sorted(edge_reasons.items())
    ]
    ordering_output = [
        {"before": before, "after": after, "kinds": sorted(kinds)}
        for (before, after), kinds in sorted(ordering_reasons.items())
    ]
    runtime_exports = [
        {"name": name, "consumers": sorted(consumers)}
        for name, consumers in sorted(runtime_requirements.items())
    ]
    return {
        "format": INVENTORY_FORMAT,
        "profile": profile["name"],
        "linux": {
            "tag": profile["linux_tag"],
            "commit": profile["linux_commit"],
            "kernel_release": kernel_release,
            "config_sha256": config_sha256,
            "toolchain": profile["toolchain"],
        },
        "kernel_runtime": {
            "name": runtime_name,
            "artifact": profile["kernel_runtime"]["artifact"],
            "root_symbol": profile["kernel_runtime"]["root_symbol"],
            "required_exports": runtime_exports,
        },
        "roots": sorted(roots),
        "summary": {
            "module_count": len(module_output),
            "module_import_count": sum(
                len(module["imports"]) for module in module_output
            ),
            "required_module_export_count": sum(
                len(module["required_exports"]) for module in module_output
            ),
            "required_runtime_export_count": len(runtime_exports),
            "dependency_count": len(dependencies_output),
            "ordering_constraint_count": len(ordering_output),
            "resource_slot_count": len(profile["resource_slots"]),
        },
        "resource_slots": profile["resource_slots"],
        "modules": module_output,
        "dependencies": dependencies_output,
        "ordering_constraints": ordering_output,
        "load_order": load_order,
        "cleanup_order": list(reversed(load_order)),
    }


def encode_inventory(inventory):
    return json.dumps(
        inventory, ensure_ascii=False, indent=2, sort_keys=True,
    ) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-tree", type=pathlib.Path, required=True)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--core-build-dir", type=pathlib.Path)
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--check", type=pathlib.Path)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--modinfo", default="modinfo")
    arguments = parser.parse_args()

    if arguments.output and arguments.check:
        parser.error("--output and --check are mutually exclusive")
    try:
        profile = json.loads(arguments.profile.read_text(encoding="utf-8"))
        encoded = encode_inventory(generate_inventory(
            arguments.source_tree, arguments.build_dir, profile,
            arguments.nm, arguments.modinfo, arguments.core_build_dir,
        ))
        if arguments.check:
            if arguments.check.read_text(encoding="utf-8") != encoded:
                raise ClosureError(
                    f"generated inventory differs from {arguments.check}"
                )
        elif arguments.output:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(encoded, encoding="utf-8")
        else:
            sys.stdout.write(encoded)
    except (ClosureError, OSError, json.JSONDecodeError) as error:
        print(f"driver closure inventory: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
