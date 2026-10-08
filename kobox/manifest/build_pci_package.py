#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turn a canonical Linux PCI closure into a Kobox2 package.

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
    parser.add_argument("--generator-dir", type=pathlib.Path,
                        help="keep host-only generator sources/tools outside the published package")
    parser.add_argument("--profile", choices=("network", "amdgpu",
                                              "usb-storage-xhci", "nvme", "storage"),
                        default="network")
    parser.add_argument("--extension-module", action="append", type=pathlib.Path,
                        default=[], help="Kobox module built against this canonical core")
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text())
    if inventory.get("profile") != args.profile:
        raise ValueError(f"{args.profile} package requires its own closure inventory")
    order = list(inventory["load_order"])
    if len(order) != len(inventory["modules"]) or not 0 < len(order) < 64:
        raise ValueError("invalid PCI closure module count")
    if {item["path"] for item in inventory["modules"]} != set(order):
        raise ValueError("PCI closure load order omits a module")
    roots = set(inventory["roots"])
    if not roots <= set(order):
        raise ValueError("PCI closure root is not a module")
    extension_sources = {}
    extension_edges = []
    for source in args.extension_module:
        if not source.is_file() or source.suffix != ".ko":
            raise ValueError(f"missing extension module: {source}")
        name = subprocess.check_output(
            ["modinfo", "-F", "name", str(source)], text=True).strip()
        dependencies = subprocess.check_output(
            ["modinfo", "-F", "depends", str(source)], text=True).strip()
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,63}", name):
            raise ValueError(f"unsafe extension module name: {name}")
        path = f"extensions/{name}.ko"
        available = {pathlib.PurePosixPath(item).stem.replace("-", "_"): item
                     for item in order}
        if not name or name in available or path in extension_sources:
            raise ValueError(f"duplicate or unnamed extension module: {source}")
        providers = []
        for dependency in filter(None, (item.strip() for item in dependencies.split(","))):
            if dependency not in available:
                raise ValueError(f"extension dependency {dependency} is outside the closure")
            providers.append(available[dependency])
        # The generated load order must register the provider after all Linux
        # symbols it imports, but before the root driver starts probing.
        position = 1 + max((order.index(item) for item in providers), default=-1)
        if position > min(order.index(item) for item in roots):
            raise ValueError(f"extension module must precede the root driver: {name}")
        order.insert(position, path)
        extension_sources[path] = source
        extension_edges.extend((path, item) for item in providers)
        # This is a runtime service of the root driver, not an ELF import.
        # A root dependency keeps it in the sealed closure and loads it first.
        extension_edges.extend((root, path) for root in roots)
    if len(order) >= 64:
        raise ValueError("extended PCI closure has too many modules")

    output = args.output_dir
    module_output = output / "modules"
    module_output.mkdir(parents=True, exist_ok=True)
    core = output / "core.so"
    shutil.copyfile(args.runtime_dir / "linux-boot-runtime.so", core)
    source_files = [core]
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
        source = extension_sources.get(module, args.runtime_dir / "modules" / module)
        if b"name=" + name.encode() + b"\0" not in source.read_bytes():
            raise ValueError(f"hosted module name differs from Linux metadata: {module}")
        target = module_output / f"{name}.ko"
        shutil.copyfile(source, target)
        source_files.append(target)
    nodes = {path: index + 2 for index, path in enumerate(order)}
    edges = [(nodes[path], 1) for path in order]
    edges.extend((nodes[item["consumer"]], nodes[item["provider"]])
                 for item in inventory["dependencies"])
    edges.extend((nodes[consumer], nodes[provider])
                 for consumer, provider in extension_edges)
    if len(edges) != len(set(edges)):
        raise ValueError("duplicate PCI closure dependency")
    dependencies_by_node = {}
    for consumer, provider in edges:
        dependencies_by_node.setdefault(consumer, []).append(provider)
    reachable = {nodes[root] for root in roots}
    pending = list(reachable)
    while pending:
        for provider in dependencies_by_node.get(pending.pop(), ()):
            if provider not in reachable:
                reachable.add(provider)
                pending.append(provider)
    if reachable != set(range(1, len(names) + 1)):
        raise ValueError("PCI closure has artifacts unreachable from its roots")

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
    c_source = """/* Generated from a canonical Linux PCI closure; do not edit. */
#include <kobox2/closure_manifest.h>
#include <kobox2/closure.h>
#include <kobox2/pci_function_layout.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

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
static void *allocate(void *context, size_t size) {
  (void)context;
  return malloc(size);
}
static void deallocate(void *context, void *pointer, size_t size) {
  (void)context;
  (void)size;
  free(pointer);
}
static int validate_closure(void) {
  const uint8_t digest[32] = {1};
  kb2_closure_builder_t *builder = NULL;
  kb2_closure_t *closure = NULL;
  kb2_status_t status = kb2_closure_builder_create(
    allocate, deallocate, NULL, digest, sizeof(digest), &builder);
  for (size_t i = 0; status == KB2_STATUS_OK &&
       i < sizeof(artifacts)/sizeof(artifacts[0]); ++i) {
    const kb2_closure_manifest_artifact_t *item = &artifacts[i];
    status = kb2_closure_builder_add_artifact(builder, item->node_id,
      i ? KB2_ARTIFACT_RELOCATABLE_MODULE : KB2_ARTIFACT_SHARED_PROVIDER,
      item->content_digest, sizeof(item->content_digest),
      item->namespace_name.data, item->namespace_name.length);
    if (status == KB2_STATUS_OK)
      status = kb2_closure_builder_set_native_lifecycle(builder, item->node_id);
    if (status == KB2_STATUS_OK && (item->flags & KB2_CLOSURE_ARTIFACT_FLAG_ROOT))
      status = kb2_closure_builder_mark_root(builder, item->node_id);
  }
  for (size_t i = 0; status == KB2_STATUS_OK &&
       i < sizeof(dependencies)/sizeof(dependencies[0]); ++i)
    status = kb2_closure_builder_add_dependency(builder,
      dependencies[i].consumer_node_id, dependencies[i].provider_node_id);
  if (status == KB2_STATUS_OK)
    status = kb2_closure_builder_add_resource(builder, 1, KB2_RESOURCE_DEVICE,
      resource.interface_schema_digest, 32, 1, 1,
      KB2_PCI_FUNCTION_REQUIRED_RIGHTS, KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
      KB2_RESOURCE_REQUIRED | KB2_RESOURCE_SHARED);
  for (size_t i = 0; status == KB2_STATUS_OK &&
       i < sizeof(bindings)/sizeof(bindings[0]); ++i)
    status = kb2_closure_builder_bind_resource(builder,
      bindings[i].slot_id, bindings[i].node_id);
  if (status == KB2_STATUS_OK)
    status = kb2_closure_builder_seal(builder, &closure);
  kb2_closure_destroy(closure);
  kb2_closure_builder_destroy(builder);
  return status != KB2_STATUS_OK;
}
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
  if (validate_closure()) return 8;
  FILE *file = fopen(argv[1], "wb");
  if (!file) return 6;
  int ok = fwrite(bytes, 1, size, file) == size && fclose(file) == 0;
  return ok ? 0 : 7;
}
""" % ("\n".join(artifact_lines), "\n".join(edge_lines), "\n".join(binding_lines))
    generator_dir = args.generator_dir or output
    generator_dir.mkdir(parents=True, exist_ok=True)
    generated = generator_dir / f"{args.profile}-manifest-generator.c"
    generated.write_text(c_source)
    encoder = generator_dir / f"{args.profile}-manifest-generator"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(args.protocol_dir / "include"),
        "-I", str(args.protocol_dir / "generated/include"),
        "-I", str(args.protocol_dir.parent / "include"),
        str(generated), str(args.protocol_dir / "src/closure_manifest.c"),
        str(args.protocol_dir.parent / "src/closure.c"),
        "-o", str(encoder)], check=True)
    subprocess.run([str(encoder), str(output / "manifest.bin")], check=True)


if __name__ == "__main__":
    main()
