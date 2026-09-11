# SPDX-License-Identifier: GPL-2.0-only
"""Mesa Gate assertions, independent of native launch and device observation."""

from dataclasses import dataclass
import re

from .display import ScanoutEvidence
from .multi import MultiEvidence


@dataclass(frozen=True)
class MesaOptions:
    kms: bool = False
    multi: bool = False
    recovery: bool = False
    draw: bool = False
    expect_shared_mismatch: bool = False
    expect_display_mismatch: bool = False
    expect_pixel_mismatch: bool = False

    def __post_init__(self):
        if self.recovery and not self.multi:
            raise ValueError("recovery requires concurrent clients")
        if self.multi and (not self.kms or self.draw or self.expect_display_mismatch):
            raise ValueError("concurrent clients require their KMS/shared-pixel workload")
        if self.expect_shared_mismatch and (not self.multi or self.recovery):
            raise ValueError("shared-pixel control requires concurrent clients without recovery")
        if self.kms and self.draw:
            raise ValueError("KMS and offscreen drawing are separate workloads")
        if self.expect_display_mismatch and not self.kms:
            raise ValueError("display-pixel control requires KMS")
        if self.expect_pixel_mismatch and not self.draw:
            raise ValueError("shader-pixel control requires offscreen drawing")


class MesaEvidence:
    def __init__(self, options):
        self.options = options
        self.traces = {name: 0 for name in ("create", "submit", "destroy")}
        self.transfers = {name: 0 for name in ("toh", "fromh")}
        self.fences = set()
        self.completed_fences = set()
        self.drawings = []
        self.displays = []
        self.scanouts = ScanoutEvidence() if self.options.kms else None
        self.pixel_mismatch = False
        self.failed_draw_released = False
        self.driver_drained = False
        self.multi = MultiEvidence() if self.options.multi else None
        self.shared_results = []
        self.retained = 0
        self.shared_mismatch = False
        self.revoked = False
        self.restarted = False
        self.retired_irq = False
        self.death_waiters = False
        self.recovery_errors = []

    def observe(self, line, event=None):
        if event:
            kind, *values = event
            if kind in ("context_create", "context_submit", "context_destroy"):
                self.traces[kind.removeprefix("context_")] += 1
            elif kind == "transfer":
                self.transfers[values[0]] += 1
            elif kind == "fence_submit":
                self.fences.add(values[0])
            elif kind == "fence_complete":
                self.completed_fences.add(values[0])
            if self.scanouts:
                self.scanouts.observe(event)
            if self.multi:
                self.multi.observe(event)
        drawing = re.fullmatch(
            r"Mesa draw: frames=(\d+) pixels=(\d+) fences=(\d+) waits=(\d+) "
            r"reused=(\d+) slots=(\d+) uploads=(\d+) readbacks=(\d+) guards=ok\n", line)
        if drawing:
            self.drawings.append(tuple(map(int, drawing.groups())))
        display = re.fullmatch(r"Mesa KMS: cpu=(\d+) modes=2 frames=32 flips=30 events=30 released=1\n", line)
        if display:
            self.displays.append(int(display[1]))
        if self.multi:
            shared = re.fullmatch(r"Mesa shared: role=([01]) frames=32 pixels=308416 sync_file=32 peer=ok\n", line)
            if shared:
                self.shared_results.append(int(shared[1]))
            if line == "Mesa shared lifetime: exporter-exited=1 retained-pixels=4819\n":
                self.retained += 1
            if line.startswith("Mesa shared pixel mismatch: role=1 frame=0 x=17 y=23"):
                self.shared_mismatch = True
        owner = re.fullmatch(r"Remote hardware owner: generation=(\d+) requests=\d+ child-reaped=1 revoked=1\n", line)
        if line == "Mesa generation death checkpoint: actual-poll=2 pending-driver-fences=2\n":
            self.death_waiters = True
        if self.options.recovery and owner:
            if int(owner[1]) == 1:
                try:
                    self.multi.verify_killed()
                except RuntimeError as error:
                    self.recovery_errors.append(str(error))
                if self.scanouts.frames or not self.fences or not self.death_waiters:
                    self.recovery_errors.append("death must precede display with real pending device fences")
                self.revoked = True
                self.multi = MultiEvidence()
                self.scanouts = ScanoutEvidence()
                self.traces = dict.fromkeys(self.traces, 0)
                self.fences.clear()
                self.completed_fences.clear()
            elif int(owner[1]) == 2:
                self.restarted = True
        if re.fullmatch(r"Generation IRQ: retired=1 expected=1 fresh=[1-9]\d*\n", line):
            self.retired_irq = True
        if line.startswith("Mesa pixel mismatch: frame=0 x=17 y=23 "):
            self.pixel_mismatch = True
        if line == "Mesa failed draw: GL resources and context released\n":
            self.failed_draw_released = True
        if line.startswith("Native virtio:") and "cleanup=0 drained=1 warnings=0" in line:
            self.driver_drained = True

    def verify(self, result, display=None):
        if self.options.expect_shared_mismatch:
            if (result != 10 or not self.shared_mismatch or self.shared_results or
                    not self.driver_drained or self.traces["create"] != 2 or self.traces["destroy"] != 2):
                raise RuntimeError("wrong shared GPU pixel was not rejected with driver cleanup")
            print("Mesa shared oracle: wrong peer GPU pixel rejected", flush=True)
            return 0
        if self.options.expect_pixel_mismatch:
            if (result != 10 or not self.pixel_mismatch or self.drawings or not self.traces["submit"] or
                    not self.failed_draw_released or not self.driver_drained or self.traces["destroy"] != 1):
                raise RuntimeError("bad-shader control did not fail at the expected pixel")
            print("Mesa pixel oracle: deliberately wrong GPU pixel rejected", flush=True)
            return 0
        if result:
            return result
        contexts = 4 if self.options.multi else 2
        if self.traces["create"] != contexts or self.traces["destroy"] != contexts or self.traces["submit"] < contexts:
            raise RuntimeError(f"missing actual device context/submission evidence: {self.traces}")
        print(f"Mesa native virtqueue evidence: {self.traces}", flush=True)
        if self.options.multi:
            if sorted(self.shared_results) != [0, 0, 1, 1] or self.retained != 2:
                raise RuntimeError("missing two-process shared pixels or exporter-exit lifetime")
            self.multi.verify()
            if self.options.recovery and (self.recovery_errors or not (self.revoked and self.restarted and self.retired_irq)):
                raise RuntimeError("missing host-owned revoke, stale IRQ rejection or fresh generation: "
                                   + repr(self.recovery_errors))
        if self.options.kms:
            if display is None:
                raise RuntimeError("missing independent display observation")
            if (self.displays != [0, 1] or not self.driver_drained or len(self.fences) < 64 or
                    not self.fences.issubset(self.completed_fences)):
                raise RuntimeError(f"missing actual KMS/event/fence/cleanup evidence: {self.displays}")
            self.scanouts.verify()
            if self.options.expect_display_mismatch:
                display.verify_rejection()
            else:
                display.verify()
                print("Mesa KMS Gate: 64 GPU frames displayed, 60 flip events, two modes on both CPUs", flush=True)
        if self.options.draw:
            if len(self.drawings) != 2 or any(
                    frames != 128 or pixels != frames * 73 * 59 or syncs != 2 * frames or
                    waits < syncs or reused != frames - 3 or slots != 3 or
                    uploads != frames or readbacks != frames
                    for frames, pixels, syncs, waits, reused, slots, uploads, readbacks in self.drawings):
                raise RuntimeError(f"missing full two-CPU drawing evidence: {self.drawings}")
            # Mesa's normal path can encode TRANSFER3D/COPY_TRANSFER3D inside
            # SUBMIT_3D. Standalone virtio transfer counts are diagnostic only;
            # full shader-transformed PBO bytes and real fence responses are required.
            if (self.traces["submit"] < 256 or len(self.fences & self.completed_fences) < 256 or
                    not self.fences.issubset(self.completed_fences)):
                raise RuntimeError(f"missing real device submission/fence evidence: {self.traces}, "
                                   f"completed={len(self.fences & self.completed_fences)}")
            print(f"Mesa draw device evidence: transfers={self.transfers}, "
                  f"completed-fences={len(self.fences & self.completed_fences)}", flush=True)
        return 0
