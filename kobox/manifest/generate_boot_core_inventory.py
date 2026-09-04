#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Verify and inventory the boot-rooted Linux core artifact."""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys


INVENTORY_FORMAT = "kobox-linux-boot-core-inventory-dev"
PROFILE_FORMAT = "kobox-linux-runtime-profile-dev"


class BootCoreError(Exception):
    """The Linux boot core does not satisfy its profile."""


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
        raise BootCoreError(f"command failed: {' '.join(arguments)}: {detail}")
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
            raise BootCoreError(f"malformed baseline line: {line}")
        name, value = line.split("=", 1)
        values[name] = value
    return values


def parse_symbols(text):
    symbols = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        name, symbol_type, value = fields[:3]
        if symbol_type in ("U", "w", "v"):
            continue
        try:
            address = int(value, 16)
        except ValueError:
            continue
        if name in symbols:
            raise BootCoreError(f"duplicate linked symbol: {name}")
        size = 0
        if len(fields) >= 4:
            try:
                size = int(fields[3], 16)
            except ValueError:
                pass
        symbols[name] = {
            "type": symbol_type,
            "address": address,
            "size": size,
        }
    return symbols


def parse_sections(text):
    pattern = re.compile(
        r"\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-fA-F]+)\s+"
        r"[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s"
    )
    sections = {}
    for line in text.splitlines():
        match = pattern.search(line)
        if not match:
            continue
        name, address, size = match.groups()
        sections[name] = {
            "address": int(address, 16),
            "size": int(size, 16),
        }
    return sections


def parse_archive_definitions(text, build_dir):
    pattern = re.compile(r"^.*\[(.*)\]: (\S+) ([A-Za-z?])(?: |$)")
    definitions = {}
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
            raise BootCoreError(
                f"vmlinux archive member escapes build tree: {member}"
            ) from error
        definitions.setdefault(match.group(2), []).append({
            "source_object": member.as_posix(),
            "symbol_type": match.group(3),
        })
    return definitions


def linux_source_changes(source_tree, commit):
    allowed_files = {
        "AGENTS.md", "AGENTS-jp.md", "LICENSE", "README.md", "README-jp.md",
        "README-kobox.md", "README-kobox-jp.md",
    }
    tracked = run_command([
        "git", "-C", str(source_tree), "diff", "--name-only", "--no-renames",
        commit, "--",
    ]).splitlines()
    untracked = run_command([
        "git", "-C", str(source_tree), "ls-files", "--others",
        "--exclude-standard",
    ]).splitlines()
    return sorted({
        path for path in tracked + untracked
        if path not in allowed_files and not path.startswith("kobox/")
    })


def validate_profile(profile):
    if profile.get("format") != PROFILE_FORMAT:
        raise BootCoreError("unsupported closure profile format")
    runtime = profile.get("kernel_runtime")
    if not isinstance(runtime, dict):
        raise BootCoreError("profile has no kernel runtime")
    required_values = {
        "artifact": "vmlinux",
        "root_symbol": "start_kernel",
        "root_source": "init/main.o",
        "linker_script": "arch/x86/kernel/vmlinux.lds.S",
        "driver_closure": "modules-only",
        "process_lifecycle": "one-boot",
        "shutdown": "quiesce-modules-then-exit",
    }
    for field, expected in required_values.items():
        if runtime.get(field) != expected:
            raise BootCoreError(f"invalid kernel runtime {field}")
    overrides = runtime.get("upper_api_overrides")
    if overrides != []:
        raise BootCoreError("Linux upper API override count is not zero")
    sections = runtime.get("required_sections")
    boundaries = runtime.get("required_boundaries")
    initcall_order = runtime.get("required_initcall_order")
    scheduler_classes = runtime.get("required_scheduler_classes")
    if (not isinstance(sections, list) or not sections or
            len(sections) != len(set(sections))):
        raise BootCoreError("invalid required section set")
    if not isinstance(boundaries, list) or not boundaries:
        raise BootCoreError("invalid required boundary set")
    for pair in boundaries:
        if (not isinstance(pair, list) or len(pair) != 2 or
                any(not isinstance(symbol, str) or not symbol for symbol in pair)):
            raise BootCoreError("invalid required boundary pair")
    for name, sequence in (
        ("initcall order", initcall_order),
        ("scheduler class set", scheduler_classes),
    ):
        if (not isinstance(sequence, list) or not sequence or
                len(sequence) != len(set(sequence)) or
                any(not isinstance(symbol, str) or not symbol
                    for symbol in sequence)):
            raise BootCoreError(f"invalid required {name}")


