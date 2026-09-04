#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Generate a C link-only plan for one built Linux closure."""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys


CLOSURE_FORMAT = "kobox-linux-closure-inventory-dev"
PROVIDER_FORMAT = "kobox-linux-provider-inventory-dev"


class LinkPlanError(Exception):
    """The built artifacts cannot form the declared link plan."""


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise LinkPlanError(f"cannot read {path}: {error}") from error


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_nm(nm, path, dynamic=False, undefined=False):
    command = [nm]
    if dynamic:
        command.append("-D")
    command.extend((
        "--undefined-only" if undefined else "--defined-only",
        "--extern-only",
        "--format=posix",
        str(path),
    ))
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise LinkPlanError(
            f"command failed: {' '.join(command)}: {detail}"
        )
    return result.stdout


def parse_defined(text):
    records = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        name = fields[0].split("@", 1)[0]
        symbol_type = fields[1]
        if not name or symbol_type.upper() == "U":
            continue
        if name in records:
            raise LinkPlanError(f"duplicate defined symbol: {name}")
        records[name] = symbol_kind(name, symbol_type)
    return records


def parse_undefined(text):
    records = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[1] not in ("U", "w", "v"):
            continue
        name = fields[0].split("@", 1)[0]
        if name in records:
            raise LinkPlanError(f"duplicate undefined symbol: {name}")
        records[name] = fields[1] in ("w", "v")
    return records


def symbol_kind(name, symbol_type):
    normalized = symbol_type.upper()
    if normalized in ("T", "W"):
        return "function"
    if normalized in ("B", "C", "D", "G", "R", "S", "V"):
        return "object"
    raise LinkPlanError(
        f"unsupported external symbol type: {name}: {symbol_type}"
    )


def artifact_record(path, nm, dynamic=False):
    if not path.is_file():
        raise LinkPlanError(f"artifact is missing: {path}")
    return {
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
        "defined": parse_defined(run_nm(nm, path, dynamic=dynamic)),
        "undefined": {} if dynamic else parse_undefined(
            run_nm(nm, path, undefined=True)
        ),
    }


def validate_identity(closure, providers):
    if closure.get("format") != CLOSURE_FORMAT:
        raise LinkPlanError("unsupported closure inventory format")
    if providers.get("format") != PROVIDER_FORMAT:
        raise LinkPlanError("unsupported provider inventory format")
    if closure.get("profile") != providers.get("profile"):
        raise LinkPlanError("closure and provider profiles differ")
    if closure.get("linux") != providers.get("linux"):
        raise LinkPlanError("closure and provider Linux identities differ")


def require_artifact(record, declared, name):
    if record["size"] != declared.get("content_size"):
        raise LinkPlanError(f"artifact size differs: {name}")
    if record["sha256"] != declared.get("sha256"):
        raise LinkPlanError(f"artifact digest differs: {name}")
    digest = record["sha256"]
    if len(digest) != 64 or any(
            character not in "0123456789abcdef" for character in digest):
        raise LinkPlanError(f"artifact digest is malformed: {name}")


def dependency_order(closure, providers, index_by_name):
    for edge in closure.get("dependencies", []):
        consumer = edge.get("consumer")
        provider = edge.get("provider")
        if consumer not in index_by_name or provider not in index_by_name:
            raise LinkPlanError("closure dependency names an unknown node")
        if index_by_name[provider] >= index_by_name[consumer]:
            raise LinkPlanError(
                f"closure dependency order is invalid: {consumer}: {provider}"
            )
    for item in providers.get("providers", []):
        consumer = item.get("name")
        for provider in item.get("dependencies", []):
            if provider not in index_by_name or consumer not in index_by_name:
                raise LinkPlanError("provider dependency names an unknown node")
            if index_by_name[provider] >= index_by_name[consumer]:
                raise LinkPlanError(
                    f"provider dependency order is invalid: "
                    f"{consumer}: {provider}"
                )


