#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A refused call changes nothing.

`cam_post {setup, path, _timeout}` wrote its G-code and THEN answered
"unknown parameter '_timeout'": the unknown-key check ran after the handler,
because a method's vocabulary was learned by which keys it read. A caller
told "error" retries, and the file it was told was not written is on disk
twice over. The rule (docs/scripting/rpc.md#a-refused-call-changes-nothing)
is now held three ways, and this test holds each of them:

  1. Every method, an unknown key: refused by the DISPATCHER against the
     method's describe row, before any handler runs -- so before a required
     key is missed, before a tab gate, before a file is opened.
  2. Every method, every key of the wrong type, on each kind of tab: a
     refusal must leave the world as it was.
  3. The calls that DO reach outside the document -- files, tabs, settings,
     the machine store -- from a VALID baseline, with each of their keys
     made wrong in turn, and an extra key; plus the edits the single-call
     rollback takes back (part, assembly, job) and the late checks this test
     was written against, each named.

"The world" is: the saved project (every tab), the tab list and the active
tab, undo/redo, the files under the app's scratch dirs and its cwd, the
settings file, the machine store and the background jobs. The project is
saved clean before each call, so `ping`'s `dirty` says whether ANY document
moved without a save per call.

Usage: refused_calls_smoke.py <path-to-designer-binary>
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from smoke_app import launch  # noqa: E402
from smoke_shapes import cylinder  # noqa: E402
from smoke_world import (World, build_fixture, changed,  # noqa: E402
                         outside_baselines)

BOGUS = "__not_a_parameter__"
BAD = {"__bad__": True}  # an object: no scalar key accepts it


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def call(d, method, params):
    try:
        return True, d.request(method, params, timeout=120)
    except Exception as e:  # noqa: BLE001 — the message is the result
        return False, str(e)


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="designer_refused_")
    os.makedirs(os.path.join(work, "fp"))
    out = os.path.join(work, "out")
    os.makedirs(out)
    # The app inherits this as its cwd, so a file written to a relative path
    # (render_still's default render.png) lands where the world can see it.
    os.chdir(work)
    problems = []
    try:
        with launch(binary) as app:
            d = app.designer
            cam = d.call("ping").get("features", {}).get("cam", True)
            part, occ, setup = build_fixture(d, cam)
            world = World(d, app, work)
            methods = d.call("describe")["methods"]
            tabs = [part["name"], "Asm", "Dwg"] + (["Job"] if cam else [])

            def note(cond, msg):
                if not cond:
                    problems.append(msg)

            def check(label, method, params, must_refuse=True,
                      needle=None, tab=None):
                """Call; a refusal must change nothing. Returns ok."""
                if tab is not None:
                    d.call("set_active_part", part=tab)
                world.settle()
                before = world.cheap()
                ok, said = call(d, method, params)
                after = world.cheap()
                if ok:
                    if must_refuse:
                        problems.append(f"{label}: {method} {params} was "
                                        f"ACCEPTED; it should be refused")
                    return ok
                if needle and needle not in said:
                    problems.append(f"{label}: {method} refused without "
                                    f"'{needle}': {said[:300]}")
                diff = changed(before, after)
                if diff:
                    problems.append(f"{label}: {method} {params} was REFUSED "
                                    f"({said[:160]}) but changed: "
                                    f"{'; '.join(diff)}")
                return ok

            # --- 1. Every method, an unknown key --------------------------
            # The refusal must name the key, and must come from the row: a
            # method with a required key would otherwise answer "missing
            # required parameter" first, and a part-gated one "switch to a
            # part tab", because a handler (or the gate) ran before the
            # check. That it names THIS key on an assembly tab, for every
            # method, is what "before anything runs" looks like from outside.
            d.call("set_active_part", part="Asm")
            world.settle()
            before = world.cheap()
            swept = 0
            for m in methods:
                if m.get("params_free_form"):
                    continue
                try:
                    d.request(m["name"], {BOGUS: 1}, timeout=120)
                    ok, said, contract = True, "", None
                except Exception as e:  # noqa: BLE001
                    ok, said = False, str(e)
                    contract = getattr(e, "diagnostic", None)
                swept += 1
                # A method with an inspection contract refuses through it,
                # also before the handler, with a structured diagnostic.
                by_contract = bool(contract) and " at /params" in said
                if ok or (f"unknown parameter '{BOGUS}'" not in said
                          and not by_contract):
                    problems.append(
                        f"unknown key: {m['name']} answered "
                        f"{'OK' if ok else said[:200]!r} -- the dispatcher "
                        f"must refuse a key outside the row before the "
                        f"handler runs")
                diff = changed(before, world.cheap())
                if diff:
                    how = "ran" if ok else "was refused"
                    problems.append(f"unknown key: {m['name']} {how} and "
                                    f"changed: {'; '.join(diff)}")
                    # It ran (new_project, delete_part...): start again.
                    part, occ, setup = build_fixture(d, cam)
                    d.call("set_active_part", part="Asm")
                    world.settle()
                    before = world.cheap()
            # A free-form method checks its own keys, before applying any.
            check("free-form", "set_settings", {BOGUS: 1}, needle=BOGUS)
            check("free-form", "set_settings",
                  {"ui_font_scale": 1.25, BOGUS: 1}, needle=BOGUS)

            # --- 2. Every key of the wrong type, on every tab -------------
            typed = 0
            for tab in tabs:
                d.call("set_active_part", part=tab)
                world.settle()
                before = world.cheap()
                for m in methods:
                    keys = m.get("params") or []
                    if m.get("params_free_form") or not keys:
                        continue
                    ok, said = call(d, m["name"], {k: BAD for k in keys})
                    typed += 1
                    now = world.cheap()
                    diff = changed(before, now)
                    if diff and not ok:
                        problems.append(
                            f"[{tab}] {m['name']} refused ({said[:160]}) "
                            f"but changed: {'; '.join(diff)}")
                    if ok or diff:
                        # Accepted: whatever it did is its answer. Put the
                        # tab back and start from here.
                        d.call("set_active_part", part=tab)
                        world.settle()
                        now = world.cheap()
                    before = now

            # --- 3. Calls that reach outside the document -----------------
            # A VALID baseline each, then every key of it made wrong in turn,
            # every other key of the row added wrong, and an extra key. The
            # baseline itself is never run here: what is checked is that a
            # refusal of each variant writes nothing and moves nothing.
            def path(name):
                return os.path.join(out, name)

            def make_baselines():
                return outside_baselines(part, setup, out, cam)

            baselines = make_baselines()
            fixture_tabs = [p["name"] for p in d.call("list_parts")["parts"]]
            rows = {m["name"]: m for m in methods}
            variants = 0
            for i in range(len(baselines)):
                tab, method, base = baselines[i]
                row = rows[method]
                keys = row.get("params") or []
                cases = [({**base, BOGUS: 1}, BOGUS)]
                for k in keys:
                    v = base.get(k)
                    wrong = (123 if isinstance(v, str) else
                             "not a number" if isinstance(v, (int, float))
                             else BAD)
                    cases.append(({**base, k: wrong}, None))
                for params, needle in cases:
                    variants += 1
                    check(f"baseline {method}", method, params,
                          must_refuse=False, needle=needle, tab=tab)
                    # A variant that was accepted did its work for real (a
                    # tab made, renamed or deleted): start from a fresh
                    # fixture, so the next variant is judged on its own.
                    shutil.rmtree(out, ignore_errors=True)
                    os.makedirs(out)
                    tabs_now = [p["name"] for p in
                                d.call("list_parts")["parts"]]
                    if tabs_now != fixture_tabs:
                        part, occ, setup = build_fixture(d, cam)
                        baselines = make_baselines()
                        tab, method, base = baselines[i]
            # The baselines must be the real thing, or the variants above
            # proved nothing: each one, as written, is accepted.
            for tab, method, base in baselines:
                if method in ("delete_part", "set_active_part"):
                    continue  # they would rearrange what the rest relies on
                d.call("set_active_part", part=tab)
                ok, said = call(d, method, base)
                expect(ok, f"baseline {method} {base} is not a valid call: "
                           f"{said}")
                if method == "rename_part":
                    d.call("rename_part", part="Dwg 2", new_name="Dwg")

            # --- The late checks this was written against, by name -------
            if cam:
                nc = path("late.nc")
                check("cam_post _timeout", "cam_post",
                      {"setup": setup, "path": nc, "_timeout": 30},
                      needle="unknown parameter '_timeout'", tab="Job")
                note(not os.path.exists(nc),
                     "cam_post wrote its file and then refused the call")
            check("export_step deflection", "export_step",
                  {"path": path("late.step"), "deflection": 0.1},
                  needle="unknown parameter 'deflection'", tab=part["name"])
            check("save_project message", "save_project",
                  {"path": path("late.designer"), "message": 5},
                  tab=part["name"])
            check("compare_geometry second path", "compare_geometry",
                  {"a": 0, "b": 1, "missing_path": path("late-m.brep"),
                   "extra_path": 5}, tab=part["name"])
            check("compare_geometry materialize", "compare_geometry",
                  {"a": occ["id"], "b": occ["id"],
                   "missing_path": path("late-m.brep"),
                   "materialize": True}, needle="part tab", tab="Asm")
            check("set_keybinding reset+chord", "set_keybinding",
                  {"command": "part.fillet", "reset": True,
                   "chord": "Shift+B"}, needle="pass one", tab=part["name"])
            check("set_active_part twice", "set_active_part",
                  {"part": "Asm", "index": 0}, needle="once",
                  tab=part["name"])
            check("delete_part twice", "delete_part",
                  {"part": "Dwg", "index": 0}, needle="once",
                  tab=part["name"])
            check("delete_variable scope", "delete_variable",
                  {"name": "nope", "scope": "projct"}, needle="scope",
                  tab=part["name"])

            # A table thread with another pitch is refused, never re-pitched
            # to the table's (docs/modeling/holes-and-threads.md#thread) --
            # added or modified, before anything is committed.
            d.call("set_active_part", part=part["name"])
            cylinder(d, (80, 0, 0), "z", 3.0, 12.0, name="M6 boss")
            face = next(
                {"feature_id": b["created_by"], "element": f["name"]}
                for b in d.call("list_elements")["bodies"]
                for f in b["faces"]
                if f["kind"] == "cylinder" and abs(f["radius"] - 3.0) < 1e-6)
            m6 = {"type": "Thread", "name": "M6", "face": face,
                  "designation": "M6"}
            check("thread pitch on add", "add_feature",
                  {"feature": dict(m6, pitch=1.25)},
                  needle="accepted: pitch 1 ", tab=part["name"])
            check("custom thread without a pitch", "add_feature",
                  {"feature": dict(m6, designation="boss thread")},
                  needle="needs its pitch", tab=part["name"])
            tid = d.call("add_feature", feature=m6)["id"]
            check("thread pitch on modify", "modify_feature",
                  {"feature_id": tid, "feature": dict(m6, pitch=1.25)},
                  needle="accepted: pitch 1 ", tab=part["name"])
            check("thread fine pitch named", "modify_feature",
                  {"feature_id": tid,
                   "feature": dict(m6, designation="M8", pitch=1.0)},
                  needle="M8x1", tab=part["name"])

            # The single-call rollback: a handler that edited and THEN
            # refused leaves nothing behind -- part, job.
            d.call("set_active_part", part=part["name"])
            feats = d.call("get_state")["features"]
            last = feats[-1]["id"]
            check("part rollback", "delete_feature",
                  {"feature_id": last, "index": 0},
                  needle="rolled back", tab=part["name"])
            note(d.call("get_state")["features"][-1]["id"] == last,
                 "delete_feature deleted a feature under a refusal")
            # An assembly edit that snapshotted before reading its key.
            check("assembly rollback", "remove_occurrence",
                  {"id": "not an id"}, needle="rolled back", tab="Asm")
            if cam:
                check("job rollback", "set_cam_setup",
                      {"id": setup, "name": "Renamed",
                       "post": "no-such-post"},
                      needle="rolled back", tab="Job")
                names = [s["name"] for s in
                         d.call("list_cam_setups")["setups"]]
                note("Renamed" not in names,
                     f"set_cam_setup renamed the setup under a refusal: "
                     f"{names}")

            if problems:
                print("refused_calls_smoke: FAILED")
                for p in problems:
                    print("  - " + p)
                return 1
            print(f"refused_calls_smoke: {swept} methods refuse an unknown "
                  f"key first; {typed} wrong-type calls over {len(tabs)} "
                  f"tabs, {variants} variants of {len(baselines)} "
                  f"outside-the-document calls -- every refusal changed "
                  f"nothing")
            return 0
    finally:
        os.chdir(HERE)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
