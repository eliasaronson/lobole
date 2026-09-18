"""Put the grill together — by MATING it, not by placing it.

One occurrence is fixed (the left-hand cart side) and every other part is
held by a mate to a named socket on the part that carries it: the firebox to
the cart, the lid to the firebox's hinge, a valve to its own place on the
manifold, a wheel to its end of the axle. Nothing is positioned by a
transform typed in here, so nothing can drift out of place when a dimension
changes — the sockets move with the parts they belong to.

Which joints TURN is the other thing an assembly says: the lid, the two
doors, the wheels and the three knobs are revolute, and the rest are
fastened.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import check  # noqa: E402

import grill_spec as S  # noqa: E402


def place(d, part, name=None, fixed=False):
    """An occurrence with no transform: a mate puts it where it belongs."""
    kw = {"part": part, "fixed": fixed}
    if name:
        kw["name"] = name
    return d.call("add_occurrence", **kw)["id"]


def mate(d, kind, name, a_occ, a_frame, b_occ, b_frame, flipped=False):
    """`flipped` is not a detail: the same two frames mate eight ways.

    False aligns the two frames' z axes — right when both point the same way
    along the joint (a part sitting ON a seat). True opposes them, which is
    what two parts that MEET face to face need: a valve whose back face
    looks at the manifold's front. The lid came out hanging inside the
    firebox on the default, and a valve came out inside the firebox on the
    wrong one of these.
    """
    r = d.call("add_mate", type=kind, name=name, flipped=flipped,
               a={"occurrence": a_occ, "frame": a_frame},
               b={"occurrence": b_occ, "frame": b_frame})
    check(r, "mate " + name)
    return r


def assemble(d, name="Grill"):
    d.call("add_assembly", name=name)
    d.call("set_active_part", part=name)

    # --- the cart, out from its left-hand side ---------------------------
    left = place(d, "Cart side", name="Cart side left", fixed=True)
    right = place(d, "Cart side", name="Cart side right")
    mate(d, "fastened", "Cart sides", right, "inner", left, "opposite")
    back = place(d, "Cart back", name="Cart back")
    mate(d, "fastened", "Cart back", back, "inner", left, "back")
    shelf = place(d, "Cart shelf", name="Cart shelf")
    mate(d, "fastened", "Cart shelf", shelf, "seat", left, "shelf")

    for side, panel in (("left", left), ("right", right)):
        door = place(d, "Cart door", name="Cart door " + side)
        mate(d, "revolute", "Door " + side, door, "hinge_" + side, panel,
             "door")
        shelf_side = place(d, "Side shelf", name="Side shelf " + side)
        mate(d, "fastened", "Side shelf " + side, shelf_side, "hinge", panel,
             "side_shelf_" + side)
        foot = place(d, "Foot", name="Foot " + side)
        mate(d, "fastened", "Foot " + side, foot, "top", panel,
             "foot_" + side)

    axle = place(d, "Axle", name="Axle")
    mate(d, "fastened", "Axle", axle, "centre", left, "axle")
    for side in ("left", "right"):
        wheel = place(d, "Wheel", name="Wheel " + side)
        mate(d, "revolute", "Wheel " + side, wheel, "hub", axle,
             side + "_hub")

    tank = place(d, "Propane tank", name="Propane tank")
    mate(d, "fastened", "Tank", tank, "floor", shelf, "tank")
    reg = place(d, "Regulator", name="Regulator")
    mate(d, "fastened", "Regulator", reg, "inlet", tank, "outlet")

    # --- the firebox, and what it carries --------------------------------
    firebox = place(d, "Firebox", name="Firebox")
    mate(d, "fastened", "Firebox on cart", firebox, "underside", left,
         "firebox")
    panel = place(d, "Control panel", name="Control panel")
    mate(d, "fastened", "Control panel", panel, "face", firebox, "panel")
    tray = place(d, "Grease tray", name="Grease tray")
    mate(d, "fastened", "Grease tray", tray, "slide", firebox, "tray")

    hood = place(d, "Hood", name="Hood")
    mate(d, "revolute", "Lid hinge", hood, "hinge", firebox, "hinge")
    for side in ("left", "right"):
        cap = place(d, "Hood end cap", name="Hood end cap " + side)
        mate(d, "fastened", "Hood cap " + side, cap, "weld", hood,
             "cap_" + side)
    handle = place(d, "Lid handle", name="Lid handle")
    mate(d, "fastened", "Lid handle", handle, "mount", hood, "handle")
    thermo = place(d, "Thermometer", name="Thermometer")
    mate(d, "fastened", "Thermometer", thermo, "seat", hood, "thermo")

    # --- the gas train ---------------------------------------------------
    manifold = place(d, "Manifold", name="Manifold")
    mate(d, "fastened", "Manifold", manifold, "centre", firebox, "manifold")
    for i in range(S.BURNERS):
        n = i + 1
        valve = place(d, "Valve", name=f"Valve {n}")
        mate(d, "fastened", f"Valve {n}", valve, "manifold", manifold,
             f"valve_{n}", flipped=True)
        knob = place(d, "Knob", name=f"Knob {n}")
        mate(d, "revolute", f"Knob {n}", knob, "shaft", valve, "stem_end")
        burner = place(d, "Burner", name=f"Burner {n}")
        mate(d, "fastened", f"Burner {n}", burner, "inlet", firebox,
             f"burner_{n}")
        tent = place(d, "Heat tent", name=f"Heat tent {n}")
        mate(d, "fastened", f"Heat tent {n}", tent, "peak", firebox,
             f"tent_{n}")
        grate = place(d, "Cooking grate", name=f"Grate {n}")
        mate(d, "fastened", f"Grate {n}", grate, "seat", firebox,
             f"grate_{n}")
    rack = place(d, "Warming rack", name="Warming rack")
    mate(d, "fastened", "Warming rack", rack, "hang", firebox, "rack")

    solved = d.call("solve_assembly")
    bad = [m for m in solved.get("mates", [])
           if m.get("status") not in ("satisfied", "redundant")]
    if bad:
        raise SystemExit(f"mates not solved: {bad}")
    # An exploded view, which for a cart grill is the one picture that shows
    # what goes where: the app derives it from the assembly's own structure.
    d.call("auto_explode")
    return d.call("list_occurrences")
