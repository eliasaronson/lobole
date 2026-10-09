#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Derived feature over the RPC, headless (docs/modeling/derived.md):
a jaw derives a servo as a ghost; the refusals in words; stale reported and
never auto-resolved; update_derived as one undo step; derive_occurrence in
an in-context edit lands the copy where the neighbour is; the project
round-trips through disk with the copy intact.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refused, but not naming "
               f"'{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def part_id(d, name):
    return next(p["id"] for p in d.call("list_parts")["parts"]
                if p["name"] == name)


def derived_row(d):
    rows = [f for f in d.call("get_state")["features"]
            if f["type"] == "Derived"]
    expect(len(rows) == 1, f"one Derived row expected, got {len(rows)}")
    return rows[0]


def bodies(d):
    return d.call("get_state")["bodies"]


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_derived_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        d.call("add_part", name="servo")
        servo_body = box(d, (0, 0, 0), (20, 10, 5), "Body")["id"]
        servo = part_id(d, "servo")
        d.call("add_part", name="jaw")
        jaw = part_id(d, "jaw")

        # Refusals first, each naming what would be accepted.
        refused(d, "add_feature", "itself",
                feature={"type": "Derived", "name": "x", "source_part": jaw})
        refused(d, "add_feature", "servo",
                feature={"type": "Derived", "name": "x", "source_part": 999})
        refused(d, "add_feature", "no body created by feature 999",
                feature={"type": "Derived", "name": "x", "source_part": servo,
                         "bodies": [999]})
        refused(d, "derive_occurrence", "edit_in_context", occurrence=1)

        # The copy: one ghost body, the servo's volume, weightless, not
        # exportable, fresh.
        r = d.call("add_feature", feature={
            "type": "Derived", "name": "servo copy", "source_part": servo})
        did = r["id"]
        expect(r["regen"]["ok"], f"derived did not regenerate: {r['regen']}")
        row = derived_row(d)
        expect(row["stale"] is False, f"fresh copy reads {row['stale']!r}")
        expect(row["source_name"] == "servo", row)
        b = bodies(d)
        expect(len(b) == 1 and abs(b[0]["volume"] - 1000.0) < 1e-6, b)
        refused(d, "mass_properties", "reference: false")
        refused(d, "export_step", "reference",
                path=os.path.join(tmp, "jaw.step"))
        # Matter of the jaw's own beside the ghost: only it counts.
        # z -4..0: on Top, extruded down.
        box(d, (0, 0, 0), (20, 10, 4), "Jaw", reversed=True)
        mass = d.call("mass_properties")
        expect(abs(mass["total"]["volume"] - 800.0) < 1e-6, mass)
        expect(len(mass["bodies"]) == 1, mass)
        chk = d.call("check_geometry")
        expect("stale_derived" not in chk, chk)

        # The source moves. The copy says so and stays as it was.
        d.call("set_active_part", part="servo")
        # Widen the body 20 -> 40 along X: its sketch, solved by hand (the
        # far side moves and the width dimension says so).
        sk = next(f for f in d.call("get_state")["features"]
                  if f["type"] == "Sketch")
        for e in sk["entities"]:
            for end in ("a", "b"):
                if abs(e[end][0] - 20.0) < 1e-9:
                    e[end][0] = 40.0
        for c in sk["constraints"]:
            if c["kind"] == "distance_x" and abs(c["value"] - 20.0) < 1e-9:
                c["value"] = 40.0
        d.call("modify_feature", feature_id=sk["id"], feature=sk)
        d.call("set_active_part", part="jaw")
        row = derived_row(d)
        expect(row["stale"] is True, f"moved source not reported: {row}")
        expect(abs(bodies(d)[0]["volume"] - 1000.0) < 1e-6, "auto-updated!")
        chk = d.call("check_geometry")
        expect([s["id"] for s in chk.get("stale_derived", [])] == [did], chk)

        # update_derived: the new geometry, fresh, one undo step.
        r = d.call("update_derived", feature_id=did)
        expect(r["updated"] == [did] and r["regen"]["ok"], r)
        expect(abs(bodies(d)[0]["volume"] - 2000.0) < 1e-6, bodies(d))
        expect(derived_row(d)["stale"] is False, derived_row(d))
        d.call("undo")
        expect(abs(bodies(d)[0]["volume"] - 1000.0) < 1e-6, bodies(d))
        expect(derived_row(d)["stale"] is True, derived_row(d))
        r = d.call("update_derived", all=True)
        expect(r["updated"] == [did], r)
        expect(abs(bodies(d)[0]["volume"] - 2000.0) < 1e-6, bodies(d))

        # A cycle: the servo may not derive from the jaw that derives it.
        d.call("set_active_part", part="servo")
        refused(d, "add_feature", "itself",
                feature={"type": "Derived", "name": "x", "source_part": jaw})

        # In context: the jaw sits 50 mm along +X in the assembly, so the
        # servo derived from there lands 50 mm along -X in the jaw's frame.
        d.call("add_assembly", name="grip")
        servo_occ = d.call("add_occurrence", part="servo", fixed=True)["id"]
        jaw_occ = d.call("add_occurrence", part="jaw",
                         transform={"position": [50, 0, 0]})["id"]
        d.call("edit_in_context", occurrence=jaw_occ)
        refused(d, "derive_occurrence", "not a neighbour", occurrence=jaw_occ)
        r = d.call("derive_occurrence", occurrence=servo_occ, name="servo here")
        expect(r["regen"]["ok"], r)
        placed = next(b for b in bodies(d) if b["name"] == "Body" and
                      abs(b["bbox_min"][0] + 50.0) < 1e-3)
        expect(abs(placed["bbox_max"][0] + 10.0) < 1e-3, placed)  # 40 wide
        feats = d.call("get_state")["features"]
        here = next(f for f in feats if f["name"] == "servo here")
        expect(abs(here["placement"]["position"][0] + 50.0) < 1e-6, here)
        expect(here["stale"] is False, here)
        # The copy sits exactly on the real servo; the assembly's contact
        # report must not read the ghost as the jaw's matter.
        d.call("set_active_part", part="grip")
        rep = d.call("contact_report")
        expect(rep.get("clashes", []) == [], rep.get("clashes"))
        d.call("set_active_part", part="jaw")

        # A drawing of the jaw is a drawing of the jaw: the ghost projects no
        # curve (the HLR of a 197-body ghost took a view past a minute).
        d.call("create_drawing", name="jaw sheet", sheet={"size": "a4"})
        view = d.call("add_drawing_view", source="jaw", orientation="top")["id"]
        geom = d.call("list_drawing_geometry", view=view)
        owners = {e["ref"]["feature_id"] for e in geom.get("geometry", [])}
        expect(owners and did not in owners and here["id"] not in owners,
               f"ghost curves in the drawing: {owners}")
        d.call("delete_part", part="jaw sheet")

        # Through disk: the copies come back without touching the source.
        path = os.path.join(tmp, "grip.dsgn")
        d.call("save_project", path=path)
        d.call("new_project")
        d.call("open_project", path=path)
        d.call("set_active_part", part="jaw")
        feats = d.call("get_state")["features"]
        expect([f["type"] for f in feats] ==
               ["Derived", "Sketch", "Extrude", "Derived"],
               [f["type"] for f in feats])
        expect(all(f["stale"] is False for f in feats if f["type"] == "Derived"),
               feats)
        vols = sorted(round(b["volume"], 3) for b in bodies(d))
        expect(vols == [800.0, 2000.0, 2000.0], vols)
        # v85: pick the source's bodies by NAME. One Import creates every
        # body of a STEP (a camera module arrives as 197), so the created_by
        # filter cannot reach into it.
        d.call("set_active_part", part="servo")
        box(d, (0, 0, 20), (5, 5, 5), "Lug")
        d.call("set_active_part", part="jaw")
        r = d.call("add_feature", feature={
            "type": "Derived", "name": "lug only", "source_part": servo,
            "body_names": ["Lug"]})
        expect(r["regen"]["ok"], r)
        lug = [b for b in bodies(d) if b["name"].endswith("Lug")]
        expect(len(lug) == 1 and abs(lug[0]["volume"] - 125.0) < 1e-6,
               f"only the named body came across: {bodies(d)}")
        refused(d, "add_feature", "no body named 'Flange'", feature={
            "type": "Derived", "name": "x", "source_part": servo,
            "body_names": ["Flange"]})
        refused(d, "add_feature", "narrow TOGETHER", feature={
            "type": "Derived", "name": "x", "source_part": servo,
            "bodies": [servo_body], "body_names": ["Lug"]})

        # v133: a copy taken in a configuration the source does not have
        # active -- one member of a family -- named, stored as its id, and
        # judged fresh against that configuration, not the one showing.
        d.call("set_active_part", part="servo")
        lug_id = next(f["id"] for f in d.call("get_state")["features"]
                      if f["name"] == "Lug")
        bare = d.call("add_configuration", name="bare")["id"]
        d.call("set_configuration", id=bare, suppressed_features=[lug_id])
        d.call("set_active_part", part="jaw")
        refused(d, "add_feature", "(it has: bare)", feature={
            "type": "Derived", "name": "x", "source_part": servo,
            "configuration": "bear"})
        r = d.call("add_feature", feature={
            "type": "Derived", "name": "bare servo", "source_part": servo,
            "configuration": "bare", "reference": False})
        expect(r["regen"]["ok"], r)
        row = next(f for f in d.call("get_state")["features"]
                   if f["name"] == "bare servo")
        expect(row.get("configuration") == bare, row)
        expect(row["stale"] is False, f"a configured copy reads stale: {row}")
        mine = [b for b in bodies(d) if b["name"].endswith("Lug")]
        expect(len(mine) == 1, f"the bare copy brought a lug: {bodies(d)}")
        # The source's tab never left its authored model.
        d.call("set_active_part", part="servo")
        expect(d.call("list_configurations")["active"] == 0,
               "taking the copy switched the source's configuration")
        expect(len(bodies(d)) == 2, bodies(d))

        print("derived_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
