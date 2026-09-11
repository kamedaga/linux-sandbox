# SPDX-License-Identifier: GPL-2.0-only
"""Trace assertions for real concurrent Mesa contexts and shared resources."""

class MultiEvidence:
    def __init__(self):
        self.active = {}
        self.submissions = {}
        self.shared = set()
        self.pairs = 0
        self.errors = []

    def observe(self, event):
        kind, *values = event
        if kind == "context_create":
            context, name = values
            if name != "client":
                return
            if context in self.active or len(self.active) >= 2:
                self.errors.append("duplicate or unexpected context")
            self.active[context] = set()
            self.submissions[context] = 0
        if kind == "context_attach":
            context, resource = values
            if context not in self.active:
                self.errors.append("attachment without a live context")
            else:
                self.active[context].add(resource)
                if len(self.active) == 2:
                    self.shared.update(set.intersection(*self.active.values()))
        if kind == "context_submit":
            context, size = values
            if context not in self.active:
                self.errors.append("submission without a live context")
            elif len(self.active) == 2:
                self.submissions[context] += 1
        if kind == "context_destroy":
            context, = values
            if context not in self.active:
                self.errors.append("destroy without a live context")
            else:
                del self.active[context]
            if not self.active:
                if (len(self.submissions) != 2 or len(self.shared) < 2 or
                        min(self.submissions.values(), default=0) < 32):
                    self.errors.append("missing concurrent submissions or two-way PRIME attachment")
                self.submissions.clear()
                self.shared.clear()
                self.pairs += 1

    def verify(self):
        if self.errors or self.active or self.pairs != 2:
            raise RuntimeError(f"incomplete concurrent context lifecycle: {vars(self)}")
        print("Mesa concurrent device evidence: two live contexts, bidirectional PRIME, both pairs released", flush=True)

    def verify_killed(self):
        if (self.errors or len(self.active) != 2 or self.pairs or
                len(self.shared) < 2 or min(self.submissions.values(), default=0) < 16):
            raise RuntimeError(f"death did not interrupt concurrent shared rendering: {vars(self)}")
