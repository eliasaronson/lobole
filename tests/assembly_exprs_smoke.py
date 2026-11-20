#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: an assembly's numbers as expressions, over JSON-RPC.

docs/assemblies/README.md#numbers-that-are-expressions: a mate's value and
its limits bound to project variables with `<key>_expr`, the mate turning
and the assembly re-solving when a variable changes, a limit narrowed past
the mate reported in set_variable's OWN `regen` (the mate keeping its last
good values), an unknown variable refused before anything changes, the
getters answering each text beside its number, rename and the delete
refusal reaching an assembly, check_literals / extract_variables on an
assembly, and a save -> reopen that keeps every text. Also the RPC half of
"a text that does not parse": refused at the call (a file keeps it).

Usage: assembly_exprs_smoke.py <path-to-designer-binary>
"""
import math
import os
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


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


def mate(d, name):
    return next(m for m in d.call("list_mates")["mates"] if m["name"] == name)


def lid_x(d, occ):
    """Where the lid's X axis points, from its quaternion [x, y, z, w]."""
    o = next(o for o in d.call("list_occurrences")["occurrences"]
             if o["id"] == occ)
    x, y, z, w = o["transform"]["orientation"]
    return (1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y))


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="lobole_asm_exprs_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        d.call("add_part", name="block")
        box(d, (0, 0, 0), (4, 4, 4), "Blk")

        # --- A part keeps refusing a text that does not parse -------------
        refused(d, "add_feature", "len *",
                feature={"type": "Box", "name": "Bad", "on": "Top",
                         "width": 2.0, "depth": 2.0, "height": 2.0,
                         "width_expr": "len *"})

        d.call("set_variable", name="lid_open", expression="30")
        d.call("set_variable", name="swing", expression="120")
        d.call("set_variable", name="gap", expression="3")
        d.call("add_assembly", name="box")
        base = d.call("add_occurrence", part="block", fixed=True)["id"]
        lid = d.call("add_occurrence", part="block")["id"]

        # --- A mate's value bound to a variable ----------------------------
        made = d.call("add_mate", type="revolute", name="hinge",
                      a={"occurrence": base, "origin": True},
                      b={"occurrence": lid, "origin": True}, flipped=False,
                      rotation_deg_expr="lid_open", offset=5.0)
        expect(abs(made["rotation_deg"] - 30.0) < 1e-9, f"30 degrees: {made}")
        expect(made.get("rotation_deg_expr") == "lid_open",
               f"the text answers beside the number: {made}")
        expect("rotation_expr" not in made,
               "the degrees text does not stand beside the radians")

        r = d.call("set_variable", name="lid_open", expression="90")
        expect(r.get("regen", {}).get("ok", True), f"a clean change: {r}")
        h = mate(d, "hinge")
        expect(abs(h["rotation_deg"] - 90.0) < 1e-9, f"the hinge turned: {h}")
        x = lid_x(d, lid)
        expect(abs(x[1] - 1.0) < 1e-6, f"and the lid with it: {x}")

        # --- An unknown variable is refused, and changes nothing ----------
        refused(d, "set_mate", "nowhere", id=made["id"],
                rotation_deg_expr="nowhere + 1")
        refused(d, "set_mate", "does not read", id=made["id"],
                rotation_deg_expr="lid_open *")
        expect(mate(d, "hinge")["rotation_deg_expr"] == "lid_open",
               "the refused calls changed nothing")

        # --- Limits as expressions, and one narrowed past the mate --------
        d.call("set_mate", id=made["id"],
               rotation_range_deg=[-120.0, 120.0],
               rotation_range_deg_expr=["-swing", "swing"])
        h = mate(d, "hinge")
        expect(h["limits"].get("rotation_range_deg_expr") == ["-swing", "swing"],
               f"the range's texts answer: {h['limits']}")
        r = d.call("set_variable", name="swing", expression="40")
        reg = r.get("regen", {})
        expect(reg.get("ok") is False, f"set_variable reports the mate: {r}")
        rows = reg.get("assemblies", [])
        expect(rows and rows[0]["assembly"] == "box", f"its assembly: {rows}")
        expect(rows[0]["failed"][0]["owner"] == "mate 'hinge'",
               f"the mate, by name: {rows}")
        expect("outside its range" in rows[0]["failed"][0]["error"],
               f"and why: {rows}")
        h = mate(d, "hinge")
        expect(abs(h["limits"]["rotation_range_deg"][1] - 120.0) < 1e-9,
               f"the range kept its last good value: {h['limits']}")
        expect(h["status"] == "error" and "outside its range" in h["error"],
               f"the mate carries the error: {h}")
        r = d.call("set_variable", name="swing", expression="100")
        expect(r.get("regen", {}).get("ok", True), f"widened, clean: {r}")
        h = mate(d, "hinge")
        expect(abs(h["limits"]["rotation_range_deg"][1] - 100.0) < 1e-9,
               f"the texts applied again: {h['limits']}")
        expect(h["status"] != "error", f"and the error went: {h}")

        # --- A number alone unbinds -------------------------------------
        d.call("set_mate", id=made["id"], rotation_deg=45.0)
        expect("rotation_deg_expr" not in mate(d, "hinge"),
               "a number given alone is a literal again")
        d.call("set_mate", id=made["id"], rotation_deg_expr="lid_open / 2")

        # --- The other owners -------------------------------------------
        study = d.call("add_motion_study", driver=made["id"],
                       **{"from": 0.0}, to=90.0, to_expr="swing",
                       samples_expr="gap * 2")
        expect(study.get("to_expr") == "swing" and abs(study["to"] - 100.0)
               < 1e-9, f"a study's range from a variable, in degrees: {study}")
        expect(study["samples"] == 6 and study.get("samples_expr") ==
               "gap * 2", f"and its samples: {study}")
        refused(d, "set_motion_study", "whole number", study=study["id"],
                samples_expr="gap / 2")
        view = d.call("add_exploded_view", name="apart")["id"]
        step = d.call("set_explode_step", view=view, occurrence=lid,
                      offset=[0.0, 0.0, 0.0], offset_expr=["", "", "gap * 4"])
        expect(step["steps"][0]["offset"][2] == 12.0 and
               step["steps"][0].get("offset_expr") == ["", "", "gap * 4"],
               f"an explode step's offset: {step}")
        seq = d.call("set_assembly_sequence", steps=[{"occurrence": lid}],
                     allowance_mm=0.0, allowance_mm_expr="gap / 50")
        expect(abs(seq["allowance_mm"] - 0.06) < 1e-12 and
               seq.get("allowance_mm_expr") == "gap / 50",
               f"the sequence's allowance: {seq}")

        # --- Rename reaches them; delete is refused naming them ----------
        d.call("rename_variable", name="gap", new_name="clearance")
        expect(next(s for s in d.call("list_motion_studies")["studies"]
                    if s["id"] == study["id"])["samples_expr"] ==
               "clearance * 2", "the study's text followed the rename")
        msg = refused(d, "delete_variable", "box: motion study",
                      name="clearance")
        expect("box: exploded view 'apart'" in msg and
               "the assembly sequence" in msg, f"every owner named: {msg}")

        # --- Literals: an assembly's numbers against the variables -------
        d.call("set_variable", name="seat", expression="5")
        lint = d.call("check_literals", part="box")
        hits = [f for f in lint["literals"] if f["field"] == "mates/0/offset"]
        expect(hits and "seat" in hits[0]["variables"],
               f"the hinge's 5 mm seat is the variable seat: {lint}")
        plan = d.call("extract_variables", reuse=True,
                      fields=["mates/0/offset"])
        expect(plan["variables"] and plan["variables"][0]["name"] == "seat" and
               plan["variables"][0]["existing"], f"reuse binds seat: {plan}")
        d.call("extract_variables", reuse=True, fields=["mates/0/offset"],
               apply=True)
        expect(mate(d, "hinge").get("offset_expr") == "seat",
               "applied: the seat is the variable now")

        # --- Save, reopen: every text where it was -----------------------
        proj = os.path.join(tmp, "box.lob")
        d.call("save_project", path=proj)
        d.call("new_project")
        d.call("open_project", path=proj)
        d.call("set_active_part", name="box")
        h = mate(d, "hinge")
        expect(h.get("rotation_deg_expr") == "lid_open / 2" and
               h.get("offset_expr") == "seat" and
               h["limits"].get("rotation_range_deg_expr") ==
               ["-swing", "swing"], f"reopened with its texts: {h}")
        expect(abs(h["rotation_deg"] - 45.0) < 1e-9,
               f"and the value they give: {h}")
        print("assembly_exprs_smoke: OK")
    finally:
        app.terminate()


if __name__ == "__main__":
    main()