def make_plan(closure, providers, artifacts):
    validate_identity(closure, providers)
    order = closure.get("load_order")
    if not isinstance(order, list) or not order or len(order) != len(set(order)):
        raise LinkPlanError("closure load order is invalid")
    provider_records = providers.get("providers", [])
    module_records = closure.get("modules", [])
    provider_by_name = {item["name"]: item for item in provider_records}
    module_by_name = {item["path"]: item for item in module_records}
    if len(provider_by_name) != len(provider_records):
        raise LinkPlanError("duplicate provider artifact")
    if len(module_by_name) != len(module_records):
        raise LinkPlanError("duplicate module artifact")
    declared_names = set(provider_by_name) | set(module_by_name)
    if set(order) != declared_names:
        raise LinkPlanError("closure load order and artifacts differ")
    if providers.get("load_order") != [
        name for name in order if name in provider_by_name
    ]:
        raise LinkPlanError("provider and closure load orders differ")
    if set(artifacts) != declared_names:
        raise LinkPlanError("artifact records and closure differ")
    index_by_name = {name: index for index, name in enumerate(order)}
    dependency_order(closure, providers, index_by_name)
    required_provider_exports = {
        name: {
            imported["name"]
            for module in module_by_name.values()
            for imported in module.get("imports", [])
            if imported.get("provider") == name
        }
        for name in provider_by_name
    }

    nodes = []
    for name in order:
        record = artifacts[name]
        if name in provider_by_name:
            declared = provider_by_name[name]
            require_artifact(record, declared, name)
            declared_exports = declared.get("exports")
            if not isinstance(declared_exports, list) or \
                    set(declared_exports) != set(record["defined"]):
                raise LinkPlanError(f"provider export set differs: {name}")
            missing = required_provider_exports[name] - set(declared_exports)
            if missing:
                raise LinkPlanError(
                    f"provider link export is missing: "
                    f"{name}: {sorted(missing)[0]}"
                )
            link_exports = declared.get("link_exports")
            if (not isinstance(link_exports, list) or
                    len(link_exports) != len(set(link_exports)) or
                    set(link_exports) - set(declared_exports)):
                raise LinkPlanError(
                    f"provider link export set is invalid: {name}"
                )
            visible_exports = required_provider_exports[name] | set(link_exports)
            exports = [
                {"name": symbol, "kind": record["defined"][symbol]}
                for symbol in sorted(visible_exports)
            ]
            nodes.append({
                "name": name,
                "kind": "shared-provider",
                "size": record["size"],
                "sha256": record["sha256"],
                "exports": exports,
                "imports": [],
                "init_symbol": None,
                "cleanup_symbol": None,
            })
            continue

        declared = module_by_name[name]
        require_artifact(record, declared, name)
        inventory_imports = declared.get("imports", [])
        imports_by_name = {item["name"]: item for item in inventory_imports}
        if len(imports_by_name) != len(inventory_imports):
            raise LinkPlanError(f"duplicate module import: {name}")
        expected_undefined = {
            symbol: item.get("optional") is True
            for symbol, item in imports_by_name.items()
        }
        if record["undefined"] != expected_undefined:
            raise LinkPlanError(f"module import set differs: {name}")

        export_names = [
            item["name"] for item in declared.get("required_exports", [])
        ]
        if len(export_names) != len(set(export_names)):
            raise LinkPlanError(f"duplicate module export: {name}")
        has_init = "init_module" in record["defined"]
        has_cleanup = "cleanup_module" in record["defined"]
        if has_init != has_cleanup:
            raise LinkPlanError(
                f"module lifecycle is incomplete: {name}"
            )
        if has_init:
            export_names.extend(("init_module", "cleanup_module"))
        export_names = sorted(set(export_names))
        exports = []
        for symbol in export_names:
            kind = record["defined"].get(symbol)
            if kind is None:
                raise LinkPlanError(
                    f"module export is missing: {name}: {symbol}"
                )
            exports.append({"name": symbol, "kind": kind})

        imports = []
        for symbol in sorted(imports_by_name):
            item = imports_by_name[symbol]
            provider_name = item.get("provider")
            optional = item.get("optional") is True
            if provider_name is None:
                if not optional:
                    raise LinkPlanError(
                        f"strong module import has no provider: {name}: {symbol}"
                    )
                provider_index = None
            else:
                provider_index = index_by_name.get(provider_name)
                if provider_index is None or provider_index >= index_by_name[name]:
                    raise LinkPlanError(
                        f"module import provider order is invalid: "
                        f"{name}: {symbol}: {provider_name}"
                    )
                provider_exports = {
                    entry["name"] for entry in nodes[provider_index]["exports"]
                }
                if symbol not in provider_exports:
                    raise LinkPlanError(
                        f"module import is not exported: "
                        f"{name}: {symbol}: {provider_name}"
                    )
            imports.append({
                "name": symbol,
                "provider_index": provider_index,
                "optional": optional,
            })
        nodes.append({
            "name": name,
            "kind": "relocatable-module",
            "size": record["size"],
            "sha256": record["sha256"],
            "exports": exports,
            "imports": imports,
            "init_symbol": "init_module" if has_init else None,
            "cleanup_symbol": "cleanup_module" if has_cleanup else None,
        })
    return {"identity": "dev", "nodes": nodes}


