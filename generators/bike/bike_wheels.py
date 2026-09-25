"""The two wheels.

A wheel is built on ITS OWN AXIS — local Z along the axle, the wheel in the
local XY plane — and carries an "axis" frame at the origin. One revolute mate
to the frame's "rear axle" (or the fork's "front axle") puts it in the
bicycle and leaves it free to turn, which is the whole point of building it
this way rather than in world coordinates.

Each is one part with two bodies: the wheel (rim, spokes, hub — one solid)
and the tyre. Two bodies because they are two materials, and the mass of a
bicycle is mostly a question of which parts are steel.

The lacing is real three-cross: a spoke spans 60 degrees from its flange hole
to its rim hole (k crossings span k*720/N degrees), alternating leading and
trailing on each flange.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from designer_sdk import V  # noqa: E402
import bike_shop as shop  # noqa: E402
from bike_shop import axle_frame, cut, disc, revolved, tube, union  # noqa

# How far either side of its flange's mid-plane a spoke leaves the flange:
# inside the 4 mm flange, so its end stays buried in metal.
SPOKE_STAGGER = 1.0


def _ring(part, outer, inner, name):
    """A hollow solid of revolution: the outer profile less the inner one.

    A rim and a tyre are both thin shells, and a bicycle that models them
    solid weighs three times what it should — the rim alone would be 1.7 kg.
    """
    o = revolved(part, outer, (0, 0, 0), (0, 0, 1), f"{name} outer")
    i = revolved(part, inner, (0, 0, 0), (0, 0, 1), f"{name} bore")
    return cut(part, o, i, name)


def _rim(part, S):
    """A box-section rim: the bed the tyre sits on, walls, and a floor."""
    bed = float(S["RIM_BED_D"]) / 2.0
    out = bed + 2.0
    floor = bed - float(S["RIM_DEPTH"])
    half = float(S["RIM_WIDTH"]) / 2.0
    w = float(S["RIM_WALL"])
    bed_t = float(S["RIM_BED_T"])
    return _ring(part,
                 [(floor, -half), (out, -half), (out, half), (floor, half)],
                 [(floor + bed_t, -half + w), (out - w, -half + w),
                  (out - w, half - w), (floor + bed_t, half - w)], "Rim")


# ASSUMED, from the photographs: the rims are a dull, slightly rough silver
# — the same finish as the chainguard beside them — not the mirror of chrome
# plate. A roadster of this vintage could have either, and it is worth 1.4 kg:
# change this one word and the bicycle weighs what a steel-rimmed one weighs.
RIM_MATERIAL = "aluminum"


def _tyre(part, S):
    """The tyre as what it is: a casing round air, not a rubber doughnut.

    Solid, a 37-622 tyre comes out at 3.3 kg. The casing at 3.5 mm gives
    850 g, which is what one weighs with its tube in it.
    """
    bed = float(S["RIM_BED_D"]) / 2.0
    sect = float(S["TYRE_SECTION"])
    mid = bed + sect / 2.0
    casing = 3.5
    n = 24
    def circle(r):
        return [(mid + r * math.cos(2 * math.pi * k / n),
                 r * math.sin(2 * math.pi * k / n)) for k in range(n)]
    body = _ring(part, circle(sect / 2.0), circle(sect / 2.0 - casing), "Tyre")
    # The bead sits IN the rim, so the part of the casing that would be
    # inside the rim well is cut away — otherwise the tyre and the rim
    # overlap and every interference check reports the wheel clashing with
    # itself.
    well = revolved(part, [(0, -sect), (bed, -sect), (bed, sect), (0, sect)],
                    (0, 0, 0), (0, 0, 1), "Tyre well")
    return cut(part, body, well, "Tyre")


def _hub(part, S, shell_d, shell_w, flange_d, flange_gap, axle_half, name):
    ids = []
    half = float(shell_w) / 2.0
    r = float(shell_d) / 2.0
    # A profile that does not touch the axis revolves into a hollow shell
    # already — no bore to subtract. Subtracting one anyway is the app's
    # "removed no material" warning, and it was there for two builds.
    ids.append(revolved(part, [(r - 3.0, -half), (r, -half), (r, half),
                               (r - 3.0, half)], (0, 0, 0), (0, 0, 1),
                        f"{name} shell"))
    for side in (+1, -1):
        ids.append(disc(part, (0, 0, side * float(flange_gap) / 2.0),
                        (0, 0, 1), flange_d, 4.0, f"{name} flange {side:+d}"))
    ids.append(tube(part, (0, 0, -float(axle_half)), (0, 0, float(axle_half)),
                    float(S["AXLE_D"]), 0.0, f"{name} axle"))
    return ids


def _spokes(part, S, flange_r, flange_gap, name="Spoke"):
    """All thirty-six, laced three-cross.

    Built one at a time and not as a four-spoke set patterned nine times,
    which is what this tried first. The pattern is correct — thirty-six
    spokes come out of it — but its instances TOUCH where they cross at the
    flange, so the app returns them as two bodies, and a Boolean names a
    FEATURE: unioning "the pattern" into the rim took one of those bodies and
    left the other eighteen spokes lying beside the wheel, still a valid
    part. Thirty-six tubes cost 1.4 s more and are each addressable.
    """
    n = int(S["SPOKE_COUNT"])
    cross = int(S["SPOKE_CROSS"])
    span = math.radians(cross * 720.0 / n)     # 60 deg at 36h, three-cross
    rim_r = float(S["RIM_BED_D"]) / 2.0 - float(S["RIM_DEPTH"])
    ids = []
    for k in range(n):
        side = +1 if k % 2 == 0 else -1        # alternate flanges
        lead = +1 if (k // 2) % 2 == 0 else -1  # and leading/trailing on each
        rim_a = k * (2 * math.pi / n)
        hub_a = rim_a - lead * span
        # Leading and trailing spokes leave from opposite halves of the
        # flange (heads in, heads out), which is what a laced wheel does. Out
        # of one plane, two spokes of one flange CROSSED axis through axis,
        # 8 um apart: equal cylinders on intersecting axes, whose
        # intersection is singular, and the kernel's union kept a piece of
        # one spoke's skin inside the other. A millimetre each way leaves
        # them overlapping by 0.6 mm at the cross, which it solves cleanly.
        a = (flange_r * math.cos(hub_a), flange_r * math.sin(hub_a),
             side * (float(flange_gap) / 2.0 + lead * SPOKE_STAGGER))
        b = (rim_r * math.cos(rim_a), rim_r * math.sin(rim_a), 0.0)
        ids.append(tube(part, a, b, S["SPOKE_D"], 0.0, f"{name} {k}"))
    return ids


def _wheel(app, S, name, shell_d, shell_w, flange_d, flange_gap, axle_half,
           extra=None):
    """One part, THREE bodies, because a wheel is three materials and the
    mass of a bicycle is mostly which parts are steel.

    The rim is its own body rather than fused to the spokes: an alloy rim
    welded to steel spokes would have to be one material, and at 28 x 37 the
    difference between calling the rim steel and calling it alloy is 1.4 kg
    on a 16 kg bicycle. The spokes end ON the rim's floor, so there is
    nothing to fuse and nothing to gap.
    """
    part = app.part(name)
    rim = _rim(part, S)
    ids = _hub(part, S, shell_d, shell_w, flange_d, flange_gap, axle_half,
               name)
    ids += _spokes(part, S, float(flange_d) / 2.0 - 4.0, flange_gap)
    if extra:
        ids += extra(part)
    union(part, ids, f"{name} build")
    _tyre(part, S)
    for index, (slug, appearance) in enumerate(
            [(RIM_MATERIAL, "alloy"), ("steel", "chrome"),
             ("rubber", "tyre")]):
        part.app.set_material(body=index, material=slug)
        shop.look(part, appearance, body=index)
    axle_frame(part, "axis", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 3)
    del rim
    return part


def front_wheel(app, S):
    return _wheel(app, S, "Front wheel", S["FRONT_HUB_D"], S["FRONT_HUB_W"],
                  S["FLANGE_D"], S["FLANGE_SPACING"],
                  float(S["FRONT_OLD"]) / 2.0 + float(S["DROPOUT_T"]) + 1.0)


def rear_wheel(app, S):
    """The 3-speed hub's wheel. Its shell is fatter and shorter than a plain
    hub's, and it carries the cog on a driver outboard of the right-hand
    flange — which is what sets the chainline the whole drivetrain is built
    to.
    """
    def driver(part):
        z = -float(S["CHAINLINE"])
        cog_r = float(S["COG_R"])
        return [
            tube(part, (0, 0, -float(S["REAR_HUB_W"]) / 2.0),
                 (0, 0, z - float(S["COG_T_MM"]) / 2.0), 30.0, 0.0, "Driver"),
            disc(part, (0, 0, z), (0, 0, 1), 2 * (cog_r + 2.6),
                 S["COG_T_MM"], "Cog"),
        ]

    return _wheel(app, S, "Rear wheel", S["REAR_HUB_D"], S["REAR_HUB_W"],
                  S["REAR_FLANGE_D"], S["REAR_FLANGE_SPACING"],
                  float(S["REAR_OLD"]) / 2.0 + float(S["DROPOUT_T"]) + 1.0,
                  extra=driver)
