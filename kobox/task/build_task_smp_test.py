#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import importlib.util
import pathlib
import types
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).with_name("build_task_smp.py")
SPEC = importlib.util.spec_from_file_location("task_build", SCRIPT)
task_build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(task_build)


class TaskBuildTest(unittest.TestCase):
    def test_machine_support_cannot_implicitly_use_vector_registers(self):
        arguments = types.SimpleNamespace(
            source_tree=pathlib.Path("/source"), output_dir=pathlib.Path("/output"),
            cc="clang", protocol_include=pathlib.Path("/protocol"),
            architecture_include=pathlib.Path("/arch"),
            provider_build_dir=pathlib.Path("/provider"))
        with mock.patch.object(task_build, "run") as run, \
                mock.patch.object(task_build.provider, "provider_include_flags", return_value=""):
            task_build.compile_support(arguments, "kobox/task/user.c")
        self.assertIn("-mgeneral-regs-only", run.call_args.args[0])

    config = "\n".join((
        "CONFIG_SMP=y", "CONFIG_NR_CPUS=2", "CONFIG_PREEMPT=y",
        "CONFIG_PREEMPT_COUNT=y",
        "CONFIG_HIGH_RES_TIMERS=y", "CONFIG_CONTEXT_TRACKING_IDLE=y",
        "CONFIG_BUG=y", "CONFIG_RCU_EQS_DEBUG=y",
    ))

    def test_static_preempt_smp_required(self):
        task_build.validate_config(self.config)
        for setting in self.config.splitlines():
            with self.subTest(setting=setting):
                with self.assertRaises(task_build.TaskBuildError):
                    task_build.validate_config(self.config.replace(setting, ""))

    def test_dynamic_preempt_rejected(self):
        with self.assertRaisesRegex(task_build.TaskBuildError, "static PREEMPT"):
            task_build.validate_config(self.config + "\nCONFIG_PREEMPT_DYNAMIC=y")

    def test_cpu_data_sections_are_not_ordinary_bss(self):
        symbols = task_build.parse_percpu_symbols(
            "0000 g O .data..percpu..hot 0008 cpu_value\n"
            "0000 g O .bss 0008 ordinary_value\n"
        )
        self.assertEqual(symbols, {"cpu_value"})

    def test_foreign_percpu_guard_is_rejected(self):
        boundary = {"cpu_value": {"source_object": "cpu.o", "type": "data"}}
        with self.assertRaisesRegex(task_build.TaskBuildError, "per-CPU state"):
            task_build.validate_boundary_states(boundary, lambda name, owner: True)

    def test_non_percpu_guard_is_allowed(self):
        boundary = {"later_api": {"source_object": "later.o", "type": "function"}}
        task_build.validate_boundary_states(boundary, lambda name, owner: False)

    def test_data_only_sources_cannot_supply_native_switch_code(self):
        self.assertFalse(task_build.PERCPU_DATA_OBJECTS & task_build.TASK_SOURCE_OBJECTS)


if __name__ == "__main__":
    unittest.main()
