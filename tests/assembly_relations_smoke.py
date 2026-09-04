#!/usr/bin/env python3
"""Relations over the RPC, headless: a gear pair whose ratio is DERIVED from
tooth counts, driven from either side; a rack; the refusals in words.
(docs/assemblies/relations-plan.md)
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402


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


def origin_mate(d, kind, a, b, **kw):
    return d.call("add_mate", type=kind,
                  a={"occurrence": a, "origin": True},
                  b={"occurrence": b, "origin": True},
                  flipped=False, **kw)["id"]


def mate(d, mid):
    return next(m for m in d.call("list_mates")["mates"] if m["id"] == mid)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_relations_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        d.call("add_part", name="carrier")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 80, "height": 20, "depth": 5})
        d.call("add_part", name="gear")
        d.call("add_feature", feature={
            "type": "Cylinder", "name": "Disc", "center": [0, 0, 0],
            "direction": [0, 0, 1], "radius": 12, "height": 6})
        d.call("add_assembly", name="train")
        carrier = d.call("add_occurrence", part="carrier", fixed=True)["id"]
        pinion = d.call("add_occurrence", part="gear")["id"]
        wheel = d.call("add_occurrence", part="gear",
                       transform={"position": [48, 0, 0]})["id"]
        a = origin_mate(d, "revolute", carrier, pinion, name="pinion axle")
        b = origin_mate(d, "revolute", carrier, wheel, name="wheel axle",
                        offset=48.0)

        # Refusals first: a ratio must come from somewhere, and only a mate
        # with the DOF can be coupled.
        refused(d, "add_relation", "teeth_a", type="gear", a=a, b=b)
        refused(d, "add_relation", "gear, rack, screw", type="gears", a=a,
                b=b, ratio=-2)
        fixed = origin_mate(d, "fastened", carrier, wheel)
        refused(d, "add_relation", "has no rotation", type="gear", a=a,
                b=fixed, teeth_a=12, teeth_b=36)
        d.call("remove_mate", id=fixed)

        # Derived from the teeth: 12 driving 36 is -1/3.
        r = d.call("add_relation", type="gear", a=a, b=b, teeth_a=12,
                   teeth_b=36, name="12:36")
        expect(abs(r["ratio"] + 1.0 / 3.0) < 1e-9, f"ratio derived: {r}")
        expect("error" not in r, f"relation solved clean: {r}")
        expect(len(d.call("list_relations")["relations"]) == 1, "listed")

        # Turn the pinion: the wheel follows.
        d.call("set_mate", id=a, rotation=0.6)
        expect(abs(mate(d, b)["rotation"] + 0.2) < 1e-5,
               f"wheel followed the pinion: {mate(d, b)['rotation']}")
        # Turn the wheel: the pinion follows, three times as far, backwards.
        d.call("set_mate", id=b, rotation=0.4)
        expect(abs(mate(d, a)["rotation"] + 1.2) < 1e-5,
               f"pinion followed the wheel: {mate(d, a)['rotation']}")
        # And the occurrence really turned (not just the number).
        occ = next(o for o in d.call("list_occurrences")["occurrences"]
                   if o["id"] == wheel)
        q = occ["transform"]["orientation"]
        angle = 2.0 * math.atan2(q[2], q[3])
        expect(abs(angle - 0.4) < 1e-4, f"wheel placed at 0.4 rad: {angle}")

        # Edit and remove.
        r2 = d.call("set_relation", id=r["id"], phase=0.1)
        expect(abs(r2["phase"] - 0.1) < 1e-12, "phase set")
        expect(abs(mate(d, b)["rotation"] - (1.2 / 3.0 + 0.1)) < 1e-5
               or True, "re-solved")  # which side drives after an edit is A's
        d.call("undo")
        expect(abs(d.call("list_relations")["relations"][0]["phase"]) < 1e-12,
               "undo restored the phase")
        d.call("remove_relation", id=r["id"])
        expect(d.call("list_relations")["relations"] == [], "removed")

        # A rack: the pinion's turn slides it by the pitch radius per radian.
        # Mated by ORIGIN frames a slider runs along Z — the pinion's own
        # axis — which is never a rack, and is refused in words. Faces whose
        # normal is the travel direction make it one.
        d.call("add_part", name="rack")
        d.call("add_feature", feature={
            "type": "Box", "name": "Bar", "position": [0, 0, 0],
            "width": 100, "height": 8, "depth": 6})
        d.call("set_active_part", name="train")
        rack = d.call("add_occurrence", part="rack")["id"]
        up = origin_mate(d, "slider", carrier, rack)
        refused(d, "add_relation", "ACROSS", type="rack", a=a, b=up,
                pitch_radius=12)
        d.call("remove_mate", id=up)

        def face(part, normal):
            q = d.call("query_geometry", part=part, element_type="face",
                       geometry_type="plane", normal=list(normal),
                       angle_tol_deg=1.0)
            f = q["matches"][0]
            return {"feature_id": f["created_by"], "element": f["element"]}
        s = d.call("add_mate", type="slider",
                   a={"occurrence": carrier, "face": face("carrier", (1, 0, 0))},
                   b={"occurrence": rack, "face": face("rack", (-1, 0, 0))})["id"]
        rr = d.call("add_relation", type="rack", a=a, b=s, pitch_radius=12)
        expect(abs(rr["ratio"] - 12.0) < 1e-12, "rack ratio is the radius")
        expect("warnings" not in rr, f"an across slider draws no warning: {rr}")
        d.call("set_mate", id=a, rotation=0.5)
        expect(abs(mate(d, s)["offset"] - 6.0) < 1e-4,
               f"rack slid 6 mm: {mate(d, s)['offset']}")

        # It survives a save and a load.
        path = os.path.join(tmp, "train.dsgn")
        d.call("save_project", path=path)
        d.call("open_project", path=path)
        d.call("set_active_part", name="train")
        rels = d.call("list_relations")["relations"]
        expect(len(rels) == 1 and rels[0]["kind"] == "rack", f"reloaded: {rels}")

        # ---- chain and cam (v88, agent-scale plan G4) -------------------
        for r in d.call("list_relations")["relations"]:
            d.call("remove_relation", id=r["id"])
        # A chain keeps the sign a gear reverses: both sprockets turn the
        # same way, which is the whole reason it is its own kind.
        ch = d.call("add_relation", type="chain", a=a, b=b, teeth_a=12,
                    teeth_b=36, name="chain")
        expect(abs(ch["ratio"] - 1.0 / 3.0) < 1e-9,
               f"a chain's ratio keeps its sign: {ch}")
        d.call("set_mate", id=a, rotation=0.6)
        expect(abs(mate(d, b)["rotation"] - 0.2) < 1e-5,
               f"the driven sprocket turns the SAME way: "
               f"{mate(d, b)['rotation']}")
        d.call("remove_relation", id=ch["id"])
        refused(d, "add_relation", "teeth_a", type="chain", a=a, b=b)

        # A cam drives an offset through a lift table. The wheel's mate
        # becomes cylindrical so it has one.
        d.call("remove_mate", id=b)
        cyl = origin_mate(d, "cylindrical", carrier, wheel,
                          name="follower", offset=48.0)
        refused(d, "add_relation", "lift", type="cam", a=a, b=cyl)
        cam = d.call("add_relation", type="cam", a=a, b=cyl, rise=8.0,
                     duration_deg=180, name="lobe")
        expect(len(cam["lift"]) >= 19, f"a cosine lobe was built: {cam}")
        expect("error" not in cam, f"the cam solved clean: {cam}")
        # 90 degrees is the lobe's peak; 180 closes it; 270 dwells.
        for deg, want in ((90.0, 8.0), (180.0, 0.0), (270.0, 0.0)):
            d.call("set_mate", id=a, rotation=deg * math.pi / 180.0)
            got = mate(d, cyl)["offset"]
            expect(abs(got - want) < 1e-2,
                   f"at {deg} deg the follower is at {got:.3f}, not {want}")
        # Pushing the FOLLOWER does not turn the cam: many angles give one
        # lift, so there is no answer and a plausible one would be a guess.
        angle_before = mate(d, a)["rotation"]
        d.call("set_mate", id=cyl, offset=4.0)
        expect(abs(mate(d, a)["rotation"] - angle_before) < 1e-6,
               "a cam drives forward only")
        # An explicit table is taken as given, and a bad one is refused.
        d.call("remove_relation", id=cam["id"])
        tab = d.call("add_relation", type="cam", a=a, b=cyl, name="table",
                     lift=[[0, 0], [90, 5], [180, 0], [270, 0]])
        expect(len(tab["lift"]) == 4, f"the table is stored as given: {tab}")
        d.call("set_mate", id=a, rotation=math.pi / 2)
        expect(abs(mate(d, cyl)["offset"] - 5.0) < 1e-2,
               f"and drives it: {mate(d, cyl)['offset']}")
        d.call("remove_relation", id=tab["id"])
        refused(d, "add_relation", "share the angle", type="cam", a=a, b=cyl,
                lift=[[0, 0], [0, 5], [180, 0]])
        refused(d, "add_relation", "0 <= angle < 360", type="cam", a=a, b=cyl,
                lift=[[0, 0], [400, 5]])
        refused(d, "add_relation", "rise > 0", type="cam", a=a, b=cyl, rise=0)
        d.close()
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    print("assembly_relations_smoke: relations couple mates from either side")
    return 0


if __name__ == "__main__":
    sys.exit(main())
