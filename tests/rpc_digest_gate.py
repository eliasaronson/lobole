#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Gate: a response that says the part regenerated also says what it built.

The modeling-workflow prompt (python/lobole/mcp.py) tells an agent that
every mutating call returns a `digest` -- per-body volume, bbox and solid
count, and the delta the call made -- and to read it before anything else.
It was false: digests were attached handler by handler, and `set_variable`,
the call that most often moves geometry, answered `value`, `scope` and
`regen` with no digest. Nothing checked that a new handler had one.

The rule this holds (docs/scripting/mcp.md#the-workflow-prompt): a handler
branch that answers `regen` answers `digest` in the same branch. `AttachRegen`
attaches both (remote/RemoteHelpers.h), so a branch that goes through it
passes; one that writes `regen` by hand must write `digest` beside it.

It reads the source, as rpc_params_gate and batch_rollback_gate do: every
method's branch in src/remote/Remote*.cpp, by brace matching
(tools/rpc_params_scan.py). Every run first plants a branch that answers
`regen` alone and checks that the audit names it: a gate nothing fails is
not a gate.

Needs no app, no GPU and no display.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from rpc_params_scan import _scopes, _strip  # noqa: E402

REMOTE = os.path.join(ROOT, "src", "remote")

# `regen` written by hand: `out["regen"] = ...` or `{"regen", ...}`.
REGEN_RE = re.compile(r'\[\s*"regen"\s*\]\s*=(?!=)|\{\s*"regen"\s*,')
# What answers the digest: the key written the same ways, or the helper
# that writes both. Written, not merely named: a batch ERASES its members'.
DIGEST_RE = re.compile(r'\[\s*"digest"\s*\]\s*=(?!=)|\{\s*"digest"\s*,|'
                       r'\bAttachRegen\s*\(')

# Branches whose `regen` is not the answer to an edit, with the reason.
EXEMPT = {
    "get_state": "a read: its `regen` is the part's standing status, and it "
                 "changes nothing to digest",
}


def audit(sources):
    """Problems, as lines. `sources` is {path: text}."""
    problems = []
    for path in sorted(sources):
        text = _strip(sources[path])
        scopes = _scopes(text)
        for m in REGEN_RE.finditer(text):
            best = None
            for names, start, end in scopes:
                if start <= m.start() <= end and (best is None or
                                                  start > best[1]):
                    best = (names, start, end)
            if best is None:
                continue  # not inside a method's branch (a helper)
            names, start, end = best
            if all(n in EXEMPT for n in names):
                continue
            if DIGEST_RE.search(text, start, end):
                continue
            line = text.count("\n", 0, m.start()) + 1
            problems.append(
                f"{os.path.relpath(path, ROOT)}:{line}: "
                f"{', '.join(names)} answers `regen` without `digest` -- "
                "capture BodyDigestJson before the edit and answer through "
                "AttachRegen(out, doc, before)")
    return problems


PLANT = '''
bool HandlePlanted(const std::string& m, json& out) {
  if (m == "planted_regen_only") {
    out = json{{"regen", RegenSummaryJson(*doc)}};
    return true;
  }
  return false;
}
'''


def main():
    paths = sorted(glob.glob(os.path.join(REMOTE, "Remote*.cpp")))
    if not paths:
        print(f"rpc_digest_gate: no handlers under {REMOTE}")
        return 1
    sources = {p: open(p, encoding="utf-8").read() for p in paths}

    planted = audit({os.path.join(REMOTE, "RemotePlanted.cpp"): PLANT})
    if not any("planted_regen_only" in p for p in planted):
        print("rpc_digest_gate: a planted branch answering `regen` alone "
              "was not found -- the audit is blind")
        return 1

    problems = audit(sources)
    if problems:
        print("rpc_digest_gate: a response answers `regen` without "
              "`digest`:")
        for p in problems:
            print("  " + p)
        return 1
    print(f"rpc_digest_gate: OK ({len(paths)} handler files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
