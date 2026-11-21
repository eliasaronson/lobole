#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: a CAM job's and a drawing's numbers as expressions,
over JSON-RPC.

docs/cam/README.md#numbers-that-are-expressions and
docs/drawings/README.md#numbers-that-are-expressions: a tool diameter bound
with `diameter_expr` follows its variable and the toolpath is made again
from it; a value the tool cannot take is reported in set_variable's OWN
`regen` (`jobs`) and keeps its last good value; an unknown variable is
refused before anything changes, naming the key; a view's scale bound with
`scale_expr` rescales the view; rename reaches both, the delete refusal
names both, and a save -> reopen keeps every text.

Usage: job_drawing_exprs_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
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


def setups(d, job):
    return d.call("list_cam_setups", job=job)["setups"]


def cut_mm(d, job, setup):
    ops = d.call("cam_toolpath", job=job, setup=setup)["operations"]
    return ops[0]["cut_length_mm"]


def the_view(d, drawing, vid):
    d.call("set_active_part", name=drawing)
    return next(v for v in d.call("list_drawing_views")["views"]
                if v["id"] == vid)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="lobole_job_drawing_exprs_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        cam = d.call("ping").get("features", {}).get("cam", True)
        d.call("new_project")
        d.call("set_variable", name="cutter", expression="6")
        d.call("set_variable", name="sheet_scale", expression="0.5")
        d.call("set_variable", name="left", expression="60")
        box(d, (0, 0, 0), (80, 50, 10), "Plate")
        part = d.call("list_parts")["parts"][0]["name"]

        job = None
        if cam:
            # --- A tool diameter bound to a variable ---------------------
            job = d.call("machine_part")["name"]
            setup = d.call("add_cam_setup", job=job, name="Top",
                           safe_z_expr="cutter + 4")["id"]
            d.call("set_cam_setup", job=job, id=setup,
                   tool={"number": 1, "diameter_expr": "cutter",
                         "flute_length": 25})
            got = setups(d, job)[0]
            t = got["tools"][0]
            expect(abs(t["diameter"] - 6.0) < 1e-9 and
                   t.get("diameter_expr") == "cutter",
                   f"the diameter from the variable, its text beside it: {t}")
            expect(abs(got["safe_z"] - 10.0) < 1e-9 and
                   got.get("safe_z_expr") == "cutter + 4",
                   f"safe Z from its text: {got['safe_z']}")
            op = d.call("add_cam_op", job=job, setup=setup, type="profile",
                        stepdown_expr="cutter / 2")
            expect(op.get("stepdown_expr") == "cutter / 2" and
                   abs(op["stepdown"] - 3.0) < 1e-9,
                   f"an op's stepdown from its text: {op}")
            before = cut_mm(d, job, setup)

            r = d.call("set_variable", name="cutter", expression="10")
            expect("jobs" not in r.get("regen", {}),
                   f"a diameter that stands is no problem: {r}")
            t = setups(d, job)[0]["tools"][0]
            expect(abs(t["diameter"] - 10.0) < 1e-9,
                   f"the diameter followed its variable: {t}")
            after = cut_mm(d, job, setup)
            expect(abs(after - before) > 1.0,
                   f"the toolpath was made from the new tool: {before} -> "
                   f"{after}")

            # A value the tool cannot take: its own answer says so, and
            # the tool keeps its last good diameter.
            r = d.call("set_variable", name="cutter", expression="0")
            jobs = r.get("regen", {}).get("jobs", [])
            expect(r["regen"]["ok"] is False and jobs and
                   "tool T1" in jobs[0]["failed"][0]["owner"] and
                   "diameter" in jobs[0]["failed"][0]["error"],
                   f"set_variable's regen names the tool: {r}")
            t = setups(d, job)[0]["tools"][0]
            expect(abs(t["diameter"] - 10.0) < 1e-9,
                   f"the last good diameter kept: {t}")
            d.call("set_variable", name="cutter", expression="8")

            # An unknown variable is refused, naming the key, and changes
            # nothing.
            refused(d, "set_cam_setup", "diameter", job=job, id=setup,
                    tool={"number": 1, "diameter_expr": "nope"})
            refused(d, "set_cam_op", "stepdown", job=job, id=op["id"],
                    stepdown_expr="(")
            t = setups(d, job)[0]["tools"][0]
            expect(t.get("diameter_expr") == "cutter",
                   f"a refused call changed nothing: {t}")
            # A number alone unbinds.
            d.call("set_job_placement", job=job, setup=setup,
                   part=d.call("list_job_parts", job=job)["parts"][0]["id"],
                   pos_expr=["left / 6", ""], pos=[0, 3])
            pl = d.call("list_job_parts", job=job)["parts"][0]["placements"]
            expect(pl[0]["pos"] == [10, 3] and
                   pl[0].get("pos_expr") == ["left / 6", ""],
                   f"a placement's x from its text, y a number: {pl}")

            # The literals lint reads a job too: an unbound number equal
            # to a variable is a finding.
            d.call("set_variable", name="reach", expression="25")
            lits = d.call("check_literals", part=job)["literals"]
            expect(any(f["field"] == "cam/0/tools/0/flute_length" and
                       "reach" in f["variables"] for f in lits),
                   f"the tool's flute length is a finding: {lits}")

        # --- A drawing view's scale and position -------------------------
        d.call("set_active_part", name=part)
        d.call("create_drawing", name="Sheet",
               sheet={"size": "a3", "landscape": True})
        vid = d.call("add_drawing_view", source=part, orientation="front",
                     scale_expr="sheet_scale", position_expr=["left * 2", ""],
                     position=[0, 120])["id"]
        v = the_view(d, "Sheet", vid)
        expect(abs(v["scale"] - 0.5) < 1e-9 and
               v.get("scale_expr") == "sheet_scale" and
               v["position"] == [120, 120] and
               v.get("position_expr") == ["left * 2", ""],
               f"the view's scale and x from their texts: {v}")
        d.call("set_variable", name="sheet_scale", expression="0.25")
        v = the_view(d, "Sheet", vid)
        expect(abs(v["scale"] - 0.25) < 1e-9,
               f"the view rescaled with its variable: {v}")
        r = d.call("set_variable", name="sheet_scale", expression="-1")
        drawings = r.get("regen", {}).get("drawings", [])
        expect(drawings and "scale" in drawings[0]["failed"][0]["error"],
               f"a scale that cannot stand is the view's error: {r}")
        expect(abs(the_view(d, "Sheet", vid)["scale"] - 0.25) < 1e-9,
               "the last good scale kept")
        d.call("set_variable", name="sheet_scale", expression="0.5")
        refused(d, "set_drawing_view", "scale", id=vid, scale_expr="nope")

        # --- Rename and delete reach both --------------------------------
        d.call("rename_variable", name="left", new_name="margin")
        v = the_view(d, "Sheet", vid)
        expect(v.get("position_expr") == ["margin * 2", ""],
               f"the view's text followed the rename: {v}")
        msg = refused(d, "delete_variable", "Sheet: view", name="margin")
        if cam:
            expect("placement" in msg, f"the job's owner named too: {msg}")
            pl = d.call("list_job_parts", job=job)["parts"][0]["placements"]
            expect(pl[0].get("pos_expr") == ["margin / 6", ""],
                   f"the placement's text followed the rename: {pl}")

        # --- Save, reopen: every text where it was -----------------------
        proj = os.path.join(tmp, "plate.lob")
        d.call("save_project", path=proj)
        d.call("new_project")
        d.call("open_project", path=proj)
        v = the_view(d, "Sheet", vid)
        expect(v.get("scale_expr") == "sheet_scale" and
               abs(v["scale"] - 0.5) < 1e-9,
               f"the view reopened with its text: {v}")
        if cam:
            got = setups(d, job)[0]
            expect(got["tools"][0].get("diameter_expr") == "cutter" and
                   abs(got["tools"][0]["diameter"] - 8.0) < 1e-9,
                   f"the tool reopened with its text: {got['tools'][0]}")
            expect(got["operations"][0].get("stepdown_expr") == "cutter / 2",
                   f"the op reopened with its text: {got['operations'][0]}")
            # And a variable changed after the reopen still reaches it.
            d.call("set_variable", name="cutter", expression="12")
            got = setups(d, job)[0]
            expect(abs(got["tools"][0]["diameter"] - 12.0) < 1e-9 and
                   abs(got["safe_z"] - 16.0) < 1e-9,
                   f"the reopened job follows its variables: {got}")
        print("job_drawing_exprs_smoke: OK")
    finally:
        app.terminate()


if __name__ == "__main__":
    main()
