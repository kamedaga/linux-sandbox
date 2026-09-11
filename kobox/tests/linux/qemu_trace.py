# SPDX-License-Identifier: GPL-2.0-only
"""Translate QEMU trace records into backend-independent device observations."""

import re


_HEX = r"(0x[0-9a-f]+)"
_DEC = r"(\d+)"
_PATTERNS = [
    ("context_create", rf"virtio_gpu_cmd_ctx_create ctx {_HEX}, name (.+)", (16, None)),
    ("context_attach", rf"virtio_gpu_cmd_ctx_res_attach ctx {_HEX}, res {_HEX}", (16, 16)),
    ("context_submit", rf"virtio_gpu_cmd_ctx_submit ctx {_HEX}, size {_DEC}", (16, 10)),
    ("context_destroy", rf"virtio_gpu_cmd_ctx_destroy ctx {_HEX}", (16,)),
    ("resource_create", rf"virtio_gpu_cmd_res_create_3d res {_HEX}, fmt {_HEX}, w {_DEC}, h {_DEC}, d {_DEC}", (16, 16, 10, 10, 10)),
    ("scanout", rf"virtio_gpu_cmd_set_scanout id {_DEC}, res {_HEX}, w {_DEC}, h {_DEC}, x {_DEC}, y {_DEC}", (10, 16, 10, 10, 10, 10)),
    ("resource_flush", rf"virtio_gpu_cmd_res_flush res {_HEX}, w {_DEC}, h {_DEC}, x {_DEC}, y {_DEC}", (16, 10, 10, 10, 10)),
    ("resource_release", rf"virtio_gpu_cmd_res_unref res {_HEX}", (16,)),
    ("fence_submit", rf"virtio_gpu_fence_ctrl fence {_HEX}", (16,)),
    ("fence_complete", rf"virtio_gpu_fence_resp fence {_HEX}", (16,)),
]


def decode(line):
    for kind, pattern, bases in _PATTERNS:
        # Fence traces contain additional command metadata after the identity.
        match = (re.match(pattern, line) if kind.startswith("fence_") else
                 re.fullmatch(pattern + r"\n", line))
        if match:
            return (kind, *(int(value, base) if base else value
                            for value, base in zip(match.groups(), bases)))
    for direction in ("toh", "fromh"):
        if line.startswith(f"virtio_gpu_cmd_res_xfer_{direction}_3d "):
            return ("transfer", direction)
    return None