def c_string(value):
    return json.dumps(value, ensure_ascii=True)


def render_plan(plan):
    lines = [
        "// SPDX-License-Identifier: GPL-2.0-only",
        "",
        '#include "link_plan.h"',
        "",
    ]
    for index, node in enumerate(plan["nodes"]):
        if node["exports"]:
            lines.append(
                f"static const struct kobox_link_plan_export "
                f"node_{index}_exports[] = {{"
            )
            for item in node["exports"]:
                kind = "KOBOX_LINK_PLAN_SYMBOL_FUNCTION" if \
                    item["kind"] == "function" else \
                    "KOBOX_LINK_PLAN_SYMBOL_OBJECT"
                lines.append(f"\t{{ {c_string(item['name'])}, {kind} }},")
            lines.extend(("};", ""))
        if node["imports"]:
            lines.append(
                f"static const struct kobox_link_plan_import "
                f"node_{index}_imports[] = {{"
            )
            for item in node["imports"]:
                provider = "KOBOX_LINK_PLAN_NO_PROVIDER" if \
                    item["provider_index"] is None else \
                    f"{item['provider_index']}u"
                optional = "1u" if item["optional"] else "0u"
                lines.append(
                    f"\t{{ {c_string(item['name'])}, {provider}, "
                    f"{optional} }},"
                )
            lines.extend(("};", ""))

    lines.append("static const struct kobox_link_plan_node nodes[] = {")
    for index, node in enumerate(plan["nodes"]):
        kind = "KOBOX_LINK_PLAN_SHARED_PROVIDER" if \
            node["kind"] == "shared-provider" else \
            "KOBOX_LINK_PLAN_RELOCATABLE_MODULE"
        digest = ", ".join(
            f"0x{node['sha256'][offset:offset + 2]}"
            for offset in range(0, 64, 2)
        )
        exports = f"node_{index}_exports" if node["exports"] else "NULL"
        imports = f"node_{index}_imports" if node["imports"] else "NULL"
        lines.extend((
            "\t{",
            f"\t\t.name = {c_string(node['name'])},",
            f"\t\t.kind = {kind},",
            f"\t\t.content_size = UINT64_C({node['size']}),",
            f"\t\t.content_digest = {{ {digest} }},",
            f"\t\t.exports = {exports},",
            f"\t\t.export_count = {len(node['exports'])}u,",
            f"\t\t.imports = {imports},",
            f"\t\t.import_count = {len(node['imports'])}u,",
            f"\t\t.init_symbol = "
            f"{c_string(node['init_symbol']) if node['init_symbol'] else 'NULL'},",
            f"\t\t.cleanup_symbol = "
            f"{c_string(node['cleanup_symbol']) if node['cleanup_symbol'] else 'NULL'},",
            "\t},",
        ))
    lines.extend((
        "};",
        "",
        "const struct kobox_link_plan kobox_generated_link_plan = {",
        '\t.identity = "dev",',
        "\t.nodes = nodes,",
        f"\t.node_count = {len(plan['nodes'])}u,",
        "};",
        "",
    ))
    return "\n".join(lines)


def collect_artifacts(closure, providers, linux_build_dir, provider_dir, nm):
    records = {}
    for item in providers.get("providers", []):
        name = item["name"]
        records[name] = artifact_record(provider_dir / name, nm, dynamic=True)
    for item in closure.get("modules", []):
        name = item["path"]
        records[name] = artifact_record(linux_build_dir / name, nm)
    return records


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--closure-inventory", type=pathlib.Path, required=True)
    parser.add_argument("--provider-inventory", type=pathlib.Path, required=True)
    parser.add_argument("--linux-build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--provider-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--nm", default="nm")
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        closure = read_json(arguments.closure_inventory)
        providers = read_json(arguments.provider_inventory)
        artifacts = collect_artifacts(
            closure,
            providers,
            arguments.linux_build_dir.resolve(),
            arguments.provider_dir.resolve(),
            arguments.nm,
        )
        encoded = render_plan(make_plan(closure, providers, artifacts))
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8")
    except (LinkPlanError, OSError) as error:
        print(f"closure link plan: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