def generate_inventory(source_tree, build_dir, profile, nm="nm",
                       readelf="readelf"):
    source_tree = source_tree.resolve()
    build_dir = build_dir.resolve()
    validate_profile(profile)
    runtime = profile["kernel_runtime"]
    paths = {
        "vmlinux": build_dir / runtime["artifact"],
        "archive": build_dir / "vmlinux.a",
        "config": build_dir / ".config",
        "release": build_dir / "include/config/kernel.release",
        "generated_linker_script": build_dir / "arch/x86/kernel/vmlinux.lds",
        "source_linker_script": source_tree / runtime["linker_script"],
        "baseline": source_tree / "kobox/upstream-baseline.env",
    }
    for path in paths.values():
        if not path.is_file():
            raise BootCoreError(f"required build input is missing: {path}")

    baseline = parse_baseline(paths["baseline"].read_text(encoding="utf-8"))
    if (baseline.get("LINUX_TAG") != profile.get("linux_tag") or
            baseline.get("LINUX_COMMIT") != profile.get("linux_commit")):
        raise BootCoreError("profile does not match the pinned upstream baseline")
    source_changes = linux_source_changes(source_tree, profile["linux_commit"])
    if source_changes:
        raise BootCoreError(
            f"Linux source differs from pinned upstream: {source_changes[0]}"
        )
    release = paths["release"].read_text(encoding="utf-8").strip()
    if release != profile.get("kernel_release"):
        raise BootCoreError("kernel release mismatch")
    config_text = paths["config"].read_text(encoding="utf-8")
    config_digest = hashlib.sha256(config_text.encode()).hexdigest()
    if config_digest != profile.get("config_sha256"):
        raise BootCoreError("config digest mismatch")
    config = parse_config(config_text)
    for name, expected in sorted(profile.get("required_config", {}).items()):
        if config.get(name, "n") != expected:
            raise BootCoreError(f"config mismatch for {name}")

    symbols = parse_symbols(run_command([
        nm, "--extern-only", "--defined-only", "--format=posix",
        str(paths["vmlinux"]),
    ]))
    root = symbols.get(runtime["root_symbol"])
    if root is None:
        raise BootCoreError(f"missing boot root: {runtime['root_symbol']}")
    definitions = parse_archive_definitions(run_command([
        nm, "-A", "--defined-only", "--format=posix", str(paths["archive"]),
    ]), build_dir)
    root_definitions = definitions.get(runtime["root_symbol"], [])
    if len(root_definitions) != 1:
        raise BootCoreError("boot root has no unique source object")
    if root_definitions[0]["source_object"] != runtime["root_source"]:
        raise BootCoreError("boot root comes from the wrong source object")

    sections = parse_sections(run_command([
        readelf, "-SW", str(paths["vmlinux"]),
    ]))
    section_output = []
    for name in runtime["required_sections"]:
        section = sections.get(name)
        if section is None or section["size"] == 0:
            raise BootCoreError(f"required linker section is empty: {name}")
        section_output.append({"name": name, **section})

    boundary_output = []
    for begin_name, end_name in runtime["required_boundaries"]:
        begin = symbols.get(begin_name)
        end = symbols.get(end_name)
        if begin is None or end is None:
            raise BootCoreError(
                f"required linker boundary is missing: {begin_name}/{end_name}"
            )
        span = end["address"] - begin["address"]
        if span <= 0:
            raise BootCoreError(
                f"required linker boundary is empty: {begin_name}/{end_name}"
            )
        boundary_output.append({
            "begin": begin_name,
            "end": end_name,
            "begin_address": begin["address"],
            "end_address": end["address"],
            "span": span,
        })

    initcall_output = []
    prior_address = None
    for name in runtime["required_initcall_order"]:
        symbol = symbols.get(name)
        if symbol is None:
            raise BootCoreError(f"required initcall anchor is missing: {name}")
        if prior_address is not None and symbol["address"] <= prior_address:
            raise BootCoreError(f"initcall anchors are not ordered at {name}")
        prior_address = symbol["address"]
        initcall_output.append({"name": name, "address": symbol["address"]})

    scheduler_output = []
    expected_address = symbols["__sched_class_highest"]["address"]
    for name in runtime["required_scheduler_classes"]:
        symbol = symbols.get(name)
        if symbol is None or symbol["size"] == 0:
            raise BootCoreError(f"required scheduler class is missing: {name}")
        if symbol["address"] != expected_address:
            raise BootCoreError(f"scheduler class layout is not contiguous at {name}")
        scheduler_output.append({
            "name": name,
            "address": symbol["address"],
            "size": symbol["size"],
        })
        expected_address += symbol["size"]
    if expected_address != symbols["__sched_class_lowest"]["address"]:
        raise BootCoreError("scheduler class layout does not reach its boundary")

    return {
        "format": INVENTORY_FORMAT,
        "profile": profile["name"],
        "linux": {
            "tag": profile["linux_tag"],
            "commit": profile["linux_commit"],
            "kernel_release": release,
            "config_sha256": config_digest,
            "toolchain": profile["toolchain"],
        },
        "artifact": {
            "path": runtime["artifact"],
            "content_size": paths["vmlinux"].stat().st_size,
            "sha256": sha256_file(paths["vmlinux"]),
        },
        "boot_root": {
            "symbol": runtime["root_symbol"],
            "source_object": runtime["root_source"],
            "address": root["address"],
        },
        "linker": {
            "source_script": runtime["linker_script"],
            "source_script_sha256": sha256_file(paths["source_linker_script"]),
            "generated_script": "arch/x86/kernel/vmlinux.lds",
            "generated_script_sha256": sha256_file(
                paths["generated_linker_script"]
            ),
            "sections": section_output,
            "boundaries": boundary_output,
            "initcall_order": initcall_output,
            "scheduler_classes": scheduler_output,
        },
        "lifecycle": {
            "process": runtime["process_lifecycle"],
            "shutdown": runtime["shutdown"],
        },
        "gate": {
            "boot_core_linked": True,
            "linux_source_change_count": len(source_changes),
            "linux_upper_api_override_count": len(
                runtime["upper_api_overrides"]
            ),
        },
    }


def encode_inventory(inventory):
    return json.dumps(
        inventory, ensure_ascii=False, indent=2, sort_keys=True,
    ) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-tree", type=pathlib.Path, required=True)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--check", type=pathlib.Path)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--readelf", default="readelf")
    arguments = parser.parse_args()
    if arguments.output and arguments.check:
        parser.error("--output and --check are mutually exclusive")
    try:
        profile = json.loads(arguments.profile.read_text(encoding="utf-8"))
        encoded = encode_inventory(generate_inventory(
            arguments.source_tree, arguments.build_dir, profile,
            arguments.nm, arguments.readelf,
        ))
        if arguments.check:
            if arguments.check.read_text(encoding="utf-8") != encoded:
                raise BootCoreError(
                    f"generated inventory differs from {arguments.check}"
                )
        elif arguments.output:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(encoded, encoding="utf-8")
        else:
            sys.stdout.write(encoded)
    except (BootCoreError, OSError, json.JSONDecodeError) as error:
        print(f"boot core inventory: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
