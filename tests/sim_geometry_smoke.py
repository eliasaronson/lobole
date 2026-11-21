#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The geometry step of a simulation through the RPCs alone
(docs/simulation/README.md#the-geometry-step).

A plate with fastener holes and a datum plane through its middle:
check_sim_geometry names the holes' bores as narrower than the elements,
with the feature that made them; set_sim_geometry suppresses that feature
(the part keeps it), list_simulations answers it, the mesh has fewer
elements, and a load on a bore is refused naming the feature; the symmetry
plane cuts the study to a half model whose deflection is the whole one's;
set_result_display mirrors it. Every malformed or refused call changes
nothing (docs/scripting/rpc.md#a-refused-call-changes-nothing).

Skips (exit 77) on a build without Netgen.

Usage: sim_geometry_smoke.py <path-to-designer-binary>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch  # noqa: E402
import smoke_shapes as shapes  # noqa: E402

SKIP = 77


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **params):
    try:
        d.call(method, **params)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refused without '{needle}': {e}")
        return str(e)
    raise AssertionError(f"{method} {params} was accepted")


def main():
    binary = os.path.abspath(sys.argv[1])
    with launch(binary) as app:
        d = app.client
        if not d.call("ping").get("features", {}).get("mesher"):
            print("sim_geometry_smoke: SKIP (built without Netgen)")
            return SKIP
        d.call("new_project")
        # 120 x 40 x 6, four 4 mm holes along its middle (y = 20).
        shapes.box(d, (0, 0, 0), (120, 40, 6), "Plate", material="steel")
        d.call("add_feature", feature={
            "type": "Sketch", "name": "Centres",
            "plane": {"origin": [0, 0, 6], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "point", "p": [x, 20.0]}
                         for x in (20.0, 45.0, 75.0, 100.0)]})
        feats = {f["name"]: f["id"] for f in d.call("get_state")["features"]}
        holes = d.call("add_feature", feature={
            "type": "Hole", "name": "Fastener holes", "hole_kind": "simple",
            "sketch_id": feats["Centres"], "diameter": 4.0,
            "through": True})["id"]
        mid = d.call("add_feature", feature={
            "type": "DatumPlane", "name": "Middle", "kind": "offset",
            "base_plane": "xz", "offset": 20.0})["id"]
        els = d.call("list_elements")["bodies"][0]
        part_faces = len(els["faces"])

        def face(axis, at):
            name = [f["name"] for f in els["faces"]
                    if abs(abs(f["normal"][axis]) - 1) < 1e-9
                    and abs(f["centroid"][axis] - at) < 1e-6][0]
            return {"feature_id": els["created_by"], "element": name}

        bore = [f["name"] for f in els["faces"] if "bore" in f["name"]][0]

        sid = d.call("add_study", name="Bend")["id"]
        d.call("set_study", id=sid, converge="never")
        d.call("set_simulation", mesh_size_mm=5.0)
        d.call("add_load", study=sid, type="fixed", face=face(0, 0.0))
        d.call("add_load", study=sid, type="force", face=face(0, 120.0),
               magnitude=100.0, direction=[0, 0, -1])

        # The check names the bores, narrower than the 5 mm elements, with
        # the feature that made them.
        check = d.call("check_sim_geometry", study=sid)
        expect(check["bodies"][0]["element_size_mm"] == 5.0, check["bodies"])
        bores = [f for f in check["findings"] if f["kind"] == "face"
                 and "bore" in f.get("element", "")]
        expect(len(bores) == 4, f"four bores found: {check['findings']}")
        expect(all(f["feature"] == {"id": holes, "name": "Fastener holes"}
                   and f["suppressible"] for f in bores), bores)

        whole = d.call("mesh_study", id=sid)
        tets = sum(b["elements"] for b in whole["bodies"])

        # Refused, changing nothing: a feature the part does not have, the
        # pad everything stands on, a datum that is no plane, flip with no
        # plane, an unknown key in the plane, `part` on a part simulation.
        sim = d.call("list_simulations")["simulations"][0]
        expect(sim["geometry"] == {"suppressed": [], "symmetry": None},
               sim["geometry"])
        for needle, params in [
                ("no feature", {"suppress": ["Nope"]}),
                ("leaves", {"suppress": ["Plate"]}),
                ("no datum plane", {"symmetry": {"datum": "Fastener holes"}}),
                ("there is none", {"flip": True}),
                ("unknown key", {"symmetry": {"datum": mid, "at": 1}}),
                ("is the part", {"part": "Part 1", "suppress": [holes]}),
                ("not suppressed", {"unsuppress": [holes]})]:
            refused(d, "set_sim_geometry", needle, **params)
        expect(d.call("list_simulations")["simulations"][0] == sim,
               "the refused calls changed nothing")

        # Suppressed: the part keeps it, the simulation says it, and the
        # mesh has fewer elements.
        g = d.call("set_sim_geometry", suppress=["Fastener holes"])["geometry"]
        expect(g["suppressed"] == [{"feature": holes,
                                    "name": "Fastener holes"}], g)
        expect(len(d.call("list_elements")["bodies"][0]["faces"]) ==
               part_faces, "the part is unchanged")
        row = next(s for s in d.call("list_studies")["studies"]
                   if s["id"] == sid)
        less = d.call("mesh_study", id=sid)
        fewer = sum(b["elements"] for b in less["bodies"])
        expect(fewer < tets, f"fewer elements without the holes: {fewer} "
               f"against {tets}")
        check = d.call("check_sim_geometry", study=sid)
        expect(not any("bore" in f.get("element", "")
                       for f in check["findings"]), check["findings"])

        # A load on a bore the suppressed feature made is refused, naming it.
        before = d.call("list_studies")
        refused(d, "add_load", "'Fastener holes' made", study=sid,
                type="pressure", face=bore, magnitude=1.0)
        refused(d, "add_refinement", "'Fastener holes'", study=sid,
                face=bore, size=1.0)
        expect(d.call("list_studies") == before, "nothing was added")

        # The symmetry plane: a half model, its deflection the whole's.
        full = d.call("run_study", id=sid)
        g = d.call("set_sim_geometry", symmetry={"datum": "Middle"})["geometry"]
        expect(g["symmetry"]["datum"] == {"id": mid, "name": "Middle"}, g)
        row = next(s for s in d.call("list_studies")["studies"]
                   if s["id"] == sid)
        expect(row["stale"], f"the cut makes the result stale: {row}")
        half = d.call("run_study", id=sid)
        a = full["max_displacement_mm"]["value"]
        b = half["max_displacement_mm"]["value"]
        expect(abs(b - a) < 0.05 * a, f"half {b} against whole {a}")
        refused(d, "set_result_display", "true or false", study=sid,
                field="von_mises", mirror="yes")
        shown = d.call("set_result_display", study=sid, field="von_mises",
                       mirror=True)
        expect(shown["mirror"] is True, shown)
        # The other side is empty past the plane: flipped, the half kept is
        # the other one, and answers the same.
        d.call("set_sim_geometry", flip=True)
        other = d.call("run_study", id=sid)["max_displacement_mm"]["value"]
        expect(abs(other - a) < 0.05 * a, f"the other half {other}")
        # Removed, the study is whole again and mirror is refused.
        d.call("set_sim_geometry", symmetry=None, unsuppress=[holes])
        sim = d.call("list_simulations")["simulations"][0]
        expect(sim["geometry"] == {"suppressed": [], "symmetry": None},
               sim["geometry"])
        refused(d, "set_result_display", "no symmetry plane", study=sid,
                field="von_mises", mirror=True)
    print("sim_geometry_smoke: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
