#!/usr/bin/env python3
"""Stage 2: the worm, its bearing block and its knob. Opens base.dsgn,
saves worms.dsgn.

    python3 generators/pantilt/build_worms.py --port N

One worm part serves both axes (the pairs are the same), and one block part
all four bearings: its bore centre is mid-height, so it hangs from the yoke's
underside for pan and stands upside down on a ledge for tilt, the bore the
same height off its mounting face either way and its screw heads in their
counterbores.

The worm is modelled along its part Z with its blank's X (where a tooth
SPACE stands at mid-span, docs/modeling/worm-gears.md#phase) along +X; the
knob end is +Z. The blocks sit as far out along the worm as the wheel's tip
needs to clear them, so the worm carries a spacer collar each side.
"""
import common
from common import Plane, Param
import spec


def declare(part, S):
    for name, expr, comment in [
        ("m4_clear", "4.5", "mm, ISO 273 normal clearance for M4"),
        ("m4_head_d", "7", "mm, ISO 4762 M4 head"),
        ("m4_cb_d", "m4_head_d + 2 * nozzle", "mm, its counterbore"),
        ("block_web", "2 * print_wall",
         "mm, what a block's screw clamps: the web left under a counterbore "
         "from the head's side, so the screw is short"),
        ("block_t", "worm_brg_B + print_wall",
         "mm, along the worm: the bearing and a shoulder"),
        ("block_h", "worm_brg_D + 2 * print_wall",
         "mm, the bearing and a wall either side"),
        ("block_flange", "m4_cb_d + 2 * print_wall",
         "mm, each side, for a counterbored screw"),
        ("block_w", "block_h + 2 * block_flange", "mm, across the worm"),
        ("worm_span",
         "max(worm_len / 2 + run_gap, "
         "sqrt((wheel_rim_d / 2 + run_gap)^2 - "
         "(centre_dist - block_w / 2 - run_gap)^2), "
         "sqrt((wheel_rim_d / 2 + run_gap)^2 - "
         "(centre_dist - block_h / 2 - run_gap)^2))",
         "mm, worm mid-span to a block's inboard face: past the worm, and "
         "where the wheel's tip clears the block whichever way it stands"),
        ("worm_collar_d", "worm_brg_d + 0.25 * (worm_brg_D - worm_brg_d)",
         "mm, bears on the inner ring only"),
        ("knob_h", "knob_d / 2", "mm"),
        ("knob_bore", "knob_h - print_wall", "mm, how far the shaft goes in"),
    ]:
        part.param(name, expr, comment=comment, scope="project")
    S.update(spec.values(part))


def worm(app, S):
    part = app.part("worm")
    model = common.printed(part)
    declare(part, S)
    L, s = S["worm_len"], S["worm_span"]
    # Centred on the origin: it stands on a plane half its length down.
    end = part.datum_plane("xy", offset="-worm_len / 2", name="Worm end")
    blank = part.cylinder(end, d="worm_tip_d", height="worm_len",
                          name="Blank")
    face = blank.faces(kind="cylinder")[0]
    r = part.worm(face, starts=int(S["worm_starts"]), hand="right",
                  backlash=S["worm_backlash"], name="Worm")
    print("worm report:", r.get("report", {}).get("locking"))
    journal = model("shaft", S["worm_brg_d"])
    out = s + S["block_t"] + S["run_gap"]
    knob_end = out + S["knob_bore"] - S["nozzle"]
    for sign, end in ((1, knob_end), (-1, out)):
        z0 = sign * L / 2
        sk = part.sketch(Plane.xy(z0, flip=sign < 0), name="Collar")
        sk.circle((0, 0), d=Param(S["worm_collar_d"], "worm_collar_d"))
        part.extrude(sk.add(), s - L / 2, mode="add", name="Collar")
        sk = part.sketch(Plane.xy(sign * s, flip=sign < 0), name="Journal")
        sk.circle((0, 0), d=journal)
        part.extrude(sk.add(), end - s, mode="add", name="Journal")
    part.material("petg")
    common.frame(part, "axis", (0, 0, 0), (0, 0, 1), (1, 0, 0))
    common.frame(part, "knob", (0, 0, out), (0, 0, 1), (1, 0, 0))
    part.check()
    return part, r


def block(app, S):
    """The bearing block, bore along part X from its inboard face at x = 0:
    a counterbore the bearing presses into from inside, its shoulder
    outboard, so each block takes the worm's thrust one way."""
    part = app.part("worm_block")
    model = common.printed(part)
    t, h, w = S["block_t"], S["block_h"], S["block_w"]
    sk = part.sketch(Plane.yz(0), name="Block")
    sk.rect((-w / 2, -h / 2), (w / 2, h / 2))
    part.extrude(sk.add(), Param(t, "block_t"), name="Block")
    sk = part.sketch(Plane.yz(0), name="Bearing counterbore")
    sk.circle((0, 0), d=model("hole", S["worm_brg_D"]))
    part.cut(sk.add(), Param(S["worm_brg_B"], "worm_brg_B"),
             name="Bearing counterbore")
    relief = (S["worm_brg_d"] + S["worm_brg_D"]) / 2
    sk = part.sketch(Plane.yz(0), name="Relief")
    sk.circle((0, 0), d=model("hole", relief))
    part.cut(sk.add(), 1.0, end_mode="through_all", name="Relief")
    # Two screws down through the flanges, top to bottom.
    # Two screws up from the underside, counterbored to leave a web at the
    # top: a block hangs from the yoke the right way up and stands on the
    # ledge upside down, so the heads are in the counterbores either way.
    y = h / 2 + S["block_flange"] / 2
    sk = part.sketch(Plane.xy(-h / 2, flip=True), name="Screws")
    for sy in (1, -1):
        sk.point((t / 2, sy * y))
    part.hole(sk.add(), d=1.0, through=True, kind="counterbore",
              fit={"thread": "M4", "kind": "normal"},
              cb_diameter=model("hole", S["m4_cb_d"]),
              cb_depth=h - S["block_web"], name="Screws")
    part.material("petg")
    common.frame(part, "bore", (0, 0, 0), (1, 0, 0), (0, 0, 1))
    part.check()
    return part


def knob(app, S):
    part = app.part("knob")
    model = common.printed(part)
    body = part.cylinder("Top", d="knob_d", height="knob_h", name="Knob")
    sk = part.sketch(Plane.xy(0, flip=True), name="Bore")
    sk.circle((0, 0), d=model("hole", S["worm_brg_d"]))
    part.cut(sk.add(), Param(S["knob_bore"], "knob_bore"), reversed=True,
             name="Bore")
    # A grip: flutes round the rim, one per 4 mm of circumference.
    n = max(6, int(3.14159 * S["knob_d"] / 4))
    sk = part.sketch(Plane.xy(S["knob_h"]), name="Flute")
    sk.circle((S["knob_d"] / 2, 0), d=S["knob_d"] / n * 0.8)
    flute = part.cut(sk.add(), 1.0, end_mode="through_all", reversed=True,
                     name="Flute")
    part.pattern(flute, n, circular="z", name="Flutes")
    part.material("petg")
    common.frame(part, "bore", (0, 0, 0), (0, 0, 1), (1, 0, 0))
    part.check()
    return part


def main():
    a = common.args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(common.OUT / "base.dsgn"))
    S = {}
    worm(app, S)
    clock("worm")
    block(app, S)
    clock("worm block")
    knob(app, S)
    clock("knob")
    common.check_regen(app, "worms")
    common.save(app, "worms.dsgn")


if __name__ == "__main__":
    main()
