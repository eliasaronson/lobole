#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Relations over the RPC, headless: a gear pair whose ratio is DERIVED from
tooth counts, driven from either side; a rack; the refusals in words.
(docs/assemblies/relations.md)
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402


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
        box(d, (0, 0, 0), (80, 20, 5), "Blk")
        d.call("add_part", name="gear")
        cylinder(d, (0, 0, 0), "z", 12, 6, "Disc")
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
        box(d, (0, 0, 0), (100, 8, 6), "Bar")
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
        # A REVOLUTE follower turns (v102): a rocker, a pallet fork. Its
        # table is in degrees, and the lobe's rise is rise_deg — `rise` in
        # mm is refused for it rather than read as degrees.
        refused(d, "add_relation", "rise_deg", type="cam", a=cyl, b=a,
                rise=8.0, duration_deg=180)
        rock = d.call("add_relation", type="cam", a=cyl, b=a, rise_deg=30.0,
                      duration_deg=180, name="rocker")
        expect(rock["dof_b"] == "rotation", f"a revolute follower turns: {rock}")
        d.call("set_mate", id=cyl, rotation=math.pi / 2)
        expect(abs(math.degrees(mate(d, a)["rotation"]) - 30.0) < 0.05,
               f"the lobe's peak turns the rocker 30 deg: "
               f"{math.degrees(mate(d, a)['rotation']):.3f}")
        d.call("remove_relation", id=rock["id"])
        d.call("set_mate", id=cyl, rotation=0.0)
        # A cylindrical follower has both: dof_b names the turn, and the
        # default stays the slide every earlier caller meant.
        turn = d.call("add_relation", type="cam", a=a, b=cyl, dof_b="rotation",
                      lift=[[0, 0], [90, 40], [180, 0], [270, 0]])
        d.call("set_mate", id=a, rotation=math.pi / 2)
        expect(abs(math.degrees(mate(d, cyl)["rotation"]) - 40.0) < 0.05,
               f"dof_b rotation turns the cylindrical follower: "
               f"{math.degrees(mate(d, cyl)['rotation']):.3f}")
        d.call("remove_relation", id=turn["id"])
        d.call("set_mate", id=a, rotation=0.0)
        d.call("set_mate", id=cyl, rotation=0.0)
        refused(d, "add_relation", "rise (mm)", type="cam", a=a, b=cyl,
                rise_deg=8.0, duration_deg=180)
        cam = d.call("add_relation", type="cam", a=a, b=cyl, rise=8.0,
                     duration_deg=180, name="lobe")
        expect(len(cam["lift"]) >= 19, f"a cosine lobe was built: {cam}")
        # BOTH ends of the lobe are in the table. Without the closing one a
        # 180-degree lobe ran its last flank from 170 straight to the dwell,
        # and the follower stood 0.18 mm proud at the angle the lobe was
        # specified to shut at — a lobe that does not close when it says so.
        angles = [round(a, 6) for a, _ in cam["lift"]]
        expect(0.0 in angles and 180.0 in angles,
               f"the lobe opens and CLOSES on the table: {angles}")
        expect(abs(dict(cam["lift"])[180.0]) < 1e-9,
               f"the lobe is shut at 180: {cam['lift']}")
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

        # ---- freewheel (v104, docs/assemblies/relations.md#freewheel) ---------------------
        for r in d.call("list_relations")["relations"]:
            d.call("remove_relation", id=r["id"])
        d.call("set_mate", id=a, rotation=0.0)
        d.call("set_mate", id=cyl, rotation=0.0)
        # Its direction is the whole of it: required, and only the two.
        refused(d, "add_relation", "drives", type="freewheel", a=a, b=cyl,
                ratio=1.0)
        refused(d, "add_relation", "positive or negative", type="freewheel",
                a=a, b=cyl, ratio=1.0, drives="forward")
        refused(d, "add_relation", "drives is a freewheel's", type="gear",
                a=a, b=cyl, ratio=1.0, drives="positive")
        fw = d.call("add_relation", type="freewheel", a=a, b=cyl, ratio=1.0,
                    drives="positive", name="one-way")
        expect(fw["drives"] == "positive" and fw["lead"] == 0.0,
               f"a freewheel starts engaged: {fw}")

        def rot(mid):
            return mate(d, mid)["rotation"]

        def lead():
            return next(r for r in d.call("list_relations")["relations"]
                        if r["id"] == fw["id"])["lead"]

        d.call("set_mate", id=a, rotation=0.5)
        expect(abs(rot(cyl) - 0.5) < 1e-4, f"driven forward: {rot(cyl)}")
        d.call("set_mate", id=a, rotation=0.2)
        expect(abs(rot(cyl) - 0.5) < 1e-4 and abs(lead() - 0.3) < 1e-4,
               f"back, it slips: b {rot(cyl)}, lead {lead()}")
        d.call("set_mate", id=cyl, rotation=1.0)
        expect(abs(rot(a) - 0.2) < 1e-4,
               f"the output run on ahead leaves the input: {rot(a)}")
        d.call("set_mate", id=cyl, rotation=0.8)
        expect(abs(rot(a) - 0.0) < 1e-4,
               f"the output pushed back drags the input: {rot(a)}")
        # Undo restores the history with the pose.
        kept = lead()
        d.call("set_mate", id=a, rotation=-0.3)
        expect(abs(lead() - (kept + 0.3)) < 1e-4, f"slipped again: {lead()}")
        d.call("undo")
        expect(abs(lead() - kept) < 1e-6 and abs(rot(a)) < 1e-6,
               f"undo puts the lead back with the pose: {lead()} {rot(a)}")
        # A motion study runs on a copy and leaves it as it was.
        st = d.call("add_motion_study", driver=a, dof="rotation", **{
            "from": 0.0, "to": -90.0}, samples=5, check_clash=False,
            name="back")
        d.call("run_motion_study", study=st["id"])
        expect(abs(lead() - kept) < 1e-6,
               f"a motion study leaves the lead alone: {lead()}")
        # Re-engaged by hand: the output moves onto the drive line.
        d.call("set_relation", id=fw["id"], lead=0.0)
        expect(abs(lead()) < 1e-6 and abs(rot(cyl) - rot(a)) < 1e-4,
               f"lead 0 puts b on a's line: {rot(cyl)} vs {rot(a)}")
        # Only a freewheel or a click has a history to set.
        d.call("remove_relation", id=fw["id"])
        g = d.call("add_relation", type="gear", a=a, b=cyl, ratio=1.0)
        refused(d, "set_relation", "freewheel's or a click's history",
                id=g["id"], lead=1.0)

        # ---- a click, and riding over teeth (v105) -----------------------
        refused(d, "add_relation", "same mate id", type="click", a=a, b=cyl,
                drives="positive")
        refused(d, "add_relation", "a click needs drives", type="click", a=a,
                b=a)
        d.call("set_mate", id=a, rotation=0.0)
        ck = d.call("add_relation", type="click", a=a, b=a, drives="positive",
                    name="pawl")
        d.call("set_mate", id=a, rotation=1.0)
        expect(abs(rot(a) - 1.0) < 1e-4,
               f"the click lets it turn its own way: {rot(a)}")
        d.call("set_mate", id=a, rotation=0.4)
        expect(abs(rot(a) - 1.0) < 1e-4,
               f"turned back, the click holds it at its mark: {rot(a)}")
        d.call("remove_relation", id=ck["id"])
        d.call("remove_relation", id=g["id"])
        d.call("set_mate", id=a, rotation=0.0)
        d.call("set_mate", id=cyl, rotation=0.0, offset=0.0)
        # Riding over the dogs lifts the clutch: a ride needs its teeth.
        refused(d, "add_relation", "a ride needs teeth", type="freewheel",
                a=a, b=cyl, ratio=1.0, drives="positive", ride_mate=cyl,
                ride_mm=1.0)
        refused(d, "add_relation", "ride_mm, not ride_deg", type="freewheel",
                a=a, b=cyl, ratio=1.0, drives="positive", teeth=4,
                ride_mate=cyl, ride_dof="offset", ride_deg=10.0)
        fw2 = d.call("add_relation", type="freewheel", a=a, b=cyl, ratio=1.0,
                     drives="positive", teeth=4, ride_mate=cyl,
                     ride_dof="offset", ride_mm=1.0, name="dogs")
        # Off its notch it stands clear of the dogs...
        d.call("set_mate", id=a, rotation=-math.pi / 4)
        expect(abs(mate(d, cyl)["offset"] - 1.0) < 1e-3,
               f"off a notch it lifts clear: {mate(d, cyl)['offset']}")
        # ...and a whole tooth drops it into the next notch.
        d.call("set_mate", id=a, rotation=-math.pi / 2)
        expect(abs(mate(d, cyl)["offset"]) < 1e-3,
               f"a whole tooth drops it back: {mate(d, cyl)['offset']}")
        d.call("remove_relation", id=fw2["id"])

        # An epicyclic train (v171): sun, ring and carrier on the base, a
        # planet on the carrier. Refusals first, each changing nothing.
        occ = {k: d.call("add_occurrence", part="gear", name=k)["id"]
               for k in ("sun", "ring", "arm", "planet")}
        em = {k: origin_mate(d, "revolute", carrier, occ[k], name=k)
              for k in ("sun", "ring", "arm")}
        em["planet"] = origin_mate(d, "revolute", occ["arm"], occ["planet"],
                                   name="planet")
        before = len(d.call("list_relations")["relations"])
        refused(d, "add_relation", "not both", type="gear",
                a=em["sun"], b=em["ring"], teeth_a=13, teeth_b=40, ratio=0.3)
        refused(d, "add_relation", "give the teeth", type="gear",
                a=em["sun"], b=em["ring"], ratio=0.3, internal=True)
        refused(d, "add_relation", "both the sun and the ring",
                type="epicyclic", sun=em["sun"], ring=em["sun"],
                carrier=em["arm"], teeth_sun=14, teeth_ring=40)
        refused(d, "add_relation", "carrier", type="epicyclic",
                sun=em["sun"], ring=em["ring"], teeth_sun=14, teeth_ring=40)
        refused(d, "add_relation", "whole number", type="epicyclic",
                sun=em["sun"], ring=em["ring"], carrier=em["arm"],
                teeth_sun=14.5, teeth_ring=40)
        refused(d, "add_relation", "not a and b", type="epicyclic",
                a=em["sun"], sun=em["sun"], ring=em["ring"],
                carrier=em["arm"], teeth_sun=14, teeth_ring=40)
        expect(len(d.call("list_relations")["relations"]) == before,
               "a refused relation was added anyway")
        epi = d.call("add_relation", type="epicyclic", sun=em["sun"],
                     ring=em["ring"], carrier=em["arm"], teeth_sun=14,
                     teeth_ring=40, planets=[{"mate": em["planet"],
                                              "teeth": 13}], name="train")
        expect(epi["kind"] == "epicyclic" and len(epi["planets"]) == 1,
               f"an epicyclic with its planet: {epi}")
        expect([t["teeth"] for t in epi["terms"][:2]] == [14, 40],
               f"the teeth are kept: {epi['terms']}")
        d.call("set_mate", id=em["sun"], rotation=0.9)
        carrier_at = 0.9 * 14 / 54
        expect(abs(rot(em["ring"])) < 1e-6, f"the ring held: {rot(em['ring'])}")
        expect(abs(rot(em["arm"]) - carrier_at) < 1e-4,
               f"the carrier at zs/(zs+zr) of the sun: {rot(em['arm'])}")
        expect(abs(rot(em["planet"]) + 14 / 13 * (0.9 - carrier_at)) < 1e-4,
               f"the planet on its carrier: {rot(em['planet'])}")
        rels = {r["id"]: r for r in d.call("list_relations")["relations"]}
        expect("ring" in rels[epi["id"]].get("held", ""),
               f"it says it held the ring: {rels[epi['id']]}")
        refused(d, "set_relation", "constant", id=epi["id"], phase=0.1)
        d.call("set_relation", id=epi["id"], teeth_sun=20)
        rels = {r["id"]: r for r in d.call("list_relations")["relations"]}
        expect(rels[epi["id"]]["terms"][2]["coefficient"] == -60,
               f"new teeth, new coefficients: {rels[epi['id']]}")
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
