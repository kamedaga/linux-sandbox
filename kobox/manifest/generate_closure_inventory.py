#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Generate a deterministic Linux module closure inventory."""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from collections import defaultdict, deque


INVENTORY_FORMAT = "kobox-linux-closure-inventory-dev"
PROFILE_FORMAT = "kobox-linux-closure-profile-dev"


class ClosureError(Exception):
    """A build or profile cannot form a complete closure."""


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
        if len(fields) < 2:
            continue
        symbol_type = fields[1]
        if symbol_type not in ("U", "w", "v"):
            continue
        symbols.append({
            "name": fields[0],
            "weak": symbol_type in ("w", "v"),
        })
    return sorted(symbols, key=lambda item: item["name"])


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


def parse_vmlinux_archive_symbols(text, build_dir):
    pattern = re.compile(r"^.*\[(.*)\]: ([^ ]+) ([A-Za-z?])(?: |$)")
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
            raise ClosureError(f"vmlinux archive member escapes build tree: {member}") from error
        definitions[match.group(2)].append({
            "source_object": member.as_posix(),
            "symbol_type": match.group(3),
        })
    return definitions


def parse_defined_symbols(text):
    symbols = set()

    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] not in ("U", "w", "v"):
            symbols.add(fields[0])
    return symbols


def select_vmlinux_definition(symbol, definitions, linked_symbols):
    candidates = definitions.get(symbol, [])
    strong = [item for item in candidates if item["symbol_type"].isupper() and
              item["symbol_type"] not in ("W", "V")]

    if len(strong) == 1:
        return strong[0]
    if not strong and len(candidates) == 1:
        return candidates[0]
    if not candidates:
        if symbol in linked_symbols:
            return {
                "source_object": "vmlinux-linker-defined",
                "symbol_type": "A",
            }
        raise ClosureError(f"no source object defines vmlinux export: {symbol}")
    paths = ", ".join(sorted(item["source_object"] for item in candidates))
    raise ClosureError(f"ambiguous vmlinux definition for {symbol}: {paths}")


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


def classify_provider(source_object, shared_providers):
    matches = []
    default = None

    for provider in shared_providers:
        if provider.get("default"):
            if default is not None:
                raise ClosureError("profile has more than one default shared provider")
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


def topological_order(nodes, dependency_edges, ordering_edges,
                      preferred_nodes=None):
    preferred_nodes = preferred_nodes or set()

    def sort_key(node):
        return (0 if node in preferred_nodes else 1, node)

    successors = {node: set() for node in nodes}
    indegree = {node: 0 for node in nodes}

    for consumer, provider in dependency_edges:
        if consumer not in nodes or provider not in nodes:
            raise ClosureError("dependency names an unknown artifact")
        if consumer not in successors[provider]:
            successors[provider].add(consumer)
            indegree[consumer] += 1
    for before, after in ordering_edges:
        if before not in nodes or after not in nodes:
            raise ClosureError("ordering constraint names an unknown artifact")
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
        "name",
        "linux_tag",
        "linux_commit",
        "kernel_release",
        "config_sha256",
        "toolchain",
        "roots",
        "explicit_dependencies",
        "shared_providers",
        "required_config",
    )
    for field in required:
        if field not in profile:
            raise ClosureError(f"profile is missing {field}")
    if not profile["roots"]:
        raise ClosureError("profile has no root module")
    provider_names = [item.get("name") for item in profile["shared_providers"]]
    if None in provider_names or len(provider_names) != len(set(provider_names)):
        raise ClosureError("shared provider names must be present and unique")
    if sum(bool(item.get("default")) for item in profile["shared_providers"]) != 1:
        raise ClosureError("profile must have exactly one default shared provider")
    for provider in profile["shared_providers"]:
        unknown = set(provider.get("dependencies", [])) - set(provider_names)
        if unknown:
            raise ClosureError(f"shared provider dependency is unknown: {sorted(unknown)[0]}")


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
        depends_values = modinfo_values(module_path, "depends", modinfo)
        dependencies = []
        for value in depends_values:
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
            "softdep_pre": sorted(
                {normalize_module_name(item) for item in soft_pre}
            ),
            "softdep_post": sorted(
                {normalize_module_name(item) for item in soft_post}
            ),
            "undefined": parse_undefined_symbols(run_command([
                nm,
                "--undefined-only",
                "--format=posix",
                str(module_path),
            ])),
        }
        names[module_name] = relative_path
    return records, names


def owner_module_path(owner, records):
    candidate = f"{owner}.ko"
    if candidate in records:
        return candidate
    return None


