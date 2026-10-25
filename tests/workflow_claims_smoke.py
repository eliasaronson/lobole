#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The modeling-workflow prompt's claims, held against the app.

The MCP prompt (`WORKFLOW` in python/lobole/mcp.py) tells an agent what it
can rely on; docs/scripting/mcp.md#the-workflow-prompt lists each claim and
the test that pins it. These are the ones nothing else pinned:

  - a mutating call answers a `digest` -- `set_variable` first, the call
    that most often moves geometry and the one that answered without one
    (tests/rpc_digest_gate.py holds every handler to it from the source);
    `batch` answers one for the whole;
  - `set_rollback` shows an intermediate state, in the call that moves it;
  - get_state carries `selection` and `selection_items`, which is what an
    agent reads for "this" and "the selected".

The frame half of check_literals' placement claim is in literals_smoke.py,
beside the sketch half. Headless.

Usage: workflow_claims_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def total(digest):
    return sum(b["volume_mm3"] for b in digest["bodies"])


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_claims_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")

        # A 20 mm block whose height is a variable, and a bore through it.
        d.call("set_variable", name="h", expression="20")
        blk = box(d, (0, 0, 0), (20, 20, 20), "Block", height_expr="h")["id"]
        bore = cylinder(d, (10, 10, 0), "z", 3, 20, "Bore", mode="cut",
                        height_expr="h")["id"]
        solid = 20.0 ** 3
        state = d.call("get_state")
        bored = sum(b["volume_mm3"] for b in state["bodies"])
        expect(bored < solid - 1.0, f"the bore cut the block: {bored}")

        # --- set_variable answers a digest, with the delta it made --------
        r = d.call("set_variable", name="h", expression="30")
        expect("digest" in r, f"set_variable answers a digest: {sorted(r)}")
        grew = r["digest"]["delta"]["volume_mm3"]
        expect(abs(total(r["digest"]) - bored * 1.5) < 1e-3 * bored,
               f"the digest is the block at 30 mm: {r['digest']}")
        expect(abs(grew - bored * 0.5) < 1e-3 * bored,
               f"its delta is what the edit added: {grew}")
        tall = total(r["digest"])

        # --- set_rollback shows an intermediate state, in its own answer --
        r = d.call("set_rollback", feature_id=bore)
        expect("digest" in r, f"set_rollback answers a digest: {sorted(r)}")
        expect(abs(total(r["digest"]) - 20 * 20 * 30) < 1e-3,
               f"before the bore, the block is whole: {r['digest']}")
        expect(r["digest"]["delta"]["volume_mm3"] > 0,
               f"and the delta says the bore came out: {r['digest']}")
        state = d.call("get_state")
        expect(abs(sum(b["volume_mm3"] for b in state["bodies"]) - 20 * 20 * 30)
               < 1e-3, "get_state agrees with the rolled-back digest")
        r = d.call("set_rollback", index=-1)
        expect(abs(total(r["digest"]) - tall) < 1e-6,
               f"back at the end, the bore is in again: {r['digest']}")

        # --- a batch answers one digest for the whole ---------------------
        r = d.call("batch", calls=[
            {"method": "set_variable", "params": {"name": "h",
                                                  "expression": "20"}},
            {"method": "set_variable", "params": {"name": "h",
                                                  "expression": "25"}}])
        expect("digest" in r, f"a batch answers a digest: {sorted(r)}")
        for res in r["results"]:
            expect("digest" not in res["result"],
                   f"a member's digest is stripped: {res}")
        expect(abs(r["digest"]["delta"]["volume_mm3"]
                   - (bored * 1.25 - tall)) < 1e-3 * bored,
               f"its delta is the whole batch's: {r['digest']}")

        # --- get_state carries the selection ------------------------------
        state = d.call("get_state")
        expect("selection" in state and "selection_items" in state,
               f"get_state has selection and selection_items: {sorted(state)}")
        d.call("select_feature", feature_id=blk)
        items = d.call("get_state")["selection_items"]
        expect(any(it.get("feature_id") == blk or it.get("id") == blk
                   for it in items),
               f"selection_items names the selected feature: {items}")
        d.call("select_feature", feature_id=0)
        expect(d.call("get_state")["selection_items"] == [],
               "cleared, it is empty -- and an agent asks instead of "
               "guessing")

        print("workflow_claims_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    main()
