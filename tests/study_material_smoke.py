#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A study's own material, through the RPCs alone
(docs/simulation/README.md#a-study-states-its-own-material).

A bar with NO material of its own -- refused by a study that reads it --
solved with the constants the study states: add_study and set_study take
`material`, list_studies answers it, the tip deflection halves when the
study's modulus doubles, the result goes stale when the material moves,
`null` goes back to the body's (refused again; undo is the GUI test's,
sim/a_study_states_its_own_material), and every malformed
material is refused naming what is accepted, changing nothing.

Skips (exit 77) on a build without Netgen.

Usage: study_material_smoke.py <path-to-designer-binary>
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
            print("study_material_smoke: SKIP (built without Netgen)")
            return SKIP
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (100, 10, 10), "Bar")
        els = d.call("list_elements")["bodies"][0]

        def face(axis, at):
            name = [f["name"] for f in els["faces"]
                    if abs(abs(f["normal"][axis]) - 1) < 1e-9
                    and abs(f["centroid"][axis] - at) < 1e-6][0]
            return {"feature_id": els["created_by"], "element": name}

        # Refused before anything is made: no study, no simulation.
        before = d.call("list_simulations")
        for bad, needle in [
                ({"young_gpa": 200.0}, "needs poisson"),
                ({"young_gpa": 200.0, "poisson": 0.3, "e_gpa": 1.0},
                 "young_gpa, poisson, density_kg_m3?"),
                ({"young_gpa": 0.0, "poisson": 0.3}, "above 0"),
                ({"young_gpa": 200.0, "poisson": 0.5}, "below 0.5"),
                ({"young_gpa": 200.0, "poisson": 0.3, "density_kg_m3": -1},
                 "0 or more"),
                ("steel", "a study's material is {")]:
            refused(d, "add_study", needle, name="Bad", material=bad)
        expect(d.call("list_simulations") == before,
               "a refused add_study made nothing")

        own = {"young_gpa": 200.0, "poisson": 0.3}
        made = d.call("add_study", name="Bar", material=own)
        sid = made["id"]
        # The reply, and list_studies, answer it in the keys it was given.
        expect(made.get("material") == own, f"add_study answers it: {made}")
        d.call("set_study", id=sid, mesh_size=6.0, converge="never")
        d.call("add_load", study=sid, type="fixed", face=face(0, 0.0))
        d.call("add_load", study=sid, type="force", face=face(0, 100.0),
               magnitude=100.0, direction=[0, 0, -1])
        row = next(s for s in d.call("list_studies")["studies"]
                   if s["id"] == sid)
        expect(row.get("material") == own, f"list_studies answers it: {row}")

        stiff = d.call("run_study", id=sid)["max_displacement_mm"]["value"]
        expect(stiff > 0, f"solved with the study's material: {stiff}")

        # set_study: a doubled modulus halves the deflection, and the result
        # it had is stale until it is run again.
        d.call("set_study", id=sid, material={"young_gpa": 400.0,
                                              "poisson": 0.3})
        row = next(s for s in d.call("list_studies")["studies"]
                   if s["id"] == sid)
        expect(row["stale"], f"a material change makes it stale: {row}")
        half = d.call("run_study", id=sid)["max_displacement_mm"]["value"]
        expect(abs(half / stiff - 0.5) < 1e-6,
               f"E doubled, deflection halved: {stiff} -> {half}")

        # A refused set_study changes nothing.
        refused(d, "set_study", "below 0.5", id=sid,
                material={"young_gpa": 400.0, "poisson": 0.7})
        row = next(s for s in d.call("list_studies")["studies"]
                   if s["id"] == sid)
        expect(row["material"]["young_gpa"] == 400.0 and not row["stale"],
               f"the refused edit changed nothing: {row}")

        # null: the body's own material, which this body has none of.
        d.call("set_study", id=sid, material=None)
        row = next(s for s in d.call("list_studies")["studies"]
                   if s["id"] == sid)
        expect("material" not in row, f"null clears it: {row}")
        refused(d, "run_study", "stiffness", id=sid)
    print("study_material_smoke: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
