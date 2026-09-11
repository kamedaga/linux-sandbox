# SPDX-License-Identifier: GPL-2.0-only
"""Display workload assertions; no window system or process launcher."""

import re


class ScanoutEvidence:
    def __init__(self):
        self.live = {}
        self.active = 0
        self.frames = []
        self.flushes = []
        self.errors = []
        self.created = 0

    def observe(self, event):
        kind, *values = event
        if kind == "resource_create":
            resource, fmt, width, height, depth = values
            if resource in self.live:
                self.errors.append("resource ID reused before release")
            self.live[resource] = (fmt, width, height, depth)
            self.created += 1
        if kind == "scanout":
            head, resource, width, height, x, y = values
            if head or x or y:
                self.errors.append("unexpected scanout head or crop")
            self.active = resource
            if resource:
                if self.live.get(resource) != (2, width, height, 1):
                    self.errors.append("scanout is not the live XRGB8888 3D resource")
                self.frames.append((resource, width, height))
        if kind == "resource_flush":
            resource, width, height, x, y = values
            if resource != self.active or x or y:
                self.errors.append("flush does not belong to the active scanout")
            self.flushes.append((resource, width, height))
        if kind == "resource_release":
            resource, = values
            if resource == self.active or resource not in self.live:
                self.errors.append("release of active or unknown scanout resource")
            self.live.pop(resource, None)

    def verify(self):
        if (self.errors or self.live or self.active or len(self.frames) != 64 or
                self.frames != self.flushes):
            raise RuntimeError(f"invalid native scanout lifecycle: {self.errors}, "
                               f"live={self.live}, frames={len(self.frames)}, flushes={len(self.flushes)}")
        for start in range(0, 64, 16):
            group = self.frames[start:start + 16]
            dimensions = (800, 600) if (start // 16) % 2 else (640, 480)
            if (any((w, h) != dimensions for _, w, h in group) or
                    len({r for r, _, _ in group}) not in (2, 3) or
                    any(a[0] == b[0] for a, b in zip(group, group[1:]))):
                raise RuntimeError("native scanout did not continuously flip and reuse GBM resources")
        print(f"KMS native scanout: frames=64 flushes=64 resources={self.created} released=all", flush=True)

class DisplayEvidence:
    def __init__(self, result, records, reported, rejected):
        self.result = result
        self.records = records
        self.reported = reported
        self.rejected = rejected

    @classmethod
    def from_report(cls, result, output):
        # The observer report is the workload contract, not a window-system API.
        records = [tuple(map(int, record)) for record in re.findall(
            r"KMS display: code=(\d+) size=(\d+)x(\d+) pixels=ok", output)]
        return cls(result, records, "KMS display: code=" in output,
                   "seen=0000000000000000 mismatched=ffffffffffffffff" in output)

    def verify(self):
        if self.result != 0 or [r[0] for r in self.records] != list(range(64)):
            raise RuntimeError("not all GPU frames appeared in the observed display")
        for code, width, height in self.records:
            expected = (800, 600) if (code // 16) % 2 else (640, 480)
            if (width, height) != expected:
                raise RuntimeError("display dimensions do not match the modeset")

    def verify_rejection(self):
        if self.result != 1 or self.reported or not self.rejected:
            raise RuntimeError("display negative control did not observe the wrong pixels")
        print("Mesa display oracle: wrong displayed GPU pixels rejected", flush=True)
