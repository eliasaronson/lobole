"""Put the grill together: occurrences where they belong, and joints where it
actually moves — the lid, the wheels, the control knobs.

Every part is drawn in the orientation it is used in, so each occurrence is a
translation. What is left for the assembly to say is the part that matters:
which of these joints turn, and what has to clear what.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import check  # noqa: E402

import grill_spec as S  # noqa: E402

FACET_RUN = S.HOOD_FACET * math.cos(math.radians(S.HOOD_FACET_DEG))
FACET_DROP = S.HOOD_FACET * math.sin(math.radians(S.HOOD_FACET_DEG))

# The firebox pan's own height, base to rim top, and where the hood must sit
# for its facets to land on that rim.
BOX_TOP = S.FIREBOX_Z + 1.5 + S.BOX_H + 1.5
HOOD_Z = BOX_TOP + S.LID_GAP + FACET_DROP  # the lid rests ON the rim
# The burner rides at the height its port was cut at — one number, so the
# tube cannot clip the wall it passes through.
BURNER_Z = S.FIREBOX_Z + 1.5 + S.BURNER_AXIS_Z


def quat_y(deg):
    a = math.radians(deg) / 2.0
    return [0.0, math.sin(a), 0.0, math.cos(a)]


def burner_x(i):
    return (i - (S.BURNERS - 1) / 2.0) * S.BURNER_PITCH


def place(d, part, at, name=None, fixed=True, orientation=None):
    kw = {"part": part, "fixed": fixed,
          "transform": {"position": [float(v) for v in at],
                        "orientation": orientation or [0, 0, 0, 1]}}
    if name:
        kw["name"] = name
    return d.call("add_occurrence", **kw)["id"]


def revolute(d, name, a_occ, a_frame, b_occ, b_frame):
    """A joint that turns: the lid, a wheel, a knob."""
    # `flipped` is not a detail: the same two frames mate eight ways, and the
    # lid came out hanging INSIDE the firebox on the default.
    r = d.call("add_mate", type="revolute", name=name, flipped=False,
               a={"occurrence": a_occ, "frame": a_frame},
               b={"occurrence": b_occ, "frame": b_frame})
    check(r, "mate " + name)
    return r


def assemble(d, name="Grill"):
    d.call("add_assembly", name=name)
    d.call("set_active_part", part=name)
    t = 1.0  # cart gauge, for tucking panels inside their neighbours

    # --- the cart ---------------------------------------------------------
    place(d, "Cart side", (-S.CART_X / 2, 0, 0), name="Cart side left")
    place(d, "Cart side", (S.CART_X / 2 - t, 0, 0), name="Cart side right")
    place(d, "Cart back", (0, S.CART_Y / 2, 0), name="Cart back")
    # Low enough that a bottle standing on it still clears the firebox: the
    # tank is 570 tall and the cart is 650.
    place(d, "Cart shelf", (0, 0, 30.0), name="Cart shelf")
    for side, x in (("left", -1), ("right", +1)):
        place(d, "Cart door", (x * (S.DOOR_X / 2 + 12.0), -S.CART_Y / 2, 70.0),
              name="Cart door " + side)
    for side, x in (("left", -1), ("right", +1)):
        place(d, "Side shelf",
              (x * (S.CART_X / 2 + S.SIDE_SHELF_X / 2 + 35.0), -40.0,
               S.FIREBOX_Z),
              name="Side shelf " + side)

    # --- the firebox ------------------------------------------------------
    firebox = place(d, "Firebox", (0, 0, S.FIREBOX_Z), name="Firebox")
    # Centred so its bores — S.VALVE_Z above the panel's middle — land on
    # the valve stems: one number, shared, rather than two that must agree.
    place(d, "Control panel",
          (0, -(S.BOX_Y / 2 + 60.0), S.FIREBOX_Z + 30.0 - S.VALVE_Z),
          name="Control panel")
    # It slides in under the firebox floor: above the bottle's collar (which
    # tops out at 601), behind the control panel's face, and in front of the
    # bottle itself. There is 49 mm of headroom there and the tray is 40.
    place(d, "Grease tray", (0, -120.0, S.FIREBOX_Z - 30.0), name="Grease tray")

    # The lid, and the joint it opens on.
    hood = place(d, "Hood", (0, 0, HOOD_Z), name="Hood", fixed=False)
    for side, x in (("left", -1), ("right", +1)):
        place(d, "Hood end cap",
              (x * (S.BOX_X / 2 - 1.2), 0, HOOD_Z), name="Hood end cap " + side)
        # (the cap's own plane is the hood's: both sit at HOOD_Z)
    # Its legs land on the front facet: the facet falls one millimetre for
    # every millimetre out, so the bar stands the legs' own height above
    # where they touch.
    handle_out = S.HOOD_TOP_Y / 2 + 60.0
    place(d, "Lid handle",
          (0, -handle_out, HOOD_Z - 60.0 + S.HANDLE_RISE + 14.0),
          name="Lid handle")
    place(d, "Thermometer", (0, 0, HOOD_Z + 1.2 + 0.5), name="Thermometer")
    revolute(d, "Lid hinge", hood, "hinge", firebox, "hinge")

    # --- burners, gas and grates -----------------------------------------
    # Front to back: the panel's face, the knobs in front of it, the valves
    # behind it, and the manifold behind them. Each stands clear of the next
    # — a valve that shares steel with its manifold is not a joint, it is two
    # solids in one place.
    place(d, "Manifold", (0, -(S.BOX_Y / 2 + 18.0), S.FIREBOX_Z + 30.0),
          name="Manifold")
    for i in range(S.BURNERS):
        place(d, "Burner", (burner_x(i), 10.0, BURNER_Z),
              name=f"Burner {i + 1}")
        valve = place(d, "Valve",
                      (burner_x(i), -(S.BOX_Y / 2 + 44.0), S.FIREBOX_Z + 30.0),
                      name=f"Valve {i + 1}")
        knob = place(d, "Knob",
                     (burner_x(i), -(S.BOX_Y / 2 + 62.0), S.FIREBOX_Z + 30.0),
                     name=f"Knob {i + 1}", fixed=False)
        revolute(d, f"Knob {i + 1} shaft", knob, "shaft", valve, "stem_end")
        # A heat tent over each burner, its ridge on the burner's centre.
        place(d, "Heat tent",
              (burner_x(i), 0, BURNER_Z + 30.0),
              name=f"Heat tent {i + 1}",
              # Tilted so the ridge stands up and the legs fall away evenly:
              # the fold turns the second leg (180 - TENT_DEG) from the
              # first, so half of that puts the V's bisector vertical.
              orientation=quat_y((180.0 - S.TENT_DEG) / 2.0))
        place(d, "Cooking grate", (burner_x(i), 0, BURNER_Z + 100.0),
              name=f"Grate {i + 1}")
    place(d, "Warming rack", (0, 60.0, BURNER_Z + 120.0), name="Warming rack")

    # --- wheels, feet, tank ----------------------------------------------
    axle = place(d, "Axle", (0, S.CART_Y / 2 - 70.0, 95.0), name="Axle")
    for side, x in (("left", -1), ("right", +1)):
        wheel = place(d, "Wheel",
                      (x * (S.CART_X / 2 + S.WHEEL_W / 2 + 28.0),
                       S.CART_Y / 2 - 70.0, 95.0),
                      name="Wheel " + side, fixed=False)
        revolute(d, "Wheel " + side, wheel, "hub", axle, side + "_hub")
    for side, x in (("left", -1), ("right", +1)):
        place(d, "Foot", (x * (S.CART_X / 2 - 70.0), -S.CART_Y / 2 + 70.0, 0),
              name="Foot " + side)
    # Forward of the axle, which runs across the cart behind it.
    tank_z = 30.0 + 1.0
    tank_y = 0.0
    place(d, "Propane tank", (0, tank_y, tank_z), name="Propane tank")
    # On top of the bottle's valve, not inside it: the valve stands
    # TANK_COLLAR_H - 25 above the shoulder, and the bell hangs below its
    # own origin.
    place(d, "Regulator",
          (0, tank_y, tank_z + S.TANK_H + 10.0 + (S.TANK_COLLAR_H - 25.0) + 30.0),
          name="Regulator")
    # An exploded view, which for a cart grill is the one picture that shows
    # what goes where: the app derives it from the assembly's own structure.
    d.call("auto_explode")
    return d.call("list_occurrences")
