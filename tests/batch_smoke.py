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


def never_in_a_batch(d):
    """What a rollback cannot take back is refused before the batch runs.

    A batch rolls back the undo histories it marked. A call that creates or
    switches a tab leaves the marked tab's history stashed where the rollback
    does not reach it, and an edit no history records (units, the rollback
    bar, a tab's name, a folder, a git remote) has nothing to roll back: a
    batch that failed after one left it in place while saying the document
    was as it was. So each is refused before the first call runs, naming it
    (docs/scripting/rpc.md#batch).
    """
    cam = d.call("ping").get("features", {}).get("cam", True)
    d.call("set_active_part", part="rig")

    def world():
        # Every tab's revision (an assembly's occurrences included), and the
        # tabs themselves: which exist, which is active, their folders.
        return d.call("validation_revision"), d.call("list_parts")

    def refused_whole(calls, needles):
        before = world()
        err = refused(d, "batch", needles + ["unchanged"], calls=calls)
        expect(world() == before,
               f"a batch refused for {needles} changed nothing: {err}")
        return err

    never = ["add_part", "add_assembly", "create_drawing", "edit_in_context",
             "add_fastener", "add_bearing", "add_retaining_ring",
             "fit_fastener", "fit_bearing", "fit_retaining_ring",
             "set_units", "set_tube_rules", "set_rollback", "rename_part",
             "create_folder", "move_to_folder", "rename_folder",
             "dissolve_folder", "remote_add", "remote_remove",
             "set_motion_pose", "set_section", "add_section",
             "update_section", "remove_section"]
    if cam:
        never.append("machine_part")
    for method in never:
        # A call that would have run first, so "unchanged" means the refusal
        # came before ANY call, not merely before this one.
        refused_whole([{"method": "add_occurrence",
                        "params": {"part": "cube"}},
                       {"method": method, "params": {}}],
                      ["batch call 2", f"'{method}'", "on its own"])

    # ---- A verb given ANOTHER tab switches to it: refused; the active one
    # stays batchable. By name and by id.
    d.call("create_drawing", name="D1")
    d1 = d.call("list_parts")["active_id"]
    d.call("create_drawing", name="D2")
    for other in ("D1", d1):
        refused_whole([{"method": "check_drawing",
                        "params": {"drawing": other, "update": False}}],
                      ["batch call 1", "'check_drawing'", "D1",
                       "not the active tab"])
    r = d.call("batch", calls=[{"method": "check_drawing",
                                "params": {"drawing": "D2",
                                           "update": False}}])
    expect("failed" not in r, f"the active drawing by name is fine: {r}")
    # A $ref is resolved when its call runs, and checked then.
    r = d.call("batch", calls=[
        {"method": "list_parts", "params": {}},
        {"method": "check_drawing", "params": {
            "drawing": {"$ref": 1, "key": "active_id"}, "update": False}}])
    expect("failed" not in r, f"a $ref to the active drawing is fine: {r}")
    if cam:
        d.call("set_active_part", part="cube")
        job = d.call("machine_part", part="cube", name="J")
        expect(d.call("list_parts")["active_id"] == job["id"],
               f"machine_part switched to its job: {job}")
        r = d.call("batch", calls=[{"method": "list_cam_setups",
                                    "params": {"job": "J"}}])
        expect("failed" not in r, f"the active job by name is fine: {r}")
        d.call("set_active_part", part="D2")
        refused_whole([{"method": "list_cam_setups",
                        "params": {"job": "J"}}],
                      ["'list_cam_setups'", "not the active tab"])

    # ---- A motion run told to leave the assembly at a pose: the pose is
    # written with no snapshot. Without one the run is made on a copy.
    for key, value in (("leave_at", "last"), ("leave_at_deg", 30.0),
                       ("leave_at_mm", 5.0)):
        refused_whole([{"method": "run_motion_study",
                        "params": {"study": 1, key: value}}],
                      ["'run_motion_study'", key])

    # ---- A component claimed or updated on another part's tab ------------
    d.call("set_active_part", part="rig")
    cube = next(t for t in d.call("list_parts")["parts"]
                if t["name"] == "cube")
    refused_whole([{"method": "claim_component", "params": {
        "part_id": cube["id"], "namespace": "n", "key": "k",
        "fingerprint": "f", "expected_revision": "r"}}],
        ["'claim_component'", "not the active tab"])
    refused_whole([{"method": "update_component", "params": {
        "part_id": cube["id"], "source_part_id": 1, "fingerprint": "f",
        "expected_revision": "r", "source_revision": "r", "apply": True}}],
        ["'update_component'", "not the active tab"])
    # Without `apply` it is a preview, which switches nothing: whatever it
    # says, it is not the batch's refusal.
    try:
        d.call("batch", calls=[{"method": "update_component", "params": {
            "part_id": cube["id"], "source_part_id": 1, "fingerprint": "f",
            "expected_revision": "r", "source_revision": "r"}}])
    except RuntimeError as e:
        expect("not the active tab" not in str(e),
               f"a preview is not refused for its tab: {e}")

    # ---- A part-scope rename_variable: the requirements it rewrites are
    # not on the part's stack. Refused when the scope is stated, and when it
    # is only known once an earlier call of the batch has run.
    d.call("set_active_part", part="cube")
    d.call("set_variable", name="w", expression="3", scope="part")
    refused_whole([{"method": "rename_variable",
                    "params": {"name": "w", "new_name": "w2",
                               "scope": "part"}}],
                  ["'rename_variable'", "part"])
    refused_whole([{"method": "rename_variable",
                    "params": {"name": "w", "new_name": "w2"}}],
                  ["'rename_variable'", "part"])
    names = {v["name"] for v in d.call("list_variables")["variables"]}
    err = refused(d, "batch", ["rename_variable", "rolled back"], calls=[
        {"method": "set_variable",
         "params": {"name": "late", "expression": "1", "scope": "part"}},
        {"method": "rename_variable",
         "params": {"name": "late", "new_name": "late2"}}])
    expect({v["name"] for v in d.call("list_variables")["variables"]}
           == names, f"and the batch is rolled back whole: {err}")
    # The project scope's rename is on the project history: batchable.
    d.call("set_variable", name="pv", expression="2")
    r = d.call("batch", calls=[{"method": "rename_variable",
                                "params": {"name": "pv", "new_name": "pv2",
                                           "scope": "project"}}])
    expect("failed" not in r, f"a project rename runs in a batch: {r}")


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
             "params": {"path": os.path.join(tmp, "x.lob")}}])
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

        # ---- A rollback takes back the PROJECT scope too ---------------
        # A project variable or table lives outside the part's command
        # stack, on the cross-tab project history. A refused batch used to
        # leave one it had set in place while its reply said nothing had
        # changed (docs/scripting/rpc.md#a-refused-call-changes-nothing).
        def project_scope():
            return ({v["name"]: v["expression"]
                     for v in d.call("list_variables")["variables"]
                     if v["scope"] == "project"},
                    [t["name"] for t in d.call("list_tables")["tables"]])

        d.call("set_variable", name="keep", expression="1")
        d.call("set_variable", name="later", expression="7")
        d.call("undo")  # `later` waits on redo
        d.call("save_project", path=os.path.join(tmp, "vars.lob"))
        scope_before = project_scope()
        # ...and an edit that names ANOTHER part lands on that tab's own
        # stashed stack, which the rollback has to reach as well.
        print_before = d.call("get_print_settings", part="drill")["part"]
        err = refused(d, "batch", ["rolled back", "project variables"],
                      calls=[
            {"method": "set_variable",
             "params": {"name": "leak", "expression": "5"}},
            {"method": "add_feature", "params": {"feature": {
                "type": "Sketch", "name": "Interleaved", "plane": plane(),
                "entities": [{"type": "circle", "center": [0, 0],
                              "radius": 1.0}]}}},
            {"method": "set_variable",
             "params": {"name": "keep", "expression": "2"}},
            {"method": "set_print_settings",
             "params": {"part": "drill", "walls": 9}},
            {"method": "set_table", "params": {
                "name": "sizes", "columns": ["d"],
                "rows": [{"key": 1, "values": [3]}]}},
            {"method": "delete_feature", "params": {"feature_id": 999999}}])
        print(f"  project rollback says: {err[:160]}...")
        expect(project_scope() == scope_before,
               f"the project's variables and tables are as they were: "
               f"{project_scope()} vs {scope_before}")
        expect(d.call("get_print_settings", part="drill")["part"]
               == print_before,
               f"the other part's slicer settings are as they were: "
               f"{d.call('get_print_settings', part='drill')['part']}")
        expect(all(f["name"] != "Interleaved"
                   for f in d.call("get_state")["features"]),
               "the part edit between them went too")
        expect(not d.call("ping")["dirty"],
               "a saved project reads saved after the rollback")
        # The history is as it was: the redo the batch's first edit cleared
        # is back, and the rollback left no undo step of its own behind.
        d.call("redo")
        expect(project_scope()[0].get("later") == "7",
               f"redo still restores `later`: {project_scope()}")
        d.call("undo")
        d.call("undo")
        expect("keep" not in project_scope()[0],
               f"the next undo is `keep`'s own, not a stray step left by "
               f"the rollback: {project_scope()}")

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

        never_in_a_batch(d)
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
