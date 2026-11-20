#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: a part's rules, tolerances and feature gaps as
expressions, over JSON-RPC.

docs/variables/README.md#numbers-that-are-expressions: a sheet's bend radius
bound with `bend_radius_mm_expr` rebuilds the bends when its variable
changes, one the material cannot take is reported in set_variable's OWN
`regen` (`owners`) and keeps its last good value, an unknown variable is
refused before anything changes, the getters answer each text beside its
number; the tube rules, a tolerance's limits, a sketch pattern's count and a
feature's formerly plain fields (a box's position) the same; rename and the
delete refusal reach them; and a save -> reopen keeps every text.

Usage: part_exprs_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **params):
    try:
        d.call(method, **params)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refusal should say {needle!r}: {e}")
        return str(e)
    raise AssertionError(f"{method} accepted {params}")


def straight_edge(d, want_mid):
    body = d.call("list_elements")["bodies"][0]
    for e in body["edges"]:
        mid = e.get("midpoint")
        if e.get("kind") != "line" or mid is None:
            continue
        if all(abs(a - b) < 1e-6 for a, b in zip(mid, want_mid)):
            return e["name"]
    raise AssertionError(f"no straight edge at {want_mid}")


def bend_radius(d):
    bends = d.call("list_bends")["bends"]
    expect(len(bends) == 1, f"one bend: {bends}")
    return bends[0]["radius_mm"]


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="lobole_part_exprs_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        d.call("add_part", name="tray")
        d.call("set_variable", name="r_bend", expression="3")

        # --- A bend radius bound to a variable ---------------------------
        r = d.call("set_sheet_metal_rules", material="aluminum_6061_t6",
                   gauge="1.5mm", bend_radius_mm_expr="r_bend")
        expect(abs(r["rules"]["bend_radius_mm"] - 3.0) < 1e-9 and
               r["rules"].get("bend_radius_mm_expr") == "r_bend",
               f"the radius from the variable, its text beside it: {r}")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Blank",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "line", "id": 1, "a": [0, 0], "b": [100, 0]},
                {"type": "line", "id": 2, "a": [100, 0], "b": [100, 60]},
                {"type": "line", "id": 3, "a": [100, 60], "b": [0, 60]},
                {"type": "line", "id": 4, "a": [0, 60], "b": [0, 0]}]})["id"]
        base = d.call("add_feature", feature={
            "type": "BaseFlange", "name": "Base", "sketch_id": sk})["id"]
        edge = straight_edge(d, (50.0, 0.0, 1.5))
        r = d.call("add_feature", feature={
            "type": "EdgeFlange", "name": "Wall", "length": 20,
            "edges": [{"feature_id": base, "element": edge}]})
        expect(r["regen"]["ok"], r["regen"])
        expect(abs(bend_radius(d) - 3.0) < 1e-9, "bent at 3")

        r = d.call("set_variable", name="r_bend", expression="4")
        expect(r.get("regen", {}).get("ok", True), f"a clean change: {r}")
        expect(abs(bend_radius(d) - 4.0) < 1e-9,
               "the variable rebuilt the bend")

        # Under what aluminium takes: reported where the change was made.
        r = d.call("set_variable", name="r_bend", expression="1")
        reg = r.get("regen", {})
        expect(reg.get("ok") is False, f"set_variable reports it: {r}")
        owners = reg.get("owners", [])
        expect(owners and owners[0]["owner"] == "sheet-metal rules",
               f"the rules, by name: {reg}")
        expect(abs(bend_radius(d) - 4.0) < 1e-9, "the last good radius stays")
        d.call("set_variable", name="r_bend", expression="4")

        # --- Refused at the call, changing nothing ----------------------
        refused(d, "set_sheet_metal_rules", "nowhere",
                bend_radius_mm_expr="nowhere + 1")
        refused(d, "set_sheet_metal_rules", "does not read",
                k_factor_expr="0.4 *")
        got = d.call("get_sheet_metal_rules")["rules"]
        expect(got.get("bend_radius_mm_expr") == "r_bend" and
               "k_factor_expr" not in got, f"the refusals changed nothing: {got}")

        # --- A number alone unbinds -------------------------------------
        d.call("set_sheet_metal_rules", bend_radius_mm=4.0)
        expect("bend_radius_mm_expr" not in
               d.call("get_sheet_metal_rules")["rules"], "a literal again")
        d.call("set_sheet_metal_rules", bend_radius_mm_expr="r_bend")

        # --- The tube rules, a tolerance, a pattern, a box --------------
        d.call("set_variable", name="die", expression="60")
        t = d.call("set_tube_rules", material="steel",
                   former_radii_mm=[0.0, 80.0],
                   former_radii_mm_expr=["die", ""],
                   min_straight_mm_expr="die / 2")
        expect(t["rules"]["former_radii_mm"] == [60.0, 80.0] and
               t["rules"].get("former_radii_mm_expr") == ["die", ""] and
               t["rules"].get("min_straight_expr") == "die / 2",
               f"the die set from a variable: {t}")
        d.call("set_variable", name="die", expression="70")
        t = d.call("get_tube_rules")["rules"]
        expect(t["former_radii_mm"][0] == 70.0 and t["min_straight"] == 35.0,
               f"the rules followed it: {t}")
        refused(d, "set_tube_rules", "nowhere", min_straight_mm_expr="nowhere")

        d.call("set_variable", name="tol", expression="0.2")
        pin = d.call("add_feature", feature={
            "type": "Cylinder", "name": "Pin", "on": "Top",
            "diameter": 10.0, "height": 10.0})
        expect(pin["regen"]["ok"], pin["regen"])
        bore = [(b["created_by"], f["name"])
                for b in d.call("list_elements")["bodies"]
                for f in b.get("faces", []) if f.get("kind") == "cylinder"]
        expect(bore, "the pin's round face")
        tol = d.call("set_tolerance", feature_id=bore[0][0],
                     element=bore[0][1], lower_expr="-tol", upper_expr="tol")
        expect(abs(tol["upper"] - 0.2) < 1e-12 and
               tol.get("upper_expr") == "tol", f"the band's texts: {tol}")
        d.call("set_variable", name="tol", expression="0.3")
        listed = d.call("list_tolerances")["tolerances"]
        expect(abs(listed[0]["upper"] - 0.3) < 1e-12 and
               listed[0].get("lower_expr") == "-tol",
               f"the band followed: {listed}")
        r = d.call("set_variable", name="tol", expression="-0.1")
        reg = r.get("regen", {})
        expect(any(o["owner"].startswith("tolerance ") and "reversed" in
                   o["error"] for o in reg.get("owners", [])),
               f"a reversed band reported: {reg}")
        d.call("set_variable", name="tol", expression="0.3")

        d.call("set_variable", name="n", expression="4")
        dots = d.call("add_feature", feature={
            "type": "Sketch", "name": "Dots",
            "plane": {"origin": [0, 0, 30], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "id": 7, "center": [0, 0],
                          "radius": 1.0}]})["id"]
        pat = d.call("pattern", sketch_id=dots, sources=[7], count=2,
                     count_expr="n", step=[5.0, 0.0], step_expr=["n * 2", ""])
        expect(len(pat["generated"]) == 3, f"n copies: {pat}")
        refused(d, "pattern", "nowhere", sketch_id=dots, sources=[7],
                count_expr="nowhere", step=[5.0, 0.0])
        d.call("set_variable", name="n", expression="6")
        sketch = next(f for f in d.call("get_state")["features"]
                      if f["id"] == dots)
        op = sketch["derived_ops"][0]
        expect(op["count"] == 6 and op.get("count_expr") == "n" and
               op["vec"][0] == 12.0, f"the op followed n: {op}")

        axis = d.call("add_feature", feature={
            "type": "DatumAxis", "name": "Lid", "kind": "two_points",
            "points": [[0.0, 0.0, 0.0], [0.0, 0.0, 10.0]],
            "points_expr": [["die", "", ""], ["die", "", ""]]})
        expect(axis["regen"]["ok"], axis["regen"])
        spec = next(f for f in d.call("get_state")["features"]
                    if f["id"] == axis["id"])
        expect(spec["points"][0][0] == 70.0 and
               spec.get("points_expr") == [["die", "", ""], ["die", "", ""]],
               f"a datum's points from a variable: {spec}")

        # --- Rename reaches them; delete is refused naming them ----------
        d.call("rename_variable", name="die", new_name="former")
        expect(d.call("get_tube_rules")["rules"]["former_radii_mm_expr"] ==
               ["former", ""], "the tube's text followed the rename")
        msg = refused(d, "delete_variable", "tube rules", name="former")
        expect("feature 'Lid'" in msg, f"every owner named: {msg}")
        refused(d, "delete_variable", "sheet-metal rules", name="r_bend")
        refused(d, "delete_variable", "tolerance", name="tol")

        # --- Save, reopen: every text where it was -----------------------
        proj = os.path.join(tmp, "tray.lob")
        d.call("save_project", path=proj)
        d.call("new_project")
        d.call("open_project", path=proj)
        d.call("set_active_part", name="tray")
        got = d.call("get_sheet_metal_rules")["rules"]
        expect(got.get("bend_radius_mm_expr") == "r_bend" and
               abs(got["bend_radius_mm"] - 4.0) < 1e-9,
               f"reopened with its text: {got}")
        expect(d.call("get_tube_rules")["rules"].get("min_straight_expr") ==
               "former / 2", "the tube's texts reopened")
        expect(d.call("list_tolerances")["tolerances"][0].get("upper_expr") ==
               "tol", "the band's texts reopened")
        print("part_exprs_smoke: OK")
    finally:
        app.terminate()


if __name__ == "__main__":
    main()
