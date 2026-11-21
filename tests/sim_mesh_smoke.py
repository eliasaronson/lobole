#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The mesh step, the study check, the result fields and the VTU export over
the RPCs alone (docs/simulation/README.md#the-mesh-you-look-at,
#checking-a-study, #results-display, #vtu-export). Headless: no renderer.

Each part plants what the verb exists to find and reads it back:

  - mesh_study keeps its mesh and reports on it: a plate one element through,
    a refinement that asks for nothing, the volume beside the body's;
  - check_study answers THREE planted problems in one reply, each with a call
    that fixes it, and is empty once they are made; a load edit leaves the
    kept mesh current and a mesh-size edit marks it stale;
  - a run's applied and reaction totals cancel;
  - set_result_display takes every field the result holds and refuses the
    ones it does not, by name; the range clamp is checked before it is set;
  - export_study_result writes quadratic tetrahedra to a file and refuses a
    result reopened from disk, which keeps its surface only.

What a field's NUMBER is belongs to tests/test_fem_fields.cpp, against closed
forms; what the mesh report finds, to tests/test_fem_mesh_report.cpp. This is
the surface over them.

Skips (exit 77) on a build without Netgen.

Usage: sim_mesh_smoke.py <path-to-designer-binary>
"""
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch  # noqa: E402
import smoke_shapes as shapes  # noqa: E402

SKIP = 77


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def refused(d, method, needle, **params):
    try:
        d.call(method, **params)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refused naming '{needle}': {e}")
        return str(e)
    raise AssertionError(f"{method}({params}) was accepted")


def faces_of(d):
    els = d.call("list_elements")["bodies"][0]
    by_normal = {}
    for f in els["faces"]:
        n = tuple(round(c) for c in f["normal"])
        by_normal[n] = f["name"]
    return els["created_by"], by_normal


def mesh_report(d):
    """A 60 x 20 x 1 plate at the automatic size: one element through."""
    d.call("new_project")
    fid = shapes.box(d, (0, 0, 0), (60, 20, 1), "Plate")["id"]
    d.call("set_material", feature_id=fid, material="steel")
    created_by, face = faces_of(d)
    study = d.call("add_study", name="Plate")["id"]
    d.call("add_load", study=study, type="fixed",
           face={"feature_id": created_by, "element": face[(-1, 0, 0)]})
    d.call("add_load", study=study, type="force", magnitude=5.0,
           direction=[0, 0, -1],
           face={"feature_id": created_by, "element": face[(1, 0, 0)]})
    # A refinement coarser than the study's own size: it can take nothing.
    d.call("add_refinement", study=study, size=50.0,
           face={"feature_id": created_by, "element": face[(0, 0, 1)]})
    refused(d, "set_mesh_display", "no mesh to draw", study=study,
            view="edges")
    try:
        mesh = d.call("mesh_study", id=study)
    except RuntimeError as e:
        if "without FEM support" in str(e):
            print("sim_mesh_smoke: built without Netgen — skipping")
            sys.exit(SKIP)
        raise
    body = mesh["bodies"][0]
    expect(body["elements"] > 0 and body["nodes"] > 0, "meshed to elements")
    report = body["report"]
    expect(0.0 < report["worst_quality"] <= 1.0,
           f"a worst quality: {report['worst_quality']}")
    worst = report["worst_elements"]
    expect(len(worst) >= 1 and all(len(w["at"]) == 3 for w in worst),
           "the worst elements, each with where it is")
    expect(worst == sorted(worst, key=lambda w: w["quality"]),
           "worst first")
    expect(all(0.0 < w["at"][2] < 1.0 for w in worst),
           "each inside the plate")
    size = report["size_mm"]
    expect(0 < size["min"] <= size["mean"] <= size["max"],
           f"element sizes in order: {size}")
    vol = report["volume"]
    expect(abs(vol["body_mm3"] - 1200.0) < 1e-6
           and vol["apart_mm3"] <= vol["allowed_mm3"],
           f"the mesh fills the body: {vol}")
    walls = report["thin_walls"]
    expect(len(walls) >= 1 and abs(walls[0]["thickness_mm"] - 1.0) < 1e-6,
           f"the wall one element through, 1 mm: {walls[:1]}")
    expect({walls[0]["face_a"], walls[0]["face_b"]}
           == {face[(0, 0, 1)], face[(0, 0, -1)]},
           "between the plate's two faces, by name")
    misses = report["refinement_misses"]
    expect(len(misses) == 1 and misses[0]["face"] == face[(0, 0, 1)]
           and misses[0]["asked_mm"] == 50.0,
           f"the refinement that did not take: {misses}")
    expect(report["straightened_nodes"] == 0, "nothing straightened")
    codes = [f["code"] for f in mesh["findings"]]
    expect({"volume", "thin_wall", "refinement", "worst_element"}
           <= set(codes), f"the same as rows: {sorted(set(codes))}")
    framed = [f for f in mesh["findings"] if f["code"] == "thin_wall"][0]
    expect("at" in framed and framed["radius_mm"] > 0,
           "a row says where to look")
    expect(mesh["stale"] is False, "fresh")

    # The mesh is kept: it can be drawn, a load edit leaves it current, and
    # the mesh size moving marks it.
    shown = d.call("set_mesh_display", study=study, view="quality")
    expect(shown == {"view": "quality", "stale": False}, f"drawn: {shown}")
    refused(d, "set_mesh_display", "views: off, edges, quality, size",
            study=study, view="wire")
    d.call("add_load", study=study, type="pressure", magnitude=0.01,
           face={"feature_id": created_by, "element": face[(0, 0, 1)]})
    expect(d.call("set_mesh_display", study=study, view="size")["stale"]
           is False, "editing a load leaves the mesh current")
    check = d.call("check_study", id=study)
    expect(check["ok"] is True and check["problems"] == [],
           f"nothing a run would refuse: {check}")
    expect(check["receipt"]["standing"] == "unverified",
           "its receipt does not claim the run will solve")
    d.call("set_study", id=study, mesh_size=3.0)
    expect(d.call("set_mesh_display", study=study, view="edges")["stale"]
           is True, "editing the mesh size marks it stale")
    check = d.call("check_study", id=study)
    stale = [p for p in check["problems"] if p["code"] == "stale_mesh"]
    expect(check["ok"] is False and len(stale) == 1
           and stale[0]["fix"]["method"] == "mesh_study"
           and stale[0]["fix"]["params"] == {"id": study},
           f"and the check says so, with the call: {check}")
    again = d.call("mesh_study", **stale[0]["fix"]["params"])
    expect(again["stale"] is False
           and d.call("check_study", id=study)["ok"] is True,
           "made again, it is current")
    # Editing the part marks the mesh, as the mesh size did. The display
    # calls brought the simulation's tab forward; the part is edited on its
    # own.
    d.call("exit_simulation")
    plate = [f for f in d.call("get_state")["features"] if f["id"] == fid][0]
    plate = {k: v for k, v in plate.items()
             if k not in ("id", "distance_expr")}
    plate["distance"] = 1.5  # the plate's thickness
    d.call("modify_feature", feature_id=fid, feature=plate)
    expect(d.call("set_mesh_display", study=study, view="edges")["stale"]
           is True, "editing the part marks the mesh stale")
    d.call("set_mesh_display", study=study, view="off")


def check_three(d):
    """Three faults planted in one study, answered in one reply."""
    d.call("new_project")
    shapes.box(d, (0, 0, 0), (100, 10, 10), "Bar")  # 1: no material
    boss = shapes.cylinder(d, (50, 40, 0), "z", 3.0, 6.0, "Boss")["id"]
    bodies = d.call("list_elements")["bodies"]
    bar = [b for b in bodies if b["created_by"] != boss][0]
    created_by = bar["created_by"]
    face = {tuple(round(c) for c in f["normal"]): f["name"]
            for f in bar["faces"]}
    boss_face = [b for b in bodies if b["created_by"] == boss][0][
        "faces"][0]["name"]
    study = d.call("add_study", name="Faulty")["id"]
    # 2: a force, and nothing held.
    d.call("add_load", study=study, type="force", magnitude=100.0,
           direction=[1, 0, 0],
           face={"feature_id": created_by, "element": face[(1, 0, 0)]})
    # 3: a load whose face then goes with its feature.
    gone = d.call("add_load", study=study, type="pressure", magnitude=1.0,
                  face={"feature_id": boss, "element": boss_face})["id"]
    d.call("delete_feature", id=boss)
    before = d.call("list_studies")["studies"][0]
    revision = d.call("validation_revision")
    check = d.call("check_study", id=study)
    codes = sorted(p["code"] for p in check["problems"])
    problems = {p["code"]: p for p in check["problems"]}
    expect(check["ok"] is False, "not ok")
    expect(codes == ["anchor", "material", "no_support"],
           f"three planted problems in one reply: {codes}")
    expect(problems["anchor"]["load_id"] == gone
           and problems["anchor"]["fix"]["method"] == "remove_load"
           and problems["anchor"]["fix"]["params"] == {"study": study,
                                                       "id": gone},
           f"the dangling load, with the call that removes it: "
           f"{problems['anchor']}")
    expect(problems["no_support"]["fix"]["method"] == "add_load"
           and problems["no_support"]["fix"]["params"]["type"] == "fixed"
           and problems["no_support"]["fix"]["params"]["face"] is None
           and "note" in problems["no_support"]["fix"],
           "a support to add, its face the caller's to choose")
    expect(problems["material"]["fix"]["method"] == "set_material"
           and problems["material"]["body"] == "Bar",
           "a material to set, on the body by name")
    after = d.call("list_studies")["studies"][0]
    expect(after == before, "read-only: the study is as it was")
    expect(d.call("validation_revision") == revision,
           "and the document's version did not move")
    # A run refuses the first of them; the check was the list.
    refused(d, "run_study", "", id=study)

    # Make each fix as it was given, filling in what was the caller's.
    d.call(problems["anchor"]["fix"]["method"],
           **problems["anchor"]["fix"]["params"])
    fix = dict(problems["no_support"]["fix"]["params"])
    fix["face"] = {"feature_id": created_by, "element": face[(-1, 0, 0)]}
    d.call(problems["no_support"]["fix"]["method"], **fix)
    fix = dict(problems["material"]["fix"]["params"])
    fix["material"] = "steel"
    d.call(problems["material"]["fix"]["method"], **fix)
    check = d.call("check_study", id=study)
    expect(check["ok"] is True and check["problems"] == [],
           f"each fix made, nothing is left: {check}")
    return study


def fields_and_export(d, study, scratch):
    d.call("set_study", id=study, mesh_size=4.0)
    res = d.call("run_study", id=study, converge="never")
    applied = res["applied_total"]["force_n"]
    carried = res["reaction_total"]["force_n"]
    expect(abs(applied[0] - 100.0) < 1e-6 and abs(applied[1]) < 1e-6
           and abs(applied[2]) < 1e-6, f"100 N applied along +X: {applied}")
    expect(all(abs(a + c) < 1e-3 for a, c in zip(applied, carried)),
           f"and the supports carry it: {carried}")

    # Every field a static result holds is taken, with its unit.
    units = {"von_mises": "MPa", "displacement": "mm",
             "principal_max": "MPa", "principal_mid": "MPa",
             "principal_min": "MPa", "factor_of_safety": ""}
    for field, unit in units.items():
        shown = d.call("set_result_display", study=study, field=field)
        expect(shown["field"] == field and shown["unit"] == unit,
               f"{field} in '{unit}'")
    shown = d.call("set_result_display", study=study, field="across_layers",
                   layer_normal=[0, 0, 1])
    expect(shown["field"] == "across_layers" and shown["unit"] == "MPa",
           "across_layers along a build direction")
    # ...and the ones it does not are refused, saying what is missing.
    refused(d, "set_result_display", "layer_normal", study=study,
            field="across_layers")
    refused(d, "set_result_display", "layer_normal must have a length",
            study=study, field="across_layers", layer_normal=[0, 0, 0])
    refused(d, "set_result_display", "interlayer strength", study=study,
            field="interlayer_factor", layer_normal=[0, 0, 1])
    refused(d, "set_result_display", "plastic strain", study=study,
            field="plastic_strain")
    refused(d, "set_result_display", "contact pressure", study=study,
            field="contact_pressure")
    refused(d, "set_result_display", "no temperature", study=study,
            field="temperature")
    refused(d, "set_result_display", "topology study's views", study=study,
            field="density")
    refused(d, "set_result_display", "principal_max, principal_mid",
            study=study, field="principal")

    # The range clamp: set, read back, given back; and checked before
    # anything is set.
    shown = d.call("set_result_display", study=study, field="von_mises",
                   range_min=0.5, range_max=250.0, markers=False)
    expect(shown["range_min"] == 0.5 and shown["range_max"] == 250.0
           and shown["markers"] is False, f"a range and no markers: {shown}")
    refused(d, "set_result_display", "range_max must be above range_min",
            study=study, field="von_mises", range_max=0.25)
    kept = d.call("set_result_display", study=study, field="von_mises")
    expect(kept["range_min"] == 0.5 and kept["range_max"] == 250.0,
           "a refused call changed nothing")
    shown = d.call("set_result_display", study=study, field="von_mises",
                   range_min=None, range_max=None, markers=True)
    expect(shown["range_min"] is None and shown["range_max"] is None,
           "null gives the range back to the field")
    shown = d.call("set_result_display", study=study, field="von_mises",
                   range_max=250.0)
    shown = d.call("set_result_display", study=study, field="displacement")
    expect(shown["range_max"] is None, "a range is one field's")
    # The above-yield preset: steel's yield, by name rather than by number.
    shown = d.call("set_result_display", study=study, field="von_mises",
                   range_max="yield")
    expect(shown["range_max"] == 250.0, f"range_max at the yield: {shown}")
    refused(d, "set_result_display", 'a number, null or "yield"',
            study=study, field="von_mises", range_max="high")
    refused(d, "set_result_display", "a stress",
            study=study, field="displacement", range_max="yield")
    d.call("set_result_display", study=study, field="none")

    # --- The export.
    path = os.path.join(scratch, "bar.vtu")
    done = d.call("export_study_result", id=study, path=path)
    expect(done["cell_type"] == "VTK_QUADRATIC_TETRA"
           and done["cells"] == res["elements"]
           and done["points"] == res["nodes"],
           f"every element and node written: {done}")
    expect(done["point_data"][:3] == ["displacement", "von_mises", "stress"]
           and "body" in done["cell_data"], f"with its fields: {done}")
    text = open(path).read()
    expect(done["bytes"] == len(text.encode()), "the size it says")
    piece = re.search(r'<Piece NumberOfPoints="(\d+)" NumberOfCells="(\d+)"',
                      text)
    expect(piece and int(piece.group(1)) == done["points"]
           and int(piece.group(2)) == done["cells"], "a .vtu piece")
    types = re.search(r'Name="types"[^>]*>\s*([\d\s]+)</DataArray>', text)
    expect(types and set(types.group(1).split()) == {"24"},
           "every cell a quadratic tetra (24)")
    refused(d, "export_study_result", "does not exist", id=study,
            path=os.path.join(scratch, "nowhere", "bar.vtu"))
    refused(d, "export_study_result", "path is required", id=study)
    refused(d, "export_study_result", "load case", id=study, path=path,
            case=1)

    project = os.path.join(scratch, "bar.lob")
    d.call("save_project", path=project)
    return project


def reopened(d, project, scratch):
    """A result reopened from disk, in a session that never solved it, keeps
    its surface only: refused, saying how to get the rest back, and nothing
    written. (The session that solved it still holds the whole result by its
    input hash, so this is another one.)"""
    d.call("open_project", path=project)
    study = d.call("list_studies")["studies"][0]["id"]
    expect(d.call("list_studies")["studies"][0]["has_results"] is True,
           "the result reopened")
    res = d.call("study_results", id=study)
    expect("reaction_total" in res and "applied_total" not in res,
           "its reactions are kept; the applied total is a run's")
    again = os.path.join(scratch, "reopened.vtu")
    why = refused(d, "export_study_result", "boundary surface only",
                  id=study, path=again)
    expect("recompute" in why and not os.path.exists(again),
           "and says to run again; no file was made")
    # The fields its surface holds are still shown.
    shown = d.call("set_result_display", study=study, field="principal_min")
    expect(shown["unit"] == "MPa", "a reopened result still has its fields")


def main():
    binary = sys.argv[1]
    scratch = tempfile.mkdtemp(prefix="lobole_sim_mesh_")
    with launch(binary) as app:
        d = app.client
        expect(d.call("ping")["ok"], "ping")
        d.call("set_settings", converge_auto_s=0.0)
        mesh_report(d)
        study = check_three(d)
        project = fields_and_export(d, study, scratch)
    with launch(binary) as app:
        reopened(app.client, project, scratch)
    print("sim_mesh_smoke: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
