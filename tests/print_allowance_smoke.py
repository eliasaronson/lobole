#!/usr/bin/env python3
"""Integration test: the printing process allowance over RPC, headless.

docs/tolerances/README.md#printed-parts. What only the RPC layer shows: a part
declares how it is printed, and check_fit, check_fits, print_allowance and a
Thread's add_feature answer say what to MODEL for it — and the advice they
give, followed, is what they then accept.

Usage: print_allowance_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402

PETG = {"process": "fdm", "material": "petg"}


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def near(a, b, tol=1e-6):
    return abs(a - b) < tol


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e),
               f"{method} refused, but not naming '{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def round_faces(d):
    return [(b["created_by"], f["name"])
            for b in d.call("list_elements")["bodies"]
            for f in b.get("faces", []) if f.get("kind") == "cylinder"]


def bored_part(d, name, diameter):
    """A block with one bore through it; returns the bore face."""
    d.call("add_part", name=name)
    box(d, (-15, -15, 0), (30, 30, 12), "Body")
    d.call("add_feature", feature={
        "type": "Sketch", "name": "Centre",
        "plane": {"origin": [0, 0, 12], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [0, 0], "radius": 2}]})
    sk = {f["name"]: f["id"] for f in d.call("get_state")["features"]}["Centre"]
    d.call("add_feature", feature={
        "type": "Hole", "name": "Bore", "hole_kind": "simple",
        "sketch_id": sk, "diameter": diameter, "through": True})
    faces = round_faces(d)
    expect(len(faces) == 1, f"one bore: {faces}")
    return faces[0]


def pin(d, name, diameter):
    d.call("add_part", name=name)
    cylinder(d, (0, 0, 0), "z", diameter / 2.0, 30, "Pin")
    return round_faces(d)[0]


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_print_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")

        # --- Asking before there is a model --------------------------------
        r = d.call("check_fit", size=8.0,
                   hole={"class": "H7", "print": PETG})
        expect(near(r["print"]["hole"]["model"], 8.2), f"model Ø8.2: {r}")
        expect(r["print_notes"][0] ==
               "H7 on a printed Ø8 bore (fdm petg): model Ø8.2",
               f"the sentence: {r}")
        expect("finer than fdm holds" in r["print"]["hole"]["band_note"],
               f"H7 is finer than a print holds: {r}")
        expect("coupon" in r["print"]["hole"]["confirm"], f"confirm: {r}")

        r = d.call("print_allowance", kind="internal_thread",
                   designation="M8", **PETG)
        expect(near(r["model_mm"], 8.4) and near(r["pitch"], 1.25),
               f"a printed M8 nut thread: {r}")
        r = d.call("print_allowance", kind="hole", size_mm=12, **PETG)
        expect(near(r["model_mm"], 12.3), f"a Ø12 hole: {r}")
        expect(near(r["stl_chord_loss_mm"], 0.2),
               f"export_stl's default chords take up to 0.2: {r}")

        # --- Refused, naming what is accepted ------------------------------
        refused(d, "print_allowance", "Accepted: hole, shaft", kind="bore",
                size_mm=8, **PETG)
        refused(d, "print_allowance", "Accepted: fdm", kind="hole",
                size_mm=8, process="sla", material="petg")
        refused(d, "print_allowance", "Accepted for fdm: petg", kind="hole",
                size_mm=8, process="fdm", material="pla")
        refused(d, "check_fit", "Accepted for fdm: petg", size=8.0,
                hole={"class": "H7",
                      "print": {"process": "fdm", "material": "abs"}})

        # --- A printed bracket and a steel pin ------------------------------
        pf, pname = pin(d, "Pin", 8.0)
        d.call("set_tolerance", feature_id=pf, element=pname,
               **{"class": "g6"})
        # Modelled Ø8.2 because it is printed: that prints Ø8.
        bf, bname = bored_part(d, "Bracket", 8.2)
        refused(d, "set_print_process", "needs both", process="fdm")
        refused(d, "set_print_process", "Accepted: fdm", process="sls",
                material="petg")
        refused(d, "set_print_process", "Accepted for fdm: petg",
                process="fdm", material="pla")
        state0 = d.call("get_state")
        r = d.call("set_print_process", **PETG)
        expect(r["print"] == PETG, f"declared: {r}")
        expect(d.call("get_print_process")["print"] == PETG, "reads back")
        expect(len(d.call("get_state")["features"]) ==
               len(state0["features"]), "rebuilds nothing, adds nothing")
        d.call("set_tolerance", feature_id=bf, element=bname,
               **{"class": "H7"})

        r = d.call("check_fit", hole={"class": "H7", "feature_id": bf,
                                      "element": bname})
        expect(near(r["size"], 8.0) and r["size_from"] == "printed",
               f"the bore is at what it prints: {r}")
        expect(r["print"]["hole"]["compensated"], f"compensated: {r}")

        # One measured face in each part: the fit is at Ø8, not two sizes.
        r = d.call("check_fit",
                   hole={"class": "H7", "feature_id": bf, "element": bname,
                         "part": "Bracket"},
                   shaft={"class": "g6", "feature_id": pf, "element": pname,
                          "part": "Pin"})
        expect(near(r["size"], 8.0) and r["kind"] == "clearance",
               f"H7/g6 at the printed Ø8: {r}")
        expect("shaft" not in r["print"], f"the steel pin is not printed: {r}")

        # --- check_fits off the mate --------------------------------------
        d.call("add_assembly", name="Joint")
        ob = d.call("add_occurrence", part="Bracket")["id"]
        op = d.call("add_occurrence", part="Pin")["id"]
        d.call("add_mate", type="revolute", name="Pivot",
               a={"occurrence": ob, "face": {"feature_id": bf,
                                             "element": bname}},
               b={"occurrence": op, "face": {"feature_id": pf,
                                             "element": pname}})
        fit = d.call("check_fits")["fits"][0]
        expect(fit["status"] == "pass", f"H7/g6 turns: {fit}")
        expect(near(fit["size"], 8.0), f"at the printed size: {fit}")
        expect(near(fit["hole"]["print"]["model"], 8.2),
               f"and says what to model: {fit}")

        # A printed bore modelled at the BARE size is told what to model.
        bf2, bname2 = bored_part(d, "Bare", 8.0)
        d.call("set_print_process", **PETG)
        d.call("set_tolerance", feature_id=bf2, element=bname2,
               **{"class": "H7"})
        refused(d, "check_fit", "model the hole 8.2",
                hole={"class": "H7", "feature_id": bf2, "element": bname2},
                shaft={"class": "g6", "feature_id": pf, "element": pname,
                       "part": "Pin"})
        d.call("set_active_part", name="Joint")
        ob2 = d.call("add_occurrence", part="Bare")["id"]
        d.call("add_mate", type="revolute", name="Bare pivot",
               a={"occurrence": ob2, "face": {"feature_id": bf2,
                                              "element": bname2}},
               b={"occurrence": op, "face": {"feature_id": pf,
                                             "element": pname}})
        bare = [f for f in d.call("check_fits")["fits"]
                if f["mate_name"] == "Bare pivot"][0]
        expect(bare["status"] == "unverified", f"two basic sizes: {bare}")
        expect("model the bore Ø8.2" in bare["reason"],
               f"and the reason says what to model: {bare}")

        # --- A printed thread ---------------------------------------------
        tf, tname = bored_part(d, "Nut", 6.75)  # M8's tap drill
        d.call("set_print_process", **PETG)
        r = d.call("add_feature", feature={
            "type": "Thread", "name": "M8", "designation": "M8",
            "face": {"feature_id": tf, "element": tname}})
        pr = r["print"]
        expect(pr["side"] == "internal" and pr["known"], f"internal: {pr}")
        expect(near(pr["model_major"], 8.4), f"model the major Ø8.4: {pr}")
        expect(near(pr["prints_major"], 7.6), f"as modelled: {pr}")
        expect(near(pr["model_face"], 6.647 + 0.4, 1e-3),
               f"as a custom thread's bore: {pr}")
        expect(any("cosmetic" in n for n in pr["notes"]),
               f"a cosmetic thread prints nothing: {pr}")
        threads = d.call("get_print_process")["threads"]
        expect(len(threads) == 1, f"listed with the part: {threads}")

        # Follow the advice: a custom thread on the bore it names prints the
        # M8's major.
        nut2 = bored_part(d, "Nut2", round(pr["model_face"], 3))
        d.call("set_print_process", **PETG)
        r = d.call("add_feature", feature={
            "type": "Thread", "name": "Printed M8",
            "designation": "printed nut thread", "pitch": 1.25,
            "face": {"feature_id": nut2[0], "element": nut2[1]}})
        pr = r["print"]
        expect(not pr["known"], f"a custom thread: {pr}")
        expect(near(pr["prints_major"], 8.0, 1e-3),
               f"the advised custom thread prints the M8 major: {pr}")

        # Undo takes the process back off.
        d.call("undo")
        d.call("undo")
        expect(d.call("get_print_process")["print"] is None,
               "undo clears the process")

        print("print_allowance_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
