"""Everything on the grill that is not folded sheet: burners, gas, grates,
wheels, the tank, and the bits a hand touches.

These are plain solids — the sheet-metal features have no business here — but
they are what makes the assembly a grill rather than a stack of panels, and
they are what the clash sweep has to clear.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import check, frame, part, sketch  # noqa: E402
from _lib import circle, line, plane_at_z  # noqa: E402

import grill_spec as S  # noqa: E402


def material(d, name):
    """Every body of this part is one material — the part IS the part.

    EVERY body: a patterned grate is one body per bar, and setting only the
    first left the rest with no material at all — so the grate weighed
    nothing, and the whole assembly's mass came back null because of it.
    """
    for i in range(len(d.call("get_state")["bodies"])):
        d.call("set_material", material=name, body=i)


def cylinder(d, name, centre, direction, radius, height):
    r = d.call("add_feature", feature={
        "type": "Cylinder", "name": name, "center": list(centre),
        "direction": list(direction), "radius": radius, "height": height})
    check(r, name)
    return r["id"]


def box(d, name, at, w, h, dp):
    r = d.call("add_feature", feature={
        "type": "Box", "name": name, "position": list(at),
        "width": w, "height": h, "depth": dp})
    check(r, name)
    return r["id"]


def boolean(d, name, target, tool, op="union"):
    """Union/subtract, returning the id that OWNS the result.

    A boolean consumes its operands, so the body that comes out answers to a
    new created_by: chaining a second boolean onto the id that went in fails
    with "an operand body was not found".
    """
    r = d.call("add_feature", feature={
        "type": "Boolean", "name": name, "target_id": target,
        "tool_id": tool, "op": op})
    check(r, name)
    # The call's own digest says which body came out — asking the document
    # for "the first body" picks a neighbour as soon as a part holds two.
    for body in r.get("digest", {}).get("bodies", []):
        if body["name"] == name:
            return body["created_by"]
    return r["id"]


def burner(d):
    """A burner tube: the cast tube, with its ports drilled down both sides."""
    part(d, "Burner")
    tube = cylinder(d, "Tube", (0, -S.BURNER_LEN / 2, 0), (0, 1, 0),
                    S.BURNER_D / 2, S.BURNER_LEN)
    # The venturi that slides over the valve's orifice.
    inlet = cylinder(d, "Venturi", (0, -S.BURNER_LEN / 2 - 40.0, 0), (0, 1, 0),
                     S.BURNER_D / 2 + 3.0, 40.0)
    tube = boolean(d, "Tube and venturi", tube, inlet)
    frame(d, "inlet", (0.0, -S.BURNER_LEN / 2 - 40.0, 0.0),
          z_axis=(0, -1, 0), x_axis=(1, 0, 0))
    material(d, "stainless_steel")
    return tube


def manifold(d):
    """The tube the valves hang off, across the front."""
    part(d, "Manifold")
    tube = cylinder(d, "Manifold tube", (-S.MANIFOLD_LEN / 2, 0, 0), (1, 0, 0),
                    S.MANIFOLD_D / 2, S.MANIFOLD_LEN)
    frame(d, "centre", (0.0, 0.0, 0.0), z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    material(d, "brass")
    return tube


def valve(d):
    """One burner valve: body, stem, and the orifice that feeds the venturi."""
    part(d, "Valve")
    w, h, dp = S.VALVE_BODY
    body = box(d, "Body", (-w / 2, -dp / 2, -h / 2), w, h, dp)
    stem = cylinder(d, "Stem", (0, -dp / 2 - 30.0, 0), (0, 1, 0),
                    S.VALVE_STEM_D / 2, 30.0)
    body = boolean(d, "Body and stem", body, stem)
    frame(d, "stem_end", (0.0, -dp / 2 - 30.0, 0.0), z_axis=(0, -1, 0),
          x_axis=(1, 0, 0))
    frame(d, "outlet", (0.0, dp / 2, 0.0), z_axis=(0, 1, 0), x_axis=(1, 0, 0))
    material(d, "brass")
    return body


def knob(d):
    """A control knob, with the flat a thumb finds."""
    part(d, "Knob")
    body = cylinder(d, "Knob body", (0, 0, 0), (0, -1, 0), S.KNOB_D / 2,
                    S.KNOB_H)
    grip = box(d, "Pointer", (-3.0, -S.KNOB_H, 0.0), 6.0, S.KNOB_H,
               S.KNOB_D / 2 + 6.0)
    body = boolean(d, "Knob and pointer", body, grip)
    frame(d, "shaft", (0.0, 0.0, 0.0), z_axis=(0, -1, 0), x_axis=(1, 0, 0))
    material(d, "plastic_black")
    return body


def grate(d):
    """One of three cooking-grate sections: two rails and the bars between."""
    part(d, "Cooking grate")
    rail_w = 10.0
    left = box(d, "Left rail", (-S.GRATE_X / 2, -S.GRATE_Y / 2, 0.0),
               rail_w, S.GRATE_Y, S.GRATE_BAR_D)
    right = box(d, "Right rail", (S.GRATE_X / 2 - rail_w, -S.GRATE_Y / 2, 0.0),
                rail_w, S.GRATE_Y, S.GRATE_BAR_D)
    left = boolean(d, "Rails", left, right)
    bar = cylinder(d, "Bar", (-S.GRATE_X / 2 + rail_w, -S.GRATE_Y / 2 + 10.0,
                              S.GRATE_BAR_D / 2), (1, 0, 0),
                   S.GRATE_BAR_D / 2, S.GRATE_X - 2 * rail_w)
    count = int((S.GRATE_Y - 20.0) // S.GRATE_PITCH) + 1
    r = d.call("add_feature", feature={
        "type": "Pattern", "name": "Bars", "kind": "linear",
        "source_id": bar, "count": count, "spacing": S.GRATE_PITCH,
        "axis": {"kind": "y"}})
    check(r, "grate bars")
    frame(d, "seat", (0.0, 0.0, 0.0))
    material(d, "steel")  # porcelain-coated cast iron in life
    return left


def warming_rack(d):
    """The half-shelf up under the lid."""
    part(d, "Warming rack")
    rail_d = S.RACK_BAR_D
    frame_x = S.COOK_X - 20.0
    left = cylinder(d, "Front rail", (-frame_x / 2, -S.RACK_Y / 2, 0),
                    (1, 0, 0), rail_d / 2, frame_x)
    back = cylinder(d, "Back rail", (-frame_x / 2, S.RACK_Y / 2, 0),
                    (1, 0, 0), rail_d / 2, frame_x)
    left = boolean(d, "Rails", left, back)
    bar = cylinder(d, "Bar", (-frame_x / 2 + 20.0, -S.RACK_Y / 2, 0),
                   (0, 1, 0), rail_d / 2, S.RACK_Y)
    count = int((frame_x - 40.0) // 40.0) + 1
    r = d.call("add_feature", feature={
        "type": "Pattern", "name": "Bars", "kind": "linear",
        "source_id": bar, "count": count, "spacing": 40.0,
        "axis": {"kind": "x"}})
    check(r, "rack bars")
    frame(d, "hang", (0.0, S.RACK_Y / 2, 0.0))
    material(d, "stainless_steel")
    return left


def wheel(d):
    part(d, "Wheel")
    # Built on the axle's own axis (X), so the hub frame's z — which is what
    # a revolute turns about — is the axis it really spins on.
    body = cylinder(d, "Tyre", (-S.WHEEL_W / 2, 0, 0), (1, 0, 0),
                    S.WHEEL_D / 2, S.WHEEL_W)
    bore = cylinder(d, "Bore", (-S.WHEEL_W / 2 - 1, 0, 0), (1, 0, 0),
                    S.AXLE_D / 2, S.WHEEL_W + 2)
    body = boolean(d, "Bored", body, bore, op="subtract")
    frame(d, "hub", (0.0, 0.0, 0.0), z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    material(d, "plastic_black")
    return body


def axle(d):
    part(d, "Axle")
    rod = cylinder(d, "Rod", (-S.CART_X / 2 - 40.0, 0, 0), (1, 0, 0),
                   S.AXLE_D / 2, S.CART_X + 80.0)
    # One frame per wheel: a revolute aligns frame ORIGINS, so both wheels
    # mated to a single centre frame ended up in the same place, inside the
    # cart.
    frame(d, "centre", (0.0, 0.0, 0.0), z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    for side, x in (("left_hub", -1), ("right_hub", +1)):
        frame(d, side, (x * (S.CART_X / 2 + S.WHEEL_W / 2 + 28.0), 0.0, 0.0),
              z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    material(d, "steel")
    return rod


def foot(d):
    part(d, "Foot")
    body = cylinder(d, "Foot", (0, 0, 0), (0, 0, 1), S.FOOT_D / 2, S.FOOT_H)
    frame(d, "top", (0.0, 0.0, S.FOOT_H), z_axis=(0, 0, 1), x_axis=(1, 0, 0))
    material(d, "rubber")
    return body


def tank(d):
    """A 9 kg propane bottle: a hollow shell, a foot ring, a collar you can
    get a hand into, and the valve it protects.

    Hollow on purpose: a solid cylinder of steel this size weighs a third of
    a tonne, and a BOM that says so is worse than no BOM.
    """
    part(d, "Propane tank")
    wall = 2.5
    body = cylinder(d, "Bottle", (0, 0, 20.0), (0, 0, 1), S.TANK_D / 2,
                    S.TANK_H)
    void = cylinder(d, "Bore", (0, 0, 20.0 + wall), (0, 0, 1),
                    S.TANK_D / 2 - wall, S.TANK_H - 2 * wall)
    body = boolean(d, "Shell", body, void, op="subtract")
    # A ring, like the collar: the bottle stands on a rolled skirt, and a
    # solid disc of steel here alone weighs more than the whole bottle.
    base_ring = cylinder(d, "Foot ring", (0, 0, 0), (0, 0, 1),
                         S.TANK_D / 2 - 10.0, 30.0)
    base_bore = cylinder(d, "Foot bore", (0, 0, -1.0), (0, 0, 1),
                         S.TANK_D / 2 - 10.0 - 4.0, 32.0)
    base_ring = boolean(d, "Foot skirt", base_ring, base_bore, op="subtract")
    body = boolean(d, "Bottle and foot", body, base_ring)
    # The collar is a ring, not a disc: it is what the bottle is carried by,
    # and it has to leave the valve reachable.
    collar = cylinder(d, "Collar", (0, 0, S.TANK_H), (0, 0, 1),
                      S.TANK_D / 2 - 40.0, S.TANK_COLLAR_H)
    collar_bore = cylinder(d, "Collar bore", (0, 0, S.TANK_H - 1.0), (0, 0, 1),
                           S.TANK_D / 2 - 52.0, S.TANK_COLLAR_H + 2.0)
    collar = boolean(d, "Collar ring", collar, collar_bore, op="subtract")
    body = boolean(d, "With collar", body, collar)
    valve_body = cylinder(d, "Valve", (0, 0, S.TANK_H + 10.0), (0, 0, 1),
                          22.0, S.TANK_COLLAR_H - 25.0)
    body = boolean(d, "With valve", body, valve_body)
    frame(d, "outlet", (0.0, 0.0, S.TANK_H + S.TANK_COLLAR_H - 15.0))
    frame(d, "floor", (0.0, 0.0, 0.0))
    material(d, "steel")
    return body


def regulator(d):
    """The regulator and the hose stub that reaches the manifold."""
    part(d, "Regulator")
    body = cylinder(d, "Bell", (0, 0, 0), (0, 0, -1), 35.0, 30.0)
    stub = cylinder(d, "Hose stub", (0, 0, -6.0), (1, 0, 0), 7.0, 90.0)
    body = boolean(d, "Bell and stub", body, stub)
    frame(d, "tank", (0.0, 0.0, 0.0))
    material(d, "brass")
    return body


def lid_handle(d):
    """A U of tube: the only part on the grill that is bent round a former."""
    part(d, "Lid handle")
    half = S.HANDLE_SPAN / 2
    bar = cylinder(d, "Bar", (-half, 0, 0), (1, 0, 0), S.HANDLE_D / 2,
                   S.HANDLE_SPAN)
    for side, x in (("Left", -half), ("Right", half)):
        leg = cylinder(d, side + " leg", (x, 0, 0), (0, 0, -1),
                       S.HANDLE_D / 2, S.HANDLE_RISE)
        bar = boolean(d, side + " joint", bar, leg)
    frame(d, "left_foot", (-half, 0.0, -S.HANDLE_RISE))
    material(d, "stainless_steel")
    return bar


def thermometer(d):
    part(d, "Thermometer")
    bezel = cylinder(d, "Bezel", (0, 0, 0), (0, 0, 1),
                     S.THERMO_BEZEL_D / 2, 18.0)
    stem = cylinder(d, "Stem", (0, 0, -40.0), (0, 0, 1),
                    S.THERMO_STEM_D / 2, 40.0)
    bezel = boolean(d, "Bezel and stem", bezel, stem)
    frame(d, "seat", (0.0, 0.0, 0.0))
    material(d, "stainless_steel")
    return bezel
