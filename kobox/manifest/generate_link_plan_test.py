#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).with_name("generate_link_plan.py")
SPEC = importlib.util.spec_from_file_location("closure_link_plan", SCRIPT)
link_plan = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(link_plan)


DIGEST_A = "11" * 32
DIGEST_B = "22" * 32
DIGEST_C = "33" * 32


def fixture():
    linux = {"tag": "v1", "commit": "abc"}
    closure = {
        "format": link_plan.CLOSURE_FORMAT,
        "profile": "fixture",
        "linux": linux,
        "load_order": ["core.so", "provider.ko", "root.ko"],
        "dependencies": [
            {"consumer": "provider.ko", "provider": "core.so"},
            {"consumer": "root.ko", "provider": "provider.ko"},
        ],
        "modules": [
            {
                "path": "provider.ko",
                "content_size": 20,
                "sha256": DIGEST_B,
                "imports": [{
                    "name": "core_call",
                    "provider": "core.so",
                    "optional": False,
                }],
                "required_exports": [{"name": "provider_call"}],
            },
            {
                "path": "root.ko",
                "content_size": 30,
                "sha256": DIGEST_C,
                "imports": [
                    {
                        "name": "optional_hook",
                        "provider": None,
                        "optional": True,
                    },
                    {
                        "name": "provider_call",
                        "provider": "provider.ko",
                        "optional": False,
                    },
                ],
                "required_exports": [],
            },
        ],
    }
    providers = {
        "format": link_plan.PROVIDER_FORMAT,
        "profile": "fixture",
        "linux": linux,
        "load_order": ["core.so"],
        "providers": [{
            "name": "core.so",
            "content_size": 10,
            "sha256": DIGEST_A,
            "dependencies": [],
            "exports": ["core_call", "provider_internal"],
            "link_exports": ["provider_internal"],
        }],
    }
    artifacts = {
        "core.so": {
            "size": 10,
            "sha256": DIGEST_A,
            "defined": {
                "core_call": "function",
                "provider_internal": "object",
            },
            "undefined": {},
        },
        "provider.ko": {
            "size": 20,
            "sha256": DIGEST_B,
            "defined": {
                "provider_call": "function",
                "init_module": "function",
                "cleanup_module": "function",
            },
            "undefined": {"core_call": False},
        },
        "root.ko": {
            "size": 30,
            "sha256": DIGEST_C,
            "defined": {"root_internal": "function"},
            "undefined": {
                "optional_hook": True,
                "provider_call": False,
            },
        },
    }
    return closure, providers, artifacts


class LinkPlanTest(unittest.TestCase):
    def test_plan_keeps_declared_provider_exports(self):
        closure, providers, artifacts = fixture()
        plan = link_plan.make_plan(closure, providers, artifacts)

        self.assertEqual(plan["identity"], "dev")
        self.assertEqual(len(plan["nodes"]), 3)
        self.assertEqual(
            [item["name"] for item in plan["nodes"][0]["exports"]],
            ["core_call", "provider_internal"],
        )
        self.assertEqual(
            plan["nodes"][1]["imports"][0]["provider_index"], 0
        )
        self.assertEqual(plan["nodes"][1]["init_symbol"], "init_module")
        self.assertEqual(
            plan["nodes"][1]["cleanup_symbol"], "cleanup_module"
        )
        self.assertIsNone(plan["nodes"][2]["init_symbol"])
        self.assertEqual(
            [item["provider_index"] for item in plan["nodes"][2]["imports"]],
            [None, 1],
        )

    def test_artifact_digest_mismatch_is_rejected(self):
        closure, providers, artifacts = fixture()
        artifacts["root.ko"]["sha256"] = "44" * 32
        with self.assertRaisesRegex(
            link_plan.LinkPlanError, "artifact digest differs"
        ):
            link_plan.make_plan(closure, providers, artifacts)

    def test_dependency_order_is_rejected(self):
        closure, providers, artifacts = fixture()
        closure["load_order"] = ["provider.ko", "core.so", "root.ko"]
        with self.assertRaisesRegex(
            link_plan.LinkPlanError, "dependency order is invalid"
        ):
            link_plan.make_plan(closure, providers, artifacts)

    def test_unlisted_module_import_is_rejected(self):
        closure, providers, artifacts = fixture()
        artifacts["root.ko"]["undefined"]["hidden_import"] = False
        with self.assertRaisesRegex(
            link_plan.LinkPlanError, "module import set differs"
        ):
            link_plan.make_plan(closure, providers, artifacts)

    def test_incomplete_module_lifecycle_is_rejected(self):
        closure, providers, artifacts = fixture()
        artifacts["root.ko"]["defined"]["init_module"] = "function"
        with self.assertRaisesRegex(
            link_plan.LinkPlanError, "module lifecycle is incomplete"
        ):
            link_plan.make_plan(closure, providers, artifacts)

    def test_generated_source_is_dev_and_bounded(self):
        closure, providers, artifacts = fixture()
        source = link_plan.render_plan(
            link_plan.make_plan(closure, providers, artifacts)
        )

        self.assertIn('.identity = "dev"', source)
        self.assertIn(".node_count = 3u", source)
        self.assertIn("KOBOX_LINK_PLAN_NO_PROVIDER", source)


if __name__ == "__main__":
    unittest.main()
