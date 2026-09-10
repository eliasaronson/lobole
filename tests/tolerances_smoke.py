#!/usr/bin/env python3
"""Integration test: tolerances and fits over RPC, driven headless.

Covers what only the RPC layer can show: a tolerance surviving as design
intent (set/list/clear/undo), and check_fits pairing faces off the MATES
rather than off a pairing table — including the two things that make it
honest, which are that the mate type supplies the intent and that anything
unanswerable comes back unverified instead of passing.

The sub-assembly case is here rather than in a unit test because reaching
the part behind a mate anchor is a walk through three documents, and every
earlier version of that walk stopped at the first one.

Usage: tolerances_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e),
               f"{method} refused, but not naming '{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def bore_face(d):
    """The one cylindrical face of the active part, as (feature_id, name)."""
    found = [(b["created_by"], f["name"])
             for b in d.call("list_elements")["bodies"]
             for f in b.get("faces", []) if f.get("kind") == "cylinder"]
    expect(len(found) == 1, f"expected one cylindrical face, got {found}")
    return found[0]


def housing(d, name, diameter=20.0):
    """A block with one bore drilled through it."""
    d.call("add_part", name=name)
    d.call("add_feature", feature={
        "type": "Box", "name": "Body", "position": [-20, -20, 0],
        "width": 40, "height": 40, "depth": 30})
    d.call("add_feature", feature={
        "type": "Sketch", "name": "Centre",
        "plane": {"origin": [0, 0, 30], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [0, 0], "radius": 5}]})
    sk = {f["name"]: f["id"] for f in d.call("get_state")["features"]}["Centre"]
    d.call("add_feature", feature={
        "type": "Hole", "name": "Bore", "hole_kind": "simple",
        "sketch_id": sk, "diameter": diameter, "through": True})
    return bore_face(d)


def shaft(d, name, diameter=20.0):
    d.call("add_part", name=name)
    d.call("add_feature", feature={
        "type": "Cylinder", "name": "Journal", "center": [0, 0, 0],
        "direction": [0, 0, 1], "radius": diameter / 2.0, "height": 60})
    return bore_face(d)


def only_fit(d):
    r = d.call("check_fits")
    expect(len(r["fits"]) == 1, f"expected exactly one fit: {r}")
    return r, r["fits"][0]


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_tolerances_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")

        # --- A tolerance is design intent about one face ------------------
        hf, hname = housing(d, "Housing")
        # `class` is a python keyword, hence the dict splat throughout.
        r = d.call("set_tolerance", feature_id=hf, element=hname,
                   **{"class": "H7"})
        expect(r["nominal"] == 20.0, f"measured nominal: {r}")
        expect(abs(r["max"] - 20.021) < 1e-9, f"H7 upper limit: {r}")
        expect(abs(r["min"] - 20.0) < 1e-9, f"H7 lower limit: {r}")

        listed = d.call("list_tolerances")["tolerances"]
        expect(len(listed) == 1, f"one tolerance: {listed}")
        expect(listed[0]["resolved"], f"resolves: {listed}")

        # One per element: a second set REPLACES rather than stacking a
        # contradiction the checker would have to choose between.
        d.call("set_tolerance", feature_id=hf, element=hname, **{"class": "H8"})
        listed = d.call("list_tolerances")["tolerances"]
        expect(len(listed) == 1, f"still one tolerance: {listed}")
        expect(listed[0]["class"] == "H8", f"replaced: {listed}")
        d.call("set_tolerance", feature_id=hf, element=hname, **{"class": "H7"})

        # Refused rather than stored wrong.
        refused(d, "set_tolerance", "not both", feature_id=hf, element=hname,
                lower=0.0, upper=0.1, **{"class": "H7"})
        refused(d, "set_tolerance", "come as a pair", feature_id=hf,
                element=hname, lower=0.05)
        refused(d, "set_tolerance", "must not exceed", feature_id=hf,
                element=hname, lower=0.1, upper=0.0)
        refused(d, "set_tolerance", "IT4 to IT13", feature_id=hf,
                element=hname, **{"class": "H99"})

        # --- check_fits pairs off the MATES -------------------------------
        sf, sname = shaft(d, "Shaft")
        d.call("set_tolerance", feature_id=sf, element=sname,
               **{"class": "g6"})

        d.call("add_assembly", name="Rig")
        oh = d.call("add_occurrence", part="Housing")["id"]
        os_ = d.call("add_occurrence", part="Shaft")["id"]
        d.call("add_mate", type="revolute", name="Journal",
               a={"occurrence": oh, "face": {"feature_id": hf,
                                             "element": hname}},
               b={"occurrence": os_, "face": {"feature_id": sf,
                                              "element": sname}})

        r, fit = only_fit(d)
        expect(fit["status"] == "pass", f"H7/g6 on a revolute: {fit}")
        expect(fit["kind"] == "clearance", f"H7/g6 kind: {fit}")
        expect(abs(fit["min_clearance"] - 0.007) < 1e-9, f"min: {fit}")
        expect(abs(fit["max_clearance"] - 0.041) < 1e-9, f"max: {fit}")
        expect(r["status"] == "pass" and r["passed"] == 1, f"summary: {r}")

        # The mate type IS the intent: a joint that turns cannot be pressed.
        def shaft_class(cls):
            d.call("set_active_part", name="Shaft")
            d.call("set_tolerance", feature_id=sf, element=sname,
                   **{"class": cls})
            d.call("set_active_part", name="Rig")
            return only_fit(d)[1]

        fit = shaft_class("p6")
        expect(fit["status"] == "fail", f"interference on a revolute: {fit}")
        expect(fit["kind"] == "interference", f"p6 kind: {fit}")
        expect("turns" in fit["reason"], f"names the contradiction: {fit}")

        fit = shaft_class("k6")
        expect(fit["status"] == "warn", f"transition on a revolute: {fit}")
        expect(fit["kind"] == "transition", f"k6 kind: {fit}")

        shaft_class("g6")

        # A fastened joint says nothing about which fit it wants, so its fit
        # is reported and NOT judged — the verdict would be invented.
        d.call("set_active_part", name="Rig")
        mate = d.call("list_mates")["mates"][0]["id"]
        d.call("remove_mate", id=mate)
        d.call("add_mate", type="fastened", name="Pressed",
               a={"occurrence": oh, "face": {"feature_id": hf,
                                             "element": hname}},
               b={"occurrence": os_, "face": {"feature_id": sf,
                                              "element": sname}})
        r, fit = only_fit(d)
        expect(fit["status"] == "reported", f"fastened is not judged: {fit}")
        expect(r["failed"] == 0 and r["passed"] == 0, f"summary: {r}")
        expect(r["reported"] == 1, f"counted apart: {r}")

        # --- Unanswerable is never a pass ---------------------------------
        d.call("set_active_part", name="Shaft")
        tol = d.call("list_tolerances")["tolerances"][0]["id"]
        d.call("clear_tolerance", id=tol)
        d.call("set_active_part", name="Rig")
        r, fit = only_fit(d)
        expect(fit["status"] == "unverified", f"one side untoleranced: {fit}")
        expect("carries no tolerance" in fit["reason"], f"reason: {fit}")
        expect(r["status"] == "unverified", f"unverified outranks: {r}")

        # ...and undo brings the tolerance back, across tabs.
        d.call("set_active_part", name="Shaft")
        d.call("undo")
        d.call("set_active_part", name="Rig")
        expect(only_fit(d)[1]["status"] == "reported", "undo restored it")

        # --- A fit inside a sub-assembly ----------------------------------
        # The bore now lives one level down: an outer assembly places "Rig"
        # as a unit, and the mate anchors through it with an inner_path. The
        # part behind that anchor is three documents away, and a fit is
        # between two SIZES, so the walk has to reach it without caring where
        # the unit was placed.
        d.call("add_assembly", name="Machine")
        unit = d.call("add_occurrence", part="Rig")["id"]
        pin = d.call("add_occurrence", part="Shaft")["id"]
        d.call("add_mate", type="revolute", name="Through the unit",
               a={"occurrence": unit, "inner_path": [oh],
                  "face": {"feature_id": hf, "element": hname}},
               b={"occurrence": pin, "face": {"feature_id": sf,
                                              "element": sname}})
        r, fit = only_fit(d)
        expect(fit["status"] == "pass",
               f"a fit one level down still resolves: {fit}")
        expect("/" in fit["hole"]["occurrence"],
               f"and the side is named by its path: {fit}")
        expect(abs(fit["min_clearance"] - 0.007) < 1e-9,
               f"same numbers as at the top level: {fit}")

        # --- A tolerance on a LENGTH (T5a) --------------------------------
        # Diameters are the easy half. A distance tolerance names a second
        # face and the nominal is measured between them.
        d.call("add_part", name="Plate")
        d.call("add_feature", feature={
            "type": "Box", "name": "Slab", "position": [0, 0, 0],
            "width": 40, "height": 40, "depth": 10})
        faces = [f["name"] for b in d.call("list_elements")["bodies"]
                 for f in b.get("faces", []) if f.get("kind") == "plane"]
        expect(len(faces) == 6, f"a box has six planar faces: {faces}")
        pid = {f["name"]: f["id"]
               for f in d.call("get_state")["features"]}["Slab"]

        r = d.call("set_tolerance", feature_id=pid, element=faces[0],
                   b={"feature_id": pid, "element": faces[1]},
                   lower=-0.1, upper=0.1, comment="plate width")
        expect(r["kind"] == "distance", f"tagged as a distance: {r}")
        expect(abs(r["nominal"] - 40.0) < 1e-9, f"measured width: {r}")
        expect(abs(r["min"] - 39.9) < 1e-9, f"lower limit: {r}")
        expect(abs(r["max"] - 40.1) < 1e-9, f"upper limit: {r}")

        listed = d.call("list_tolerances")["tolerances"]
        expect(len(listed) == 1, f"one tolerance on Plate: {listed}")
        expect(listed[0]["kind"] == "distance", f"kind survives: {listed}")
        expect(listed[0]["b"]["element"] == faces[1], f"both refs: {listed}")

        # A face's own size and a distance measured from it are different
        # subjects, so they coexist rather than replacing one another.
        d.call("set_tolerance", feature_id=pid, element=faces[0],
               b={"feature_id": pid, "element": faces[2]},
               lower=-0.2, upper=0.2)
        expect(len(d.call("list_tolerances")["tolerances"]) == 2,
               "a second distance from the same face is its own subject")

        refused(d, "set_tolerance", "an ISO class is a SIZE", feature_id=pid,
                element=faces[0], b={"feature_id": pid, "element": faces[1]},
                **{"class": "H7"})
        refused(d, "set_tolerance", "no distance from itself", feature_id=pid,
                element=faces[0], b={"feature_id": pid, "element": faces[0]},
                lower=-0.1, upper=0.1)
        refused(d, "set_tolerance", "no size of its own", feature_id=pid,
                element=faces[0], lower=-0.1, upper=0.1)
        refused(d, "set_tolerance", "needs both", feature_id=pid,
                element=faces[0], b={"feature_id": pid}, lower=0.0, upper=0.1)

        print("tolerances_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
