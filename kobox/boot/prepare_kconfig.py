#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Generate the hosted entry for native Kbuild's KBUILD_KCONFIG argument."""

import argparse
import importlib.util
import pathlib


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "provider/build_shared_providers.py"
SPEC = importlib.util.spec_from_file_location("shared_provider_build", SCRIPT)
provider = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provider)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-tree", required=True, type=pathlib.Path)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    print(provider.prepare_hosted_kconfig(args.source_tree, args.build_dir))


if __name__ == "__main__":
    main()
