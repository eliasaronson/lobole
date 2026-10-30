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
from smoke_shapes import cylinder, mesh_sphere, sphere  # noqa: E402
from smoke_world import (World, build_fixture, changed,  # noqa: E402
                         outside_baselines)

BOGUS = "__not_a_parameter__"
BAD = {"__bad__": True}  # an object: no scalar key accepts it


# A stand-in ccx (docs/simulation/README.md#nonlinear-static): it converges
# one increment, to 40 % of the load, and then stops converging, saying so
# as ccx does.
STALLING_CCX = r"""#!/bin/sh
if [ "$1" = "-v" ]; then echo; echo 'This is Version 2.23'; exit 201; fi
nodes=$(awk '/^\*NODE, NSET=NALL/{on=1;next} /^\*/{on=0} on{sub(/,.*/,"");print}' job.inp)
sup=$(awk '/^\*NSET, NSET=NSUP/{on=1;next} /^\*/{on=0} on{gsub(/,/," ");print}' job.inp)
{ echo; echo ' displacements (vx,vy,vz) for set NALL and time  4.0000000E-01'
  echo; for n in $nodes; do echo "  $n  0.0E+00  0.0E+00  0.0E+00"; done; echo
  echo ' forces (fx,fy,fz) for set NSUP and time  4.0000000E-01'
  echo; for n in $sup; do echo "  $n  0.0E+00  0.0E+00  0.0E+00"; done
  echo; } > job.dat
printf '  STEP INC ATT ITRS TOT STEP INC\n     1  1  1  3  4.0E-01  4.0E-01  4.0E-01\n' > job.sta
echo ' *ERROR: increment size smaller than minimum'
exit 201
"""


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
            d = app.client
            features = d.call("ping").get("features", {})
            cam = features.get("cam", True)
            # Without a mesher a run is refused before ccx would start.
            mesher = features.get("mesher", True)
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
            # Renamed to check_print's name for it: refused, naming it.
            check("export_stl deflection", "export_stl",
                  {"path": path("late.stl"), "deflection": 0.1},
                  needle="'deflection' (renamed: it is 'stl_deflection_mm' "
                         "now)", tab=part["name"])
            check("save_project message", "save_project",
                  {"path": path("late"), "message": 5},
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

            # A section into a sketch
            # (docs/sketching/README.md#a-section-into-a-sketch): every
            # parameter checked before the sketch is touched, and a plane
            # that cuts nothing rolled back.
            d.call("set_active_part", part=part["name"])
            probe = d.call("add_feature", feature={
                "type": "Sketch", "name": "Section probe",
                "plane": {"origin": [0, 0, 500], "normal": [0, 0, 1],
                          "uAxis": [1, 0, 0]}, "entities": []})["id"]
            for label, params, needle in [
                    ("project_section unknown fit",
                     {"sketch_id": probe, "fit": "smooth"}, "smooth"),
                    ("project_section zero tolerance",
                     {"sketch_id": probe, "tolerance_mm": 0},
                     "tolerance_mm"),
                    ("project_section unknown body",
                     {"sketch_id": probe, "bodies": ["Nope"]}, "Nope"),
                    ("project_section a plane that cuts nothing",
                     {"sketch_id": probe}, "cuts none")]:
                check(label, "project_section", params, needle=needle,
                      tab=part["name"])
            d.call("delete_feature", feature_id=probe)

            # Imports (docs/interchange/README.md#up-axis,
            # docs/sketching/README.md#importing-dxf-and-svg): every one read
            # and checked before a feature, a sketch or a tab exists.
            fx = os.path.join(HERE, "fixtures", "import")
            glb = os.path.join(work, "refuse-units.glb")
            mesh_sphere(glb)
            for label, method, params, needle in [
                    ("import_step unknown up_axis", "import_step",
                     {"path": os.path.join(fx, "box-solidworks.step"),
                      "up_axis": "y"}, "Accepted: auto, none, +x"),
                    ("import_step units on IGES", "import_step",
                     {"path": os.path.join(fx, "box-solid.igs"),
                      "units": "in"}, "IGES, which declares its own unit"),
                    ("import_step units on glTF", "import_step",
                     {"path": glb, "units": "m"},
                     "metres by its specification"),
                    ("import_step a drawing", "import_step",
                     {"path": os.path.join(fx, "unitless.dxf")},
                     "import_sketch"),
                    ("import_step a mirror for a scale", "import_step",
                     {"path": os.path.join(fx, "box-solidworks.step"),
                      "scale": -1}, "greater than 0"),
                    ("import_step an unknown placement key", "import_step",
                     {"path": os.path.join(fx, "box-solidworks.step"),
                      "placement": {"move_mm": [0, 0, 1]}},
                     "translate_mm and rotate_deg"),
                    ("import_sketch unit contradicting $INSUNITS",
                     "import_sketch",
                     {"path": os.path.join(fx, "plate-ezdxf.dxf"),
                      "units": "mm"}, "declares $INSUNITS 1"),
                    ("import_sketch rational spline", "import_sketch",
                     {"path": os.path.join(fx, "refuse-rational-spline.dxf")},
                     "rational spline"),
                    ("import_sketch unknown layer", "import_sketch",
                     {"path": os.path.join(fx, "plate-ezdxf.dxf"),
                      "layers": ["OUTLINE", "NOPE"]},
                     "no layer 'NOPE'. Its layers:"),
                    ("import_sketch clip-path", "import_sketch",
                     {"path": os.path.join(fx, "refuse-clip.svg")},
                     "is clipped"),
                    ("import_sketch sketch_id and plane", "import_sketch",
                     {"path": os.path.join(fx, "unitless.dxf"),
                      "sketch_id": 1, "plane": "Top"}, "one or the other")]:
                check(label, method, params, needle=needle,
                      tab=part["name"])

            # Aligning an import, refused on its inputs and on the geometry
            # before the placement is written
            # (docs/interchange/README.md#placing-an-import). The import is
            # removed again after.
            d.call("set_active_part", part=part["name"])
            got = d.call("import_step",
                         path=os.path.join(fx, "box-solidworks.step"))
            note(not got.get("regen", {}).get("errors"),
                 f"align fixture import failed: {got}")
            imp = d.call("get_state")["features"][-1]["id"]
            for label, params, needle in [
                    ("align_import unknown mode",
                     {"mode": "flor", "point_mm": [0, 0, 0]},
                     "floor, three_points, principal, scale"),
                    ("align_import distance on a floor",
                     {"mode": "floor", "point_mm": [0, 0, 0],
                      "distance_mm": 10}, "distance_mm is scale's"),
                    ("align_import a point off the import",
                     {"mode": "floor", "point_mm": [5000, 0, 0]},
                     "nearest surface"),
                    ("align_import three points in a line",
                     {"mode": "three_points",
                      "points_mm": [[0, 0, 0], [1, 0, 0], [2, 0, 0]]},
                     "in a line"),
                    ("align_import two points for three",
                     {"mode": "three_points",
                      "points_mm": [[0, 0, 0], [1, 0, 0]]},
                     "three_points takes three"),
                    ("align_import scale without a distance",
                     {"mode": "scale", "points_mm": [[0, 0, 0], [1, 0, 0]]},
                     "distance_mm")]:
                check(label, "align_import", dict(params, feature_id=imp),
                      needle=needle, tab=part["name"])
            d.call("delete_feature", feature_id=imp)

            # An expression requirement and a design sweep
            # (docs/requirements/README.md#expression, #design-sweep): every
            # refusal before anything is stored or started.
            d.call("set_active_part", part=part["name"])
            d.call("set_variable", name="seat_od", expression="22")
            d.call("set_variable", name="seat_bore", expression="15")
            d.call("set_variable", name="unread", expression="3")
            d.call("add_requirement", name="wall",
                   measure={"kind": "expression",
                            "expression": "seat_od - seat_bore"},
                   bound={"kind": "within", "min": 4, "max": 12})

            def expr_req(expression, **measure):
                return {"name": "x",
                        "measure": dict(kind="expression",
                                        expression=expression, **measure),
                        "bound": {"kind": "at_least", "value": 1}}
            check("expression unknown name", "add_requirement",
                  expr_req("seat_od - seat_bor"),
                  needle="not a variable of the project (its variables:",
                  tab=part["name"])
            check("expression malformed", "add_requirement",
                  expr_req("seat_od -"), needle="does not read",
                  tab=part["name"])
            check("expression configuration without a part",
                  "add_requirement", expr_req("seat_od", configuration=0),
                  needle="needs `document`", tab=part["name"])
            check("expression configuration the part lacks",
                  "add_requirement",
                  expr_req("seat_od", document=part["id"], configuration=99),
                  needle="its configurations: 0 (base)", tab=part["name"])
            check("delete a variable a requirement reads", "delete_variable",
                  {"name": "seat_bore"}, needle="requirement 'wall'",
                  tab=part["name"])
            span = {"name": "seat_bore", "min": 10, "max": 20}
            for label, params, needle in [
                    ("not in scope",
                     {"variables": [dict(span, name="seat_bor")]},
                     "variables in scope:"),
                    ("nothing reads it",
                     {"variables": [dict(span, name="unread")]},
                     "no part reads 'unread'"),
                    ("min above max",
                     {"variables": [dict(span, min=30)]}, "is above max"),
                    ("an unknown key in a variable",
                     {"variables": [dict(span, step=1)]},
                     "unknown key 'step'"),
                    ("samples", {"variables": [span], "samples": 99},
                     "`samples` is a whole number from 2 to 33"),
                    ("refine", {"variables": [span], "refine": 9},
                     "`refine` is a whole number from 0 to 8"),
                    ("over the cap",
                     {"variables": [span], "samples": 33, "max_points": 10},
                     "would run 33 points, over the cap of 10"),
                    ("a cap over the ceiling",
                     {"variables": [span], "max_points": 1000},
                     "`max_points` is a whole number from 1 to 256"),
                    ("documents not a part",
                     {"variables": [span], "documents": [occ["id"] + 999]},
                     "documents:"),
                    ("an assembly as a document",
                     {"variables": [span],
                      "documents": [next(p["id"] for p in
                                         d.call("list_parts")["parts"]
                                         if p["name"] == "Asm")]},
                     "this measure reads a part"),
                    ("no variables", {}, "needs `variables`")]:
                check(f"design_sweep {label}", "design_sweep", params,
                      needle=needle, tab=part["name"])
                check(f"design_sweep {label} (async)", "design_sweep",
                      dict(params, **{"async": True}), needle=needle,
                      tab=part["name"])
            check("design_sweep_result of no sweep", "design_sweep_result",
                  {"job": 987654}, needle="no design sweep 987654",
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
            # A designation that claims a series and is not a row is refused
            # the same way, before it is stored -- not kept as a feature
            # error that get_print_process then read as "external, Ø0".
            # Both spellings: "M8 printed" was stored and "M8 printed nut"
            # refused by one path, each the other way round by another.
            for bad in ("M6 printed", "M6 printed nut"):
                check(f"thread designation {bad!r}", "add_feature",
                      {"feature": dict(m6, designation=bad)},
                      needle="not in the ISO metric thread table",
                      tab=part["name"])
            # A Cradle (docs/modeling/README.md#cradle) refused on its
            # inputs before it is stored: the boss as the block, a ball
            # inside it as the scan (removed again after).
            boss = d.call("get_state")["bodies"][-1]["created_by"]
            ball = sphere(d, (80, 0, 6), 2.0, name="Cradle scan")
            scan = d.call("get_state")["bodies"][-1]["created_by"]
            cradle = {"type": "Cradle", "target_id": boss, "tool_id": scan,
                      "clearance_mm": 0.5}
            check("cradle direction zero", "add_feature",
                  {"feature": dict(cradle, direction=[0, 0, 0])},
                  needle="non-zero", tab=part["name"])
            check("cradle past the budget", "add_feature",
                  {"feature": dict(cradle, resolution_mm=1e-4)},
                  needle="cost budget", tab=part["name"])
            check("cradle sweep not a bool", "add_feature",
                  {"feature": dict(cradle, sweep="no")},
                  needle="sweep", tab=part["name"])
            for fid in ball.get("ids", [ball["id"]]):  # the solid first
                d.call("delete_feature", feature_id=fid)
            tid = d.call("add_feature", feature=m6)["id"]
            check("thread designation on modify", "modify_feature",
                  {"feature_id": tid,
                   "feature": dict(m6, designation="M6 printed")},
                  needle="not in the ISO metric thread table",
                  tab=part["name"])
            # A material on a feature that creates no body sets nothing:
            # refused, naming the feature that made the body it modifies
            # (the clock weighed every body null this way).
            boss = face["feature_id"]
            bore = cylinder(d, (80, 0, 6), "z", 1.0, 10.0, name="M6 bore",
                            mode="cut")["id"]
            check("set_material on a cut", "set_material",
                  {"feature_id": bore, "material": "steel"},
                  needle=f"It modifies 'M6 boss', made by feature_id {boss}",
                  tab=part["name"])
            check("set_material on a thread", "set_material",
                  {"feature_id": tid, "material": "steel"},
                  needle="created no body", tab=part["name"])
            check("set_appearance on a thread", "set_appearance",
                  {"feature_id": tid, "color": [0.5, 0.5, 0.5]},
                  needle="created no body", tab=part["name"])
            check("thread pitch on modify", "modify_feature",
                  {"feature_id": tid, "feature": dict(m6, pitch=1.25)},
                  needle="accepted: pitch 1 ", tab=part["name"])
            check("thread fine pitch named", "modify_feature",
                  {"feature_id": tid,
                   "feature": dict(m6, designation="M8", pitch=1.0)},
                  needle="M8x1", tab=part["name"])

            # What the bodies refuse (Feature::WhyRefusedOn), before the
            # feature is committed (docs/modeling/README.md#refused-on-the-bodies):
            # an element named by the feature that MADE it rather than the
            # one that created its body, and a pattern of a feature that
            # made no body.
            d.call("set_active_part", part=part["name"])
            plate = next(b for b in d.call("list_elements")["bodies"]
                         if b["name"] == "Plate")
            rim = next(e["name"] for e in plate["edges"]
                       if not e.get("smooth"))
            rounded = d.call("add_feature", feature={
                "type": "Fillet", "name": "Round",
                "edges": [{"feature_id": plate["created_by"],
                           "element": rim}], "radius": 1.0})["id"]
            plate = next(b for b in d.call("list_elements")["bodies"]
                         if b["name"] == "Plate")
            by_round = next(e["name"] for e in plate["edges"]
                            if e.get("made_by") == rounded
                            and not e.get("smooth"))
            check("fillet named by made_by", "add_feature",
                  {"feature": {"type": "Fillet", "name": "Again",
                               "edges": [{"feature_id": rounded,
                                          "element": by_round}],
                               "radius": 0.5}},
                  needle=f"reference it as {{feature_id: "
                         f"{plate['created_by']}}}", tab=part["name"])
            check("pattern of a fillet", "add_feature",
                  {"feature": {"type": "Pattern", "name": "Row",
                               "kind": "linear", "source_id": rounded,
                               "count": 2, "spacing": 100,
                               "axis": {"kind": "x"}}},
                  needle=f"source_id: {plate['created_by']}",
                  tab=part["name"])

            # A section cut on a datum that is not there: add_section added
            # the cut and update_section applied base_plane, each before
            # refusing the plane_id. Both now check first.
            d.call("set_active_part", part=part["name"])

            def sections():
                return d.call("list_sections")["sections"]
            cuts = sections()
            check("add_section on no datum", "add_section",
                  {"base_plane": "xz", "plane_id": 987654},
                  needle="no feature with that id", tab=part["name"])
            note(sections() == cuts,
                 f"add_section left a cut under a refusal: {sections()}")
            cut = d.call("add_section", base_plane="xy", offset=1.0)["id"]
            cuts = sections()
            check("update_section on no datum", "update_section",
                  {"id": cut, "base_plane": "yz", "offset": 2.0,
                   "plane_id": 987654},
                  needle="no feature with that id", tab=part["name"])
            note(sections() == cuts,
                 f"update_section changed the cut under a refusal: "
                 f"{cuts} -> {sections()}")
            d.call("remove_section", id=cut)

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
                # finish_stock is left FOR a finishing pass: without a
                # finish_tool the save dropped it (the reader refuses it in
                # a file), so the call refuses it, naming what it needs.
                ops = [o for s in d.call("list_cam_setups")["setups"]
                       if s["id"] == setup for o in s["operations"]]
                check("finish_stock without finish_tool", "add_cam_op",
                      {"setup": setup, "type": "profile", "top_z": 21.0,
                       "bottom_z": 0.0, "finish_stock": 0.3},
                      needle="set finish_tool", tab="Job")
                check("finish_stock without finish_tool on set_cam_op",
                      "set_cam_op",
                      {"id": ops[0]["id"], "name": "Restocked",
                       "finish_stock": 0.3},
                      needle="set finish_tool", tab="Job")
                after = [o for s in d.call("list_cam_setups")["setups"]
                         if s["id"] == setup for o in s["operations"]]
                note(after == ops,
                     f"a refused finish_stock changed the operations: "
                     f"{ops} -> {after}")

            # CalculiX (docs/simulation/README.md#calculix): each refused
            # before anything starts -- no job, no run directory, no file.
            d.call("set_active_part", part=part["name"])
            static = d.call("add_study", name="On CalculiX")["id"]
            modal = d.call("add_study", name="Modes", type="modal")["id"]
            check("calculix on a modal study", "set_study",
                  {"id": modal, "solver": "calculix"},
                  needle="solved in-house", tab=part["name"])
            check("a solver that is none", "set_study",
                  {"id": static, "solver": "ccx"},
                  needle="auto, in_house, calculix", tab=part["name"])
            check("timeout_s on an in-house study", "run_study",
                  {"id": static, "timeout_s": 5},
                  needle="timeout_s bounds a CalculiX run", tab=part["name"])
            check("a deck into no folder", "export_study_inp",
                  {"id": static, "path": path(os.path.join("no", "x.inp"))},
                  needle="does not exist", tab=part["name"])
            check("a deck of a modal study", "export_study_inp",
                  {"id": modal, "path": path("m.inp")},
                  needle="static, buckling or thermal", tab=part["name"])
            d.call("set_study", id=static, solver="calculix")
            d.call("set_settings",
                   calculix_path=os.path.join(work, "no-ccx-here"))
            check("no ccx", "run_study", {"id": static},
                  needle="sudo apt install calculix-ccx", tab=part["name"])
            check("no ccx, async", "run_study", {"id": static, "async": True},
                  needle="calculix_path setting", tab=part["name"])
            d.call("set_settings", calculix_path="")
            d.call("remove_study", id=modal)
            d.call("remove_study", id=static)
            # Buckling (docs/simulation/README.md#buckling): a factor scales
            # one load set.
            buck = d.call("add_study", name="Buckle", type="buckling")["id"]
            check("load cases on a buckling study", "set_study",
                  {"id": buck, "load_cases": [{"name": "a"}]},
                  needle="takes no load cases", tab=part["name"])
            check("an analysis that is none", "add_study",
                  {"type": "bucking"},
                  needle="static, modal, topology, buckling", tab=part["name"])
            d.call("remove_study", id=buck)
            # Nonlinear (docs/simulation/README.md#nonlinear-static): what a
            # study cannot take is refused by set_study, and a run that stops
            # converging is refused with the load it reached, nothing stored.
            nl = d.call("add_study", name="Nonlinear")["id"]
            large = {"large_deformation": True}
            check("a nonlinear study in-house", "set_study",
                  {"id": nl, "solver": "in_house", "nonlinear": large},
                  needle="in-house solver is linear", tab=part["name"])
            check("nonlinear without large_deformation", "set_study",
                  {"id": nl, "nonlinear": {"snap_through": True}},
                  needle="large_deformation: true", tab=part["name"])
            check("a nonlinear key that is none", "set_study",
                  {"id": nl, "nonlinear": {"large_deformation": True,
                                           "riks": True}},
                  needle="has no key 'riks'", tab=part["name"])
            check("nonlinear on a modal study", "set_study",
                  {"id": nl, "type": "modal", "nonlinear": large},
                  needle="a static study's", tab=part["name"])
            check("nonlinear with load cases", "set_study",
                  {"id": nl, "nonlinear": large,
                   "load_cases": [{"name": "a"}]},
                  needle="takes no load cases", tab=part["name"])
            d.call("set_study", id=nl, mesh_size=5.0,
                   nonlinear={"large_deformation": True,
                              "snap_through": True})
            check("in_house on a nonlinear study", "set_study",
                  {"id": nl, "solver": "in_house"},
                  needle="in-house solver is linear", tab=part["name"])
            check("converge always on a snap-through run", "run_study",
                  {"id": nl, "converge": "always"},
                  needle="peak load factor", tab=part["name"])
            d.call("set_study", id=nl, nonlinear=large)
            cube = [b for b in d.call("list_elements")["bodies"]
                    if b["name"] == "Cube"][0]

            def face(sign):
                return {"feature_id": cube["created_by"],
                        "element": [f["name"] for f in cube["faces"]
                                    if abs(f["normal"][2] - sign) < 1e-6][0]}
            d.call("set_material", feature_id=cube["created_by"],
                   material="steel")
            d.call("add_load", study=nl, type="fixed", face=face(-1.0))
            d.call("add_load", study=nl, type="force", face=face(1.0),
                   magnitude=100.0, direction=[1, 0, 0])
            stalls = os.path.join(work, "stalling-ccx")
            with open(stalls, "w") as f:
                f.write(STALLING_CCX)
            os.chmod(stalls, 0o755)
            d.call("set_settings", calculix_path=stalls)
            if mesher:
                check("a nonlinear run that stops converging", "run_study",
                      {"id": nl},
                      needle="stopped converging at 40 % of the load",
                      tab=part["name"])
            listed = [x for x in d.call("list_studies")["studies"]
                      if x["id"] == nl][0]
            note(not listed["has_results"],
                 f"a refused nonlinear run stored a result: {listed}")
            d.call("set_settings", calculix_path="")
            # Plasticity (docs/simulation/README.md#plasticity): refused
            # where nonlinear is, and on a material with no hardening curve
            # -- never run perfectly plastic.
            plastic = {"plasticity": True}
            check("a plastic study in-house", "set_study",
                  {"id": nl, "solver": "in_house", "nonlinear": plastic},
                  needle="a plastic study solves on CalculiX",
                  tab=part["name"])
            check("plasticity on a modal study", "set_study",
                  {"id": nl, "type": "modal", "nonlinear": plastic},
                  needle="a static study's", tab=part["name"])
            check("plasticity with load cases", "set_study",
                  {"id": nl, "nonlinear": plastic,
                   "load_cases": [{"name": "a"}]},
                  needle="takes no load cases", tab=part["name"])
            check("plasticity that is not a flag", "set_study",
                  {"id": nl, "nonlinear": {"plasticity": "yes"}},
                  needle="plasticity is true or false", tab=part["name"])
            d.call("set_study", id=nl, nonlinear=plastic)
            check("converge always on a plastic run", "run_study",
                  {"id": nl, "converge": "always"},
                  needle="plastic strain", tab=part["name"])
            d.call("set_material", feature_id=cube["created_by"],
                   material="pla")
            # A ccx is there (the stand-in), so the material is what refuses.
            d.call("set_settings", calculix_path=stalls)
            check("a plastic run on a material with no hardening curve",
                  "run_study", {"id": nl},
                  needle="has no hardening curve", tab=part["name"])
            d.call("set_settings", calculix_path="")
            listed = [x for x in d.call("list_studies")["studies"]
                      if x["id"] == nl][0]
            note(not listed["has_results"],
                 f"a refused plastic run stored a result: {listed}")
            d.call("set_material", feature_id=cube["created_by"],
                   material="steel")
            d.call("remove_study", id=nl)
            # Thermal (docs/simulation/README.md#thermal): each refused by
            # the call that says it, naming what is accepted; nothing is
            # defaulted.
            heat = d.call("add_study", name="Heat", type="thermal")["id"]
            hot = {"study": heat, "face": face(1.0)}
            check("a thermal load type that is none", "add_load",
                  dict(hot, type="heat"), needle="heat_flux, convection",
                  tab=part["name"])
            check("a thermal key that is none", "add_load",
                  dict(hot, type="temperature", temperature_c=20.0,
                       emissivity=0.9),
                  needle="emissivity", tab=part["name"])
            check("a structural load on a thermal study", "add_load",
                  dict(hot, type="force", magnitude=10.0),
                  needle="a structural load", tab=part["name"])
            check("a temperature below absolute zero", "add_load",
                  dict(hot, type="temperature", temperature_c=-300.0),
                  needle="absolute zero", tab=part["name"])
            check("both power_w and flux_w_m2", "add_load",
                  dict(hot, type="heat_flux", power_w=1.0, flux_w_m2=5.0),
                  needle="exactly one", tab=part["name"])
            check("a convection without its film coefficient", "add_load",
                  dict(hot, type="convection", ambient_c=20.0),
                  needle="film_w_m2k", tab=part["name"])
            check("load cases on a thermal study", "set_study",
                  {"id": heat, "load_cases": [{"name": "a"}]},
                  needle="takes no load cases", tab=part["name"])
            stat = d.call("add_study", name="Expanded")["id"]
            check("a thermal load on a static study", "add_load",
                  {"study": stat, "face": face(1.0), "type": "temperature",
                   "temperature_c": 20.0},
                  needle="study of type thermal", tab=part["name"])
            modes = d.call("add_study", name="Modes", type="modal")["id"]
            check("thermal_load on a modal study", "set_study",
                  {"id": modes, "thermal_load": {"uniform_c": 50.0,
                                                 "stress_free_c": 20.0}},
                  needle="a static study's", tab=part["name"])
            d.call("remove_study", id=modes)
            buckle = d.call("add_study", name="Buckle", type="buckling")["id"]
            check("thermal_load on a buckling study", "set_study",
                  {"id": buckle, "thermal_load": {"uniform_c": 50.0,
                                                  "stress_free_c": 20.0}},
                  needle="thermal buckling", tab=part["name"])
            d.call("remove_study", id=buckle)
            check("a uniform thermal_load without stress_free_c", "set_study",
                  {"id": stat, "thermal_load": {"uniform_c": 50.0}},
                  needle="stress_free_c", tab=part["name"])
            check("thermal_load from a study that is not thermal",
                  "set_study", {"id": stat, "thermal_load": {"from_study": stat}},
                  needle="not a thermal study", tab=part["name"])
            d.call("add_load", study=heat, type="temperature", face=face(1.0),
                   temperature_c=80.0)
            check("thermal_load whose stress-free temperature cannot be "
                  "derived", "set_study",
                  {"id": stat, "thermal_load": {"from_study": heat}},
                  needle="stress_free_c", tab=part["name"])
            d.call("set_material", feature_id=cube["created_by"],
                   material="copper")
            check("a thermal run on a material with no conductivity",
                  "run_study", {"id": heat},
                  needle="aluminum_6061_t6", tab=part["name"])
            d.call("set_material", feature_id=cube["created_by"],
                   material="steel")
            d.call("remove_study", id=heat)
            heat = d.call("add_study", name="Heat", type="thermal")["id"]
            d.call("add_load", study=heat, type="heat_flux", face=face(1.0),
                   power_w=1.0)
            check("a body whose heat has nowhere to go", "run_study",
                  {"id": heat}, needle="nowhere to go", tab=part["name"])
            d.call("remove_study", id=heat)
            d.call("remove_study", id=stat)
            # An assembly takes a uniform thermal load only
            # (docs/simulation/README.md#a-thermal-load-on-an-assembly).
            d.call("set_active_part", part="Asm")
            ast = d.call("add_study", name="Heated")["id"]
            check("a thermal study on an assembly", "set_study",
                  {"id": ast, "type": "thermal"},
                  needle="does not solve heat", tab="Asm")
            check("thermal_load from_study on an assembly", "set_study",
                  {"id": ast, "thermal_load": {"from_study": ast}},
                  needle="uniform", tab="Asm")
            check("probe_study on an assembly without its occurrence",
                  "probe_study", {"id": ast, "face": "1/face0"},
                  needle="needs `occurrence`", tab="Asm")
            d.call("set_active_part", part="Asm")
            d.call("remove_study", id=ast)
            check("generic aluminium", "set_material",
                  {"feature_id": cube["created_by"], "material": "aluminum"},
                  needle="aluminum_6061_t6", tab=part["name"])
            # Contact (docs/simulation/README.md#contact): refused where a
            # nonlinear study is -- in-house, another analysis, load cases
            # -- and with what only a contact says: a curved face on the
            # ground, a negative friction, a pin beside it.
            ct = d.call("add_study", name="Contact")["id"]
            check("a contact on the ground", "add_load",
                  {"study": ct, "type": "contact", "face": face(-1.0)},
                  must_refuse=False, tab=part["name"])
            listed = [x for x in d.call("list_studies")["studies"]
                      if x["id"] == ct][0]
            rest = [ld for ld in listed["loads"]
                    if ld["type"] == "contact"][0]["id"]
            check("in_house on a contact study", "set_study",
                  {"id": ct, "solver": "in_house"},
                  needle="a study with a contact solves on CalculiX",
                  tab=part["name"])
            check("a contact study made modal", "set_study",
                  {"id": ct, "type": "modal"},
                  needle="a contact is a static study's", tab=part["name"])
            check("load cases on a contact study", "set_study",
                  {"id": ct, "load_cases": [{"name": "a"}]},
                  needle="takes no load cases", tab=part["name"])
            check("converge always on a contact study", "run_study",
                  {"id": ct, "converge": "always"},
                  needle="converge: always", tab=part["name"])
            check("snap-through on a contact study", "set_study",
                  {"id": ct, "nonlinear": {"large_deformation": True,
                                           "snap_through": True}},
                  needle="cannot hold a contact", tab=part["name"])
            check("a negative friction", "add_load",
                  {"study": ct, "type": "contact", "face": face(1.0),
                   "friction": -0.2},
                  needle="friction is a coefficient", tab=part["name"])
            check("a contact with a magnitude", "add_load",
                  {"study": ct, "type": "contact", "face": face(1.0),
                   "magnitude": 3.0},
                  needle="does not take `magnitude`", tab=part["name"])
            check("friction on a fixed support", "add_load",
                  {"study": ct, "type": "fixed", "face": face(1.0),
                   "friction": 0.3},
                  needle="does not take `friction`", tab=part["name"])
            check("contact_with on a part study", "add_load",
                  {"study": ct, "type": "contact", "contact_with": 1},
                  needle="assembly study", tab=part["name"])
            d.call("remove_load", study=ct, id=rest)
            mod = d.call("add_study", name="Contact modes", type="modal")["id"]
            check("a contact on a modal study", "add_load",
                  {"study": mod, "type": "contact", "face": face(-1.0)},
                  needle="a contact is a static study's", tab=part["name"])
            ih = d.call("add_study", name="In-house")["id"]
            d.call("set_study", id=ih, solver="in_house")
            check("a contact on an in-house study", "add_load",
                  {"study": ih, "type": "contact", "face": face(-1.0)},
                  needle="a study with a contact solves on CalculiX",
                  tab=part["name"])
            for sid in (ct, mod, ih):
                d.call("remove_study", id=sid)

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
