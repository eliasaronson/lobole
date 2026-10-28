#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The contact relation over the RPC, headless
(docs/assemblies/relations.md#contact): an eccentric on a horizontal axle
under a foot that slides up and down. `toward` comes from gravity, the foot
stands on the eccentric at every angle, list_relations reports the table,
set_relation turns the sense round, and the refusals say what a contact
relation takes.
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402

R, E = 15.0, 4.0


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


def offset_of(d, mid):
    return next(m for m in d.call("list_mates")["mates"]
                if m["id"] == mid)["offset"]


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="lobole_contact_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        d.call("add_part", name="frame")
        box(d, (-60, -60, -60), (5, 5, 5), "Blk")
        d.call("add_part", name="cam")
        cylinder(d, (E, 0, 0), "z", R, 6, "Eccentric")
        d.call("add_part", name="foot")
        box(d, (-15, -15, 0), (30, 30, 10), "Foot")
        d.call("add_assembly", name="rig")
        # The axle horizontal (its Z along world -Y), the slide vertical.
        frame = d.call("add_occurrence", part="frame", fixed=True,
                       transform={"rotate": {"axis": [1, 0, 0],
                                             "angle_deg": 90}})["id"]
        guide = d.call("add_occurrence", part="frame", fixed=True)["id"]
        cam = d.call("add_occurrence", part="cam")["id"]
        foot = d.call("add_occurrence", part="foot")["id"]
        axle = origin_mate(d, "revolute", frame, cam, name="axle")
        slide = origin_mate(d, "slider", guide, foot, name="slide")

        refused(d, "add_relation", "not a contact relation's",
                type="contact", a=axle, b=slide, ratio=2)
        refused(d, "add_relation", "positive or negative", type="contact",
                a=axle, b=slide, toward="down")
        refused(d, "add_relation", "contact relation's (type contact)",
                type="gear", a=axle, b=slide, ratio=1, toward="negative")

        rel = d.call("add_relation", type="contact", a=axle, b=slide)
        expect("error" not in rel, f"the contact relation solved: {rel}")
        expect(rel["toward"] == "negative",
               f"gravity holds the foot down onto the cam: {rel}")
        # The eccentric's centre turns in the world XZ plane: its top is
        # R + E sin(theta) above the axle, and the foot stands on it.
        for deg in (0, 37, 90, 200, 300):
            d.call("set_mate", id=axle, rotation_deg=deg)
            want = R + E * math.sin(math.radians(deg))
            got = offset_of(d, slide)
            expect(abs(got - want) < 1e-3,
                   f"at {deg} deg the foot stands at {got}, not {want}")
        listed = next(r for r in d.call("list_relations")["relations"]
                      if r["id"] == rel["id"])
        table = listed["contact"]
        expect(table["points"] >= 72 and table["drops"] == [],
               f"a smooth table over a turn: {table}")
        expect(table["bound_mm"] <= table["tolerance_mm"],
               f"the table holds its tolerance: {table}")

        # Held the other way, the foot comes up from below and its TOP
        # touches the eccentric's bottom.
        d.call("set_relation", id=rel["id"], toward="positive")
        want = -R + E * math.sin(math.radians(300)) - 10.0
        got = offset_of(d, slide)
        expect(abs(got - want) < 1e-3, f"from below: {got}, not {want}")
        refused(d, "set_relation", "not a contact relation's", id=rel["id"],
                phase=1.0)

        # A follower that never reaches the cam is refused, not added
        # broken.
        far = d.call("add_occurrence", part="frame", fixed=True,
                     transform={"position": [80, 0, 0]})["id"]
        foot2 = d.call("add_occurrence", part="foot")["id"]
        slide2 = origin_mate(d, "slider", far, foot2, name="far slide")
        refused(d, "add_relation", "never touches", type="contact", a=axle,
                b=slide2)
        d.close()
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    print("contact_relation_smoke: a follower stands on its cam's shape")
    return 0


if __name__ == "__main__":
    sys.exit(main())
