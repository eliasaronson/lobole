#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""fit_fastener and check_fasteners agree about where a joint ENDS
(docs/parts/README.md#deriving-a-fastener-from-a-hole).

They did not. `fit_fastener` stopped at the floor of a blind hole, which is
what a grip is; `check_fasteners` re-derived from the probed stack alone and
counted the material BELOW the hole as more joint. On the rig below — an 80 mm
base with a 25 mm blind hole under a 20 mm plate — one said a 45 mm grip and
the other said an 80 mm stack, with nothing happening in between:

    fit_fastener    -> M10x45, blind=True, "grip 45: plate 20, base 60"
    check_fasteners -> "is 35 mm too short: its stack is now 80 mm"

Two defects in one. The grip ignored the on-axis ray, and the check applied
the THROUGH rule ("it must span the stack") to a blind joint, which is derived
by snapping DOWN to a stocked length and so is shorter than its hole is deep by
construction. A blind screw is judged on bottoming out and thread engagement
instead.

Both now take the answer from `parts::JointEnd`, so they agree by construction.
This test holds the agreement on both kinds of joint and checks that the three
real defects are still reported.

Usage: fastener_stack_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
from designer_sdk import Plane, connect  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def plate(app, name, thickness, hole_from=-5.0, hole_depth=None):
    """A square plate with a clearance hole through it, and frames on both
    faces."""
    p = app.part(name)
    p.box("Top", width=80, depth=80, height=thickness, at=(-40, -40),
          centered=False, name="P")
    s = p.sketch(Plane.xy(hole_from), name="Hole")
    s.circle((0, 0), d=11.0)
    p.extrude(s.add(), hole_depth or (thickness + 10.0), mode="cut",
              name="Hole")
    p.frame("top", origin=(0, 0, thickness), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("bottom", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    return p


def rim_of(part, above):
    """The clearance hole's rim on the part's top face."""
    rims = [e for e in part.body().edges(circular=True, radius=5.5)
            if e.center.z > above]
    expect(len(rims) == 1, f"one rim on {part.name}'s top face: {len(rims)}")
    return rims[0]


def findings(app):
    return [f["why"] for f in app.check_fasteners().get("findings", [])]


def blind_rig(app):
    """A 20 mm plate over an 80 mm base with a 25 mm blind hole. The joint
    ends 45 mm down: 20 of plate, then the hole's floor."""
    app.new()
    b = app.part("base")
    b.box("Top", width=80, depth=80, height=60, at=(-40, -40),
          centered=False, name="Base")
    sk = b.sketch(Plane.xy(60, flip=True), name="Blind")
    sk.circle((0, 0), d=11.0)
    b.extrude(sk.add(), 25.0, mode="cut", name="Blind")
    b.frame("top", origin=(0, 0, 60), z=(0, 0, 1), x=(1, 0, 0))
    p = plate(app, "plate", 20.0)
    a = app.assembly("rig")
    bo = a.place(b, fixed=True)
    po = a.place(p, at=(0, 0, 60))
    a.fasten((bo, "top"), (po, "bottom"), flipped=False)
    a.solve()
    return a, po, p


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_fastener_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        a = connect(app.port)

        # ---- A BLIND joint: the two agree, and neither complains ---------
        rig, po, p = blind_rig(a)
        fit = rig.bolt(rim_of(p, 19.0), po, standard="iso4762", size="M10")
        expect(fit["blind"], f"the hole is blind: {fit}")
        expect(abs(fit["grip_mm"] - 45.0) < 1e-3,
               f"the joint ends at the hole's floor, 45 mm down: {fit}")
        # The decomposition adds up to the grip beside it. It used to say
        # "base 60" next to a grip of 45.
        expect("base 25 (of 60)" in fit["summary"],
               f"the summary counts only what the screw reaches: "
               f"{fit['summary']}")
        expect(findings(a) == [],
               f"the screw it just derived is not a defect: {findings(a)}")

        # ---- The same rig, a screw too LONG: bottoms out -----------------
        for o in a.list_occurrences()["occurrences"]:
            if "ISO" in o["name"]:
                a.remove_occurrence(id=o["id"])
        # Rim at world z = 80, and the axis into the hole is -Z: half a turn
        # about X.
        a.add_fastener(standard="iso4762", size="M10", length=60.0,
                       position=[0, 0, 80], orientation=[1, 0, 0, 0])
        why = findings(a)
        expect(len(why) == 1 and "bottoms out" in why[0],
               f"a 60 mm screw in a 45 mm joint bottoms out: {why}")

        # ---- ...and one too SHORT to hold: not enough thread -------------
        for o in a.list_occurrences()["occurrences"]:
            if "ISO" in o["name"]:
                a.remove_occurrence(id=o["id"])
        a.add_fastener(standard="iso4762", size="M10", length=25.0,
                       position=[0, 0, 80], orientation=[1, 0, 0, 0])
        why = findings(a)
        expect(len(why) == 1 and "of thread in base" in why[0],
               f"25 mm leaves only 5 mm in the base, and an M10 needs 10: "
               f"{why}")
        # And NOT the through rule, which is the bug this file exists for: a
        # blind screw is shorter than its hole is deep by construction.
        expect("too short" not in why[0],
               f"a blind screw is not judged on reaching a far side: {why}")

        # ---- A blind joint whose AXIS carries on into something else -----
        # Found building the air-cooled engine: its head bolts stop in a blind
        # hole in the barrel, and their axis runs on 158 mm further into a
        # crank web they do not touch. The blind-hole check read the raw
        # stack's LAST segment, so it refused ("its hole does not go all the
        # way through") even when `far_end` had said where the joint stops.
        # The stack stays untruncated on purpose — the washers and nut live in
        # it — but the question "is the blind hole at the END of the joint" is
        # exactly what a far end answers.
        rig, po, p = blind_rig(a)
        # Something further down the axis that the bolt has nothing to do
        # with, standing off on its own.
        far = plate(a, "bystander", 20.0, hole_from=-5.0, hole_depth=1.0)
        rig.place(far, at=(0, 0, -80))
        rig.solve()
        rim = rim_of(p, 19.0)
        why = None
        try:
            rig.bolt(rim, po, standard="iso4762", size="M10")
        except RuntimeError as e:
            why = str(e)
        expect(why is not None and "far_end" in why,
               f"without a far end it refuses AND names the way through: "
               f"{why}")
        floors = a.part("base", create=False).body().edges(
            circular=True, radius=5.5, where=lambda e: e.center.z < 59.0)
        expect(len(floors) == 1, f"one hole floor in the base: {len(floors)}")
        base_occ = next(o for o in rig.occurrences() if o.name == "base")
        fit = rig.bolt(rim, po, standard="iso4762", size="M10",
                       far_end={**floors[0].ref, "occurrence": base_occ.id})
        expect(fit["blind"], f"still a blind joint: {fit}")
        expect(abs(fit["grip_mm"] - 45.0) < 1e-3,
               f"and still ends at the hole's floor: {fit}")
        expect("carries on into bystander" in fit["summary"],
               f"the summary names what it stops short of: {fit['summary']}")

        # ---- A THROUGH joint: the two agree there too --------------------
        a.new()
        lower = plate(a, "lower", 20.0)
        upper = plate(a, "upper", 20.0)
        rig = a.assembly("rig")
        lo = rig.place(lower, fixed=True)
        up = rig.place(upper, at=(0, 0, 20))
        rig.fasten((lo, "top"), (up, "bottom"), flipped=False)
        rig.solve()
        fit = rig.bolt(rim_of(upper, 19.0), up, standard="iso4762",
                       size="M10", nut="iso4032")
        expect(not fit["blind"], f"clear all the way through: {fit}")
        expect(findings(a) == [],
               f"a derived through joint is not a defect: {findings(a)}")

        # ---- ...and a through screw that does NOT span is still caught ---
        for o in a.list_occurrences()["occurrences"]:
            if "ISO" in o["name"]:
                a.remove_occurrence(id=o["id"])
        a.add_fastener(standard="iso4762", size="M10", length=16.0,
                       position=[0, 0, 40], orientation=[1, 0, 0, 0])
        why = findings(a)
        expect(any("too short" in w for w in why),
               f"16 mm does not span two 20 mm plates: {why}")
        print("fastener_stack_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
