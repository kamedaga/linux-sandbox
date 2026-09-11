#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Give this test's SDL window a unique name, then exec unmodified QEMU."""

import os
import sys

qemu = os.environ["KOBOX_DISPLAY_QEMU"]
os.execv(qemu, [qemu, *sys.argv[1:], "-name", os.environ["KOBOX_DISPLAY_NAME"]])