def add_edge(edge_reasons, consumer, provider, reason, symbol=None):
    edge = edge_reasons[(consumer, provider)]
    edge["kinds"].add(reason)
    if symbol is not None:
        edge["symbols"].add(symbol)


def generate_inventory(source_tree, build_dir, profile, nm="nm", modinfo="modinfo"):
    source_tree = source_tree.resolve()
    build_dir = build_dir.resolve()
    validate_profile(profile)

    config_path = build_dir / ".config"
    symvers_path = build_dir / "Module.symvers"
    modules_order_path = build_dir / "modules.order"
    archive_path = build_dir / "vmlinux.a"
    vmlinux_path = build_dir / "vmlinux"
    release_path = build_dir / "include/config/kernel.release"
    baseline_path = source_tree / "kobox/upstream-baseline.env"
    for required_path in (
        config_path,
        symvers_path,
        modules_order_path,
        archive_path,
        vmlinux_path,
        release_path,
        baseline_path,
    ):
        if not required_path.is_file():
            raise ClosureError(f"required build input is missing: {required_path}")

    baseline = parse_baseline(baseline_path.read_text(encoding="utf-8"))
    if baseline.get("LINUX_TAG") != profile["linux_tag"] or \
       baseline.get("LINUX_COMMIT") != profile["linux_commit"]:
        raise ClosureError("profile does not match the pinned upstream baseline")
    kernel_release = release_path.read_text(encoding="utf-8").strip()
    if kernel_release != profile["kernel_release"]:
        raise ClosureError(
            f"kernel release mismatch: expected {profile['kernel_release']}, "
            f"found {kernel_release}"
        )
    config_text = config_path.read_text(encoding="utf-8")
    config_sha256 = hashlib.sha256(config_text.encode()).hexdigest()
    if config_sha256 != profile["config_sha256"]:
        raise ClosureError(
            f"config digest mismatch: expected {profile['config_sha256']}, "
            f"found {config_sha256}"
        )
    config = parse_config(config_text)
    for name, expected in sorted(profile["required_config"].items()):
        actual = config.get(name, "n")
        if actual != expected:
            raise ClosureError(
                f"config mismatch for {name}: expected {expected}, found {actual}"
            )

    records, module_names = read_modules(
        build_dir, modules_order_path, nm, modinfo
    )
    symvers = parse_symvers(symvers_path.read_text(encoding="utf-8"))
    vmlinux_definitions = parse_vmlinux_archive_symbols(run_command([
        nm,
        "-A",
        "--extern-only",
        "--defined-only",
        "--format=posix",
        str(archive_path),
    ]), build_dir)
    linked_symbols = parse_defined_symbols(run_command([
        nm,
        "--extern-only",
        "--defined-only",
        "--format=posix",
        str(vmlinux_path),
    ]))

    roots = set(profile["roots"])
    for root in roots:
        if root not in records:
            raise ClosureError(f"root module is missing: {root}")
    explicit = profile["explicit_dependencies"]
    for dependency in explicit:
        if dependency.get("consumer") not in records:
            raise ClosureError("explicit dependency consumer is missing")
        if dependency.get("provider") not in records:
            raise ClosureError("explicit dependency provider is missing")
        if not dependency.get("kind") or not dependency.get("reason"):
            raise ClosureError("explicit dependency lacks kind or reason")

    selected = set(roots)
    pending = deque(sorted(roots))
    edge_reasons = defaultdict(lambda: {"kinds": set(), "symbols": set()})
    ordering_reasons = defaultdict(set)
    while pending:
        path = pending.popleft()
        record = records[path]

        dependency_names = [
            (name, "modinfo") for name in record["modinfo_dependencies"]
        ]
        dependency_names.extend(
            (name, "softdep-pre") for name in record["softdep_pre"]
        )
        for name, kind in dependency_names:
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
            if dependency["consumer"] != path:
                continue
            provider = dependency["provider"]
            add_edge(
                edge_reasons,
                path,
                provider,
                dependency["kind"],
            )
            if provider not in selected:
                selected.add(provider)
                pending.append(provider)

    provider_by_name = {
        item["name"]: item for item in profile["shared_providers"]
    }
    shared_requirements = defaultdict(dict)
    module_requirements = defaultdict(dict)
    module_output = []
    for path in sorted(selected):
        record = records[path]
        imports = []
        for item in record["undefined"]:
            exported = resolve_export(path, item, symvers)
            if exported is None:
                imports.append({
                    "name": item["name"],
                    "optional": True,
                    "provider": None,
                })
                continue
            namespace = exported["namespace"]
            validate_import_namespace(
                path,
                item["name"],
                namespace,
                record["import_namespaces"],
            )
            provider = owner_module_path(exported["owner"], records)
            source_object = None
            if exported["owner"] == "vmlinux":
                definition = select_vmlinux_definition(
                    item["name"], vmlinux_definitions, linked_symbols
                )
                source_object = definition["source_object"]
                provider = classify_provider(
                    source_object, profile["shared_providers"]
                )
                requirement = shared_requirements[provider].setdefault(
                    item["name"],
                    {
                        "name": item["name"],
                        "export": exported["export"],
                        "namespace": namespace,
                        "source_object": source_object,
                        "consumers": set(),
                    },
                )
                requirement["consumers"].add(path)
                add_edge(edge_reasons, path, provider, "symbol", item["name"])
            elif provider is None:
                raise ClosureError(
                    f"export provider was not built as a module: "
                    f"{exported['owner']}: {item['name']}"
                )
            else:
                requirement = module_requirements[provider].setdefault(
                    item["name"],
                    {
                        "name": item["name"],
                        "export": exported["export"],
                        "namespace": namespace,
                        "consumers": set(),
                    },
                )
                requirement["consumers"].add(path)
            imports.append({
                "name": item["name"],
                "optional": item["weak"],
                "provider": provider,
                "export": exported["export"],
                "namespace": namespace,
                **({"source_object": source_object} if source_object else {}),
            })

        module_output.append({
            key: value for key, value in record.items() if key != "undefined"
        } | {
            "root": path in roots,
            "imports": imports,
        })

    for module in module_output:
        requirements = []
        for requirement in module_requirements[module["path"]].values():
            requirement = dict(requirement)
            requirement["consumers"] = sorted(requirement["consumers"])
            requirements.append(requirement)
        module["required_exports"] = sorted(
            requirements, key=lambda item: item["name"]
        )

    used_providers = set(shared_requirements)
    pending_providers = deque(sorted(used_providers))
    while pending_providers:
        provider = pending_providers.popleft()
        if provider not in provider_by_name:
            raise ClosureError(f"unknown selected shared provider: {provider}")
        for dependency in provider_by_name[provider].get("dependencies", []):
            add_edge(edge_reasons, provider, dependency, "shared-provider")
            if dependency not in used_providers:
                used_providers.add(dependency)
                pending_providers.append(dependency)

    shared_output = []
    for name in sorted(used_providers):
        definition = provider_by_name[name]
        requirements = []
        source_objects = set()
        for requirement in shared_requirements[name].values():
            requirement = dict(requirement)
            requirement["consumers"] = sorted(requirement["consumers"])
            source_objects.add(requirement["source_object"])
            requirements.append(requirement)
        shared_output.append({
            "name": name,
            "dependencies": sorted(definition.get("dependencies", [])),
            "source_objects": sorted(source_objects),
            "required_exports": sorted(
                requirements, key=lambda item: item["name"]
            ),
        })

    nodes = selected | used_providers
    dependency_edges = set(edge_reasons)
    ordering_edges = set(ordering_reasons)
    load_order = topological_order(
        nodes,
        dependency_edges,
        ordering_edges,
        used_providers,
    )

    dependencies_output = []
    for (consumer, provider), reason in sorted(edge_reasons.items()):
        dependencies_output.append({
            "consumer": consumer,
            "provider": provider,
            "kinds": sorted(reason["kinds"]),
            "symbol_count": len(reason["symbols"]),
        })
    ordering_output = []
    for (before, after), kinds in sorted(ordering_reasons.items()):
        ordering_output.append({
            "before": before,
            "after": after,
            "kinds": sorted(kinds),
        })

    module_imports = sum(len(item["imports"]) for item in module_output)
    module_exports = sum(
        len(item["required_exports"]) for item in module_output
    )
    shared_exports = sum(
        len(item["required_exports"]) for item in shared_output
    )
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
        "roots": sorted(roots),
        "summary": {
            "module_count": len(module_output),
            "shared_provider_count": len(shared_output),
            "module_import_count": module_imports,
            "required_module_export_count": module_exports,
            "required_shared_export_count": shared_exports,
            "dependency_count": len(dependencies_output),
            "ordering_constraint_count": len(ordering_output),
        },
        "shared_providers": shared_output,
        "modules": module_output,
        "dependencies": dependencies_output,
        "ordering_constraints": ordering_output,
        "load_order": load_order,
        "cleanup_order": list(reversed(load_order)),
    }


def encode_inventory(inventory):
    return json.dumps(
        inventory,
        ensure_ascii=False,
        indent=2,
        sort_keys=True,
    ) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-tree", type=pathlib.Path, required=True)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
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
            arguments.source_tree,
            arguments.build_dir,
            profile,
            arguments.nm,
            arguments.modinfo,
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
        print(f"closure inventory: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
