#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).with_name("verify_surface.py")
SPEC = importlib.util.spec_from_file_location("verify_surface", SCRIPT)
verify_surface = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify_surface)


class VerifySurfaceTest(unittest.TestCase):
    def test_machine_has_no_host_or_atomic_runtime(self):
        verify_surface.validate_machine_symbols(set())
        for symbol in ("pthread_mutex_lock", "__atomic_load_8", "__tls_get_addr",
                       "kobox_posix_cpu_notify", "syscall"):
            with self.subTest(symbol=symbol), self.assertRaisesRegex(
                verify_surface.SurfaceError, "common machine import",
            ):
                verify_surface.validate_machine_symbols({symbol})

    def test_accepts_declared_posix_surface(self):
        symbols = verify_surface.parse_undefined(
            "pthread_create U\nclock_gettime U\nmmap U\n"
        )
        verify_surface.validate_symbols(symbols)

    def test_rejects_linux_runtime_shortcut(self):
        with self.assertRaisesRegex(
            verify_surface.SurfaceError, "forbidden Linux primitive",
        ):
            verify_surface.validate_symbols({"timerfd_create"})

    def test_rejects_undeclared_host_import(self):
        with self.assertRaisesRegex(
            verify_surface.SurfaceError, "undeclared host import",
        ):
            verify_surface.validate_symbols({"socket"})


if __name__ == "__main__":
    unittest.main()
