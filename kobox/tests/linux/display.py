# SPDX-License-Identifier: GPL-2.0-only
"""Independent, read-only observation of this run's QEMU SDL window."""

from gates.display import DisplayEvidence
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path


def capture_png(path):
    """Lossless container conversion of the observed pixels, for inspection."""
    magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    width, height = map(int, dimensions.split())
    if (magic != b"P6" or maximum != b"255" or (width, height) not in
            ((640, 480), (800, 600)) or len(pixels) != width * height * 3):
        raise ValueError("invalid display capture")

    def chunk(name, data):
        return (struct.pack(">I", len(data)) + name + data +
                struct.pack(">I", zlib.crc32(name + data)))

    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    with path.with_suffix(".png").open("xb") as output:
        output.write(png)


class DisplayOracle:
    def __init__(self, executable, artifact_root, environment, qemu):
        self.directory = Path(tempfile.mkdtemp(prefix="kms-display-", dir=artifact_root))
        self.name = self.directory.name
        self.executable = executable
        self.output = ""
        self.result = None
        environment["KOBOX_DISPLAY_QEMU"] = str(qemu.resolve())
        environment["KOBOX_DISPLAY_NAME"] = self.name

    def __enter__(self):
        self.process = subprocess.Popen(
            [str(self.executable.resolve()), self.name, str(self.directory)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return self

    def __exit__(self, *_):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.output, _ = self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.output, _ = self.process.communicate()
            raise RuntimeError("display observer failed to stop")
        self.result = self.process.returncode
        print(self.output, end="", flush=True)
        for path in sorted(self.directory.glob("display-*.ppm")):
            capture_png(path)
        print(f"KMS display captures: {self.directory}", flush=True)

    def evidence(self):
        return DisplayEvidence.from_report(self.result, self.output)

    def verify(self):
        self.evidence().verify()

    def verify_rejection(self):
        self.evidence().verify_rejection()
