#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turn the canonical Linux closure into a Kobox2 network package.

The host-side C encoder uses the existing Kobox2 protocol implementation;
neither netd nor this script maintains a second driver/dependency graph.
"""

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess


def c_bytes(data):
    return "{" + ", ".join(f"0x{byte:02x}" for byte in data) + "}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--inventory", type=pathlib.Path, required=True)
    parser.add_argument("--runtime-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--protocol-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text())
    if inventory.get("profile") != "network":
        raise ValueError("network package requires the network closure inventory")
    order = inventory["load_order"]
    if len(order) != len(inventory["modules"]) or not 0 < len(order) < 64:
        raise ValueError("invalid network module count")
    if {item["path"] for item in inventory["modules"]} != set(order):
        raise ValueError("network closure load order omits a module")
    roots = set(inventory["roots"])
    if not roots <= set(order):
        raise ValueError("network closure root is not a module")

    output = args.output_dir
    module_output = output / "modules"
    module_output.mkdir(parents=True, exist_ok=True)
    source_files = [args.runtime_dir / "linux-boot-runtime.so"]
    names = ["core"]
    seen = set(names)
    for module in order:
        # Linux normalizes '-' to '_' in struct module.name. The module
        # launch gate must use that name, not the Kbuild filename spelling.
        name = pathlib.PurePosixPath(module).stem.replace("-", "_")
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,63}", name) or name in seen:
            raise ValueError(f"unsafe or duplicate module name: {name}")
        seen.add(name)
        names.append(name)
        source = args.runtime_dir / "modules" / module
        if b"name=" + name.encode() + b"\0" not in source.read_bytes():
            raise ValueError(f"hosted module name differs from Linux metadata: {module}")
        target = module_output / f"{name}.ko"
        shutil.copyfile(source, target)
        source_files.append(target)
    nodes = {path: index + 2 for index, path in enumerate(order)}
    edges = [(nodes[path], 1) for path in order]
    edges.extend((nodes[item["consumer"]], nodes[item["provider"]])
                 for item in inventory["dependencies"])
    if len(edges) != len(set(edges)):
        raise ValueError("duplicate network closure dependency")

    artifact_lines = []
    for index, (path, name) in enumerate(zip(source_files, names), start=1):
        data = path.read_bytes()
        flags = 2 | (1 if index > 1 and order[index - 2] in roots else 0)
        artifact_lines.append(
            "  {.node_id=%d, .kind=%d, .flags=%d, .content_size=%d, "
            ".content_digest=%s, .namespace_name={%s, %d}}," %
            (index, int(index > 1), flags, len(data),
             c_bytes(hashlib.sha256(data).digest()), json.dumps(name), len(name)))
    edge_lines = [f"  {{{consumer}, {provider}}}," for consumer, provider in edges]
    binding_lines = [f"  {{1, {node}}}," for node in range(1, len(names) + 1)]
    c_source = """/* Generated from the canonical Linux network closure; do not edit. */
#include <kobox2/closure_manifest.h>
#include <kobox2/pci_function_layout.h>
#include <stdio.h>
#include <stdint.h>

static const kb2_closure_manifest_artifact_t artifacts[] = {
%s
};
static const kb2_closure_manifest_dependency_t dependencies[] = {
%s
};
static const kb2_closure_manifest_binding_t bindings[] = {
%s
};
static const kb2_closure_manifest_resource_t resource = {
  .slot_id=1, .type=KB2_CLOSURE_RESOURCE_DEVICE,
  .minimum_count=1, .maximum_count=1,
  .required_rights=KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
  .maximum_rights=KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
  .flags=KB2_CLOSURE_RESOURCE_FLAG_REQUIRED | KB2_CLOSURE_RESOURCE_FLAG_SHARED,
  .interface_schema_digest=KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES,
};
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const kb2_closure_manifest_source_t source = {
    .artifacts=artifacts, .artifact_count=sizeof(artifacts)/sizeof(artifacts[0]),
    .dependencies=dependencies,
    .dependency_count=sizeof(dependencies)/sizeof(dependencies[0]),
    .resources=&resource, .resource_count=1,
    .bindings=bindings, .binding_count=sizeof(bindings)/sizeof(bindings[0]),
  };
  size_t size = 0;
  if (kb2_closure_manifest_encoded_size(&source, &size) != KB2_PROTOCOL_OK || size > 8192)
    return 3;
  uint8_t bytes[8192];
  if (kb2_closure_manifest_encode(bytes, sizeof(bytes), &size, &source) != KB2_PROTOCOL_OK)
    return 4;
  kb2_closure_manifest_t decoded;
  if (kb2_closure_manifest_decode(bytes, size, &decoded) != KB2_PROTOCOL_OK)
    return 5;
  FILE *file = fopen(argv[1], "wb");
  if (!file) return 6;
  int ok = fwrite(bytes, 1, size, file) == size && fclose(file) == 0;
  return ok ? 0 : 7;
}
""" % ("\n".join(artifact_lines), "\n".join(edge_lines), "\n".join(binding_lines))
    generated = output / "network-manifest-generator.c"
    generated.write_text(c_source)
    encoder = output / "network-manifest-generator"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(args.protocol_dir / "include"),
        "-I", str(args.protocol_dir / "generated/include"),
        str(generated), str(args.protocol_dir / "src/closure_manifest.c"),
        "-o", str(encoder)], check=True)
    subprocess.run([str(encoder), str(output / "manifest.bin")], check=True)


if __name__ == "__main__":
    main()
