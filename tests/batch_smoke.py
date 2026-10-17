#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""`batch` — one transaction, one regeneration, one undo step
(docs/scripting/rpc.md#batch).

The gripper dogfood spent 86 calls on two parts and an assembly, each paying
a full regeneration. This pins what a batch promises instead: N features
regenerate ONCE, undo takes the whole thing back in one step, a failed call
rolls back so nothing is half-applied, and the things a rollback could not
take back are refused by name.

Usage: batch_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import time

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needles, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        for needle in needles:
            expect(needle in str(e),
                   f"{method} refused, but not naming '{needle}': {e}")
        return str(e)
    raise AssertionError(f"{method} was accepted: {kw}")


def plane(z=0.0):
    return {"origin": [0, 0, z], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}


def boss_calls(n, r=2.0):
    """2n calls: a circle sketch and an additive extrude, n times over."""
    out = []
    for i in range(n):
        out.append({"method": "add_feature", "params": {"feature": {
            "type": "Sketch", "name": f"S{i}", "plane": plane(),
            "entities": [{"type": "circle",
                          "center": [6.0 * i - 30.0, 0], "radius": r}]}}})
        out.append({"method": "add_feature", "params": {"feature": {
            "type": "Extrude", "name": f"Boss{i}", "sketch_id": None,
            "distance": 3.0, "mode": "add"}}})
    return out


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_batch_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")
        d.call("new_project")
        first_part = d.call("list_parts")["parts"][0]["name"]

        # ---- 40 features, ONE regeneration ------------------------------
        # Sketches are self-contained (each carries its own entities) so the
        # extrudes can name them by the id the previous call returned — which
        # a batch cannot do. So this batch is 40 sketches: what it measures
        # is the regeneration count, not the dependency style.
        calls = [{"method": "add_feature", "params": {"feature": {
            "type": "Sketch", "name": f"S{i}", "plane": plane(),
            "entities": [{"type": "circle", "center": [3.0 * i, 0],
                          "radius": 1.0}]}}} for i in range(40)]
        t0 = time.time()
        r = d.call("batch", calls=calls)
        elapsed = time.time() - t0
        expect(r["calls"] == 40, f"40 calls: {r}")
        expect(len(r["results"]) == 40, f"40 results: {len(r['results'])}")
        expect(r["regenerations"] == 1,
               f"ONE regeneration for the batch, not {r['regenerations']}")
        expect("failed" not in r, f"nothing failed: {r.get('failed')}")
        expect(r["regen"]["ok"], f"the batch's own regen summary: {r['regen']}")
        # The per-call regen/digest are stripped: geometry is answered once.
        first = r["results"][0]["result"]
        expect("regen" not in first and "digest" not in first,
               f"per-call regen/digest stripped: {first}")
        expect("id" in first, f"but the identity it created survives: {first}")
        expect(all(e["id"] == e["result"]["id"] for e in r["results"]),
               "and each call's created id is at the top of its entry, as "
               "the call alone answers it")
        print(f"  40 features in one batch: {elapsed:.2f}s, "
              f"{r['regenerations']} regeneration")

        state = d.call("get_state")
        expect(len(state["features"]) == 40,
               f"40 features landed: {len(state['features'])}")

        # ---- ONE undo step takes the whole batch back -------------------
        d.call("undo")
        after = d.call("get_state")
        expect(len(after["features"]) == 0,
               f"one undo removed all 40, leaving {len(after['features'])}")
        d.call("redo")
        expect(len(d.call("get_state")["features"]) == 40,
               "redo puts all 40 back")
        # Clean slate for the rollback case.
        d.call("undo")
        expect(len(d.call("get_state")["features"]) == 0, "back to empty")

        # ---- A failed call rolls the WHOLE batch back -------------------
        box(d, (0, 0, 0), (10, 10, 10), "Anchor")
        before = d.call("get_state")["features"]
        err = refused(d, "batch",
                      ["call 3", "rolled back", "as it was before the batch",
                       "did not run"],
                      calls=[
                          {"method": "add_feature", "params": {"feature": {
                              "type": "Sketch", "name": "Keep1",
                              "plane": plane(),
                              "entities": [{"type": "circle",
                                            "center": [0, 0],
                                            "radius": 1.0}]}}},
                          {"method": "add_feature", "params": {"feature": {
                              "type": "Sketch", "name": "Keep2",
                              "plane": plane(),
                              "entities": [{"type": "circle",
                                            "center": [5, 0],
                                            "radius": 1.0}]}}},
                          {"method": "add_feature", "params": {"feature": {
                              "type": "Nonsense", "name": "Boom"}}},
                          {"method": "add_feature", "params": {"feature": {
                              "type": "Sketch", "name": "Never",
                              "plane": plane(), "entities": []}}}])
        print(f"  rollback says: {err[:120]}...")
        now = d.call("get_state")["features"]
        expect(len(now) == len(before),
               f"the two that succeeded were rolled back too: "
               f"{[f['name'] for f in now]}")
        expect(all(f["name"] != "Keep1" for f in now),
               "nothing from the failed batch survives")

        # A typo'd KEY is refused before ANY call runs: every call's keys are
        # checked against its row up front, so nothing is applied to roll
        # back (docs/scripting/rpc.md#a-refused-call-changes-nothing).
        refused(d, "batch", ["batch call 2", "unknown parameter 'wibble'",
                             "unchanged"], calls=[
            {"method": "add_feature", "params": {"feature": {
                "type": "Sketch", "name": "T1", "plane": plane(),
                "entities": []}}},
            {"method": "add_feature", "params": {
                "feature": {"type": "Box", "name": "T2", "on": "Top",
                            "width": 1, "depth": 1, "height": 1},
                "wibble": 3}}])
        expect(len(d.call("get_state")["features"]) == len(before),
               "a rejected key leaves the document as it was")

        # ---- stop_on_error false runs on and reports each error ---------
        r = d.call("batch", stop_on_error=False, calls=[
            {"method": "add_feature", "params": {"feature": {
                "type": "Sketch", "name": "Good1", "plane": plane(),
                "entities": [{"type": "circle", "center": [0, 0],
                              "radius": 1.0}]}}},
            {"method": "add_feature", "params": {"feature": {
                "type": "Nonsense", "name": "Bad"}}},
            {"method": "add_feature", "params": {"feature": {
                "type": "Sketch", "name": "Good2", "plane": plane(),
                "entities": [{"type": "circle", "center": [9, 0],
                              "radius": 1.0}]}}}])
        expect(len(r["failed"]) == 1 and r["failed"][0]["call"] == 2,
               f"the one failure is reported in place: {r.get('failed')}")
        names = [f["name"] for f in d.call("get_state")["features"]]
        expect("Good1" in names and "Good2" in names,
               f"the good ones stayed: {names}")
        d.call("undo")

        # ---- A feature that fails to BUILD is not a failed call ---------
        # It is reported in the final regen, exactly as it would be alone.
        r = d.call("batch", calls=[
            {"method": "add_feature", "params": {"feature": {
                "type": "Sketch", "name": "Empty", "plane": plane(),
                "entities": []}}},
            {"method": "add_feature", "params": {"feature": {
                "type": "Extrude", "name": "Nothing", "sketch_id": 999999,
                "distance": 5.0, "mode": "new_body"}}}])
        expect("failed" not in r, f"no CALL failed: {r.get('failed')}")
        expect(not r["regen"]["ok"] and r["regen"]["failed"],
               f"but the batch's own regen says the feature is broken: "
               f"{r['regen']}")
        expect("Nothing" in str(r["regen"]["failed"]),
               f"and names it: {r['regen']['failed']}")
        d.call("undo")

        # ---- Refusals: nesting, and what a rollback cannot take back ----
        refused(d, "batch", ["does not nest"], calls=[
            {"method": "batch", "params": {"calls": []}}])
        refused(d, "batch", ["save_project", "outside", "unchanged"], calls=[
            {"method": "save_project",
             "params": {"path": os.path.join(tmp, "x.dsgn")}}])
        refused(d, "batch", ["export_step", "reaches outside"], calls=[
            {"method": "export_step",
             "params": {"path": os.path.join(tmp, "x.step")}}])
        refused(d, "batch", ["screenshot"], calls=[
            {"method": "screenshot",
             "params": {"path": os.path.join(tmp, "x.png")}}])
        refused(d, "batch", ["no method", "describe"], calls=[
            {"method": "wibble", "params": {}}])
        refused(d, "batch", ["empty"], calls=[])
        refused(d, "batch", ["array"], calls={"method": "ping"})
        # Every refusal above happened BEFORE anything ran.
        expect(len(d.call("get_state")["features"]) == len(before),
               "a refused batch changed nothing")

        # ---- $ref: the sketch and the extrude that names it -------------
        # Without a backward reference a batch could not express the API's
        # commonest pair, which would have limited the whole verb to calls
        # that happen not to depend on each other.
        # In its own part, so the volume below is about these six holes and
        # nothing else the script has left lying around.
        d.call("add_part", name="drill")
        box(d, (-30, -15, 0), (60, 30, 8), "Slab")
        holes = []
        for i in range(6):
            holes.append({"method": "add_feature", "params": {"feature": {
                "type": "Sketch", "name": f"H{i}", "plane": plane(z=8.0),
                "entities": [{"type": "circle", "center": [8.0 * i - 20.0, 0],
                              "radius": 2.0}]}}})
            holes.append({"method": "add_feature", "params": {"feature": {
                "type": "Extrude", "name": f"Cut{i}",
                "sketch_id": {"$ref": len(holes)},
                "distance": 12.0, "mode": "cut", "reversed": True}}})
        r = d.call("batch", calls=holes)
        expect(r["regen"]["ok"], f"the referenced batch built: {r['regen']}")
        expect(r["regenerations"] == 1,
               f"still ONE regeneration: {r['regenerations']}")
        vol = d.call("mass_properties")["total"]["volume_mm3"]
        drilled = 60 * 30 * 8 - 6 * 3.14159265 * 4.0 * 8
        expect(abs(vol - drilled) < 1.0,
               f"six holes were actually drilled: {vol:.1f} vs {drilled:.1f}")
        d.call("undo")  # the whole drilling batch, one step
        expect(abs(d.call("mass_properties")["total"]["volume_mm3"] - 60 * 30 * 8)
               < 1e-6, "one undo took all twelve features back")

        # A $ref that points forward, at a failed call, or at a missing field
        # is refused naming what it could have had.
        refused(d, "batch", ["points BACKWARD", "call 2"], calls=[
            {"method": "add_feature", "params": {"feature": {
                "type": "Extrude", "name": "Early",
                "sketch_id": {"$ref": 2}, "distance": 1.0,
                "mode": "new_body"}}},
            {"method": "add_feature", "params": {"feature": {
                "type": "Sketch", "name": "Late", "plane": plane(),
                "entities": []}}}])
        refused(d, "batch", ["returned no 'wibble'", "it returned"], calls=[
            {"method": "add_feature", "params": {"feature": {
                "type": "Sketch", "name": "R1", "plane": plane(),
                "entities": []}}},
            {"method": "add_feature", "params": {"feature": {
                "type": "Extrude", "name": "R2",
                "sketch_id": {"$ref": 1, "key": "wibble"},
                "distance": 1.0, "mode": "new_body"}}}])
        # The slab alone: its Sketch and its Extrude.
        expect(len(d.call("get_state")["features"]) == 2,
               "and a refused $ref leaves nothing behind")
        d.call("set_active_part", part=first_part)

        # ---- A query inside a batch sees CURRENT geometry ---------------
        # The deferral is not allowed to answer from stale bodies: a
        # non-deferrable call flushes the pending regeneration first.
        r = d.call("batch", calls=[
            {"method": "add_feature", "params": {"feature": {
                "type": "Box", "name": "Slab", "on": "Top",
                "centered": False, "offset": [50, 0],
                "width": 4, "depth": 4, "height": 4}}},
            {"method": "mass_properties", "params": {}},
            {"method": "add_feature", "params": {"feature": {
                "type": "Box", "name": "Slab2", "on": "Top",
                "centered": False, "offset": [70, 0],
                "width": 2, "depth": 2, "height": 2}}}])
        vol = r["results"][1]["result"]["total"]["volume_mm3"]
        expect(abs(vol - (1000.0 + 64.0)) < 1e-6,
               f"the query saw the box the same batch had just added, and "
               f"NOT the one added after it: {vol}")
        # Two: the flush the query forced, and the batch's own at the end.
        # The count is reported so an agent can see what its ordering cost.
        expect(r["regenerations"] == 2,
               f"a query mid-batch costs a regeneration, reported: "
               f"{r['regenerations']}")
        d.call("undo")
        expect(len(d.call("get_state")["features"]) == len(before),
               "and that batch undoes as one step too")

        # ---- An assembly batch is one snapshot -------------------------
        d.call("add_part", name="cube")
        box(d, (0, 0, 0), (5, 5, 5), "C")
        d.call("add_assembly", name="rig")
        r = d.call("batch", calls=[
            {"method": "add_occurrence", "params": {"part": "cube",
                                                    "fixed": True}},
            {"method": "add_occurrence", "params": {
                "part": "cube", "transform": {"position": [20, 0, 0],
                                              "orientation": [0, 0, 0, 1]}}},
            {"method": "add_occurrence", "params": {
                "part": "cube", "transform": {"position": [40, 0, 0],
                                              "orientation": [0, 0, 0, 1]}}}])
        expect(len(r["results"]) == 3, f"three placed: {r}")
        expect(len(d.call("list_occurrences")["occurrences"]) == 3,
               "three occurrences")
        d.call("undo")
        expect(len(d.call("list_occurrences")["occurrences"]) == 0,
               "one undo took all three back")

        # A failed assembly batch restores the assembly.
        d.call("add_occurrence", part="cube", fixed=True)
        refused(d, "batch", ["rolled back", "assembly restored"], calls=[
            {"method": "add_occurrence", "params": {"part": "cube"}},
            {"method": "add_occurrence", "params": {"part": "no-such-part"}}])
        expect(len(d.call("list_occurrences")["occurrences"]) == 1,
               "the assembly is as it was before the batch")
        print("batch_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
