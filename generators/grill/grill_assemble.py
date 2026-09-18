"""Put the grill together — and JOIN it.

Every occurrence is placed by a mate to a socket on the part that carries it,
so nothing here is a transform typed into the assembly. What this round adds
is what HOLDS each one there:

- **bolted** — a clearance hole through both parts and `fit_fastener` on the
  rim, which derives the size from the hole and the length from the stack it
  passes through, and mates the parts it clamps;
- **welded** — the two parts are mated flush, face on face, so they touch;
- **pinned** — a pin line-to-line in both bores, and a revolute mate about
  the axis it makes;
- **resting** — a grate on its ledge, the bottle on its shelf: flush, and
  nothing more.

The check that it worked is `contact_report`: one connected component over
every occurrence, and no pair clashing.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import check, rim  # noqa: E402

import grill_spec as S  # noqa: E402


class Assembly:
    def __init__(self, d, name="Grill"):
        self.d = d
        # Rebuilding the assembly over a project whose parts are already
        # there is the inner loop of a round like this one, and a tab that
        # survived the last attempt is not the one this run describes.
        for p in d.call("list_parts")["parts"]:
            if p["name"] == name:
                d.call("delete_part", part=name)
        d.call("add_assembly", name=name)
        d.call("set_active_part", part=name)
        self.occ = {}
        self.turning = []

    def place(self, name, part, parent=None, socket="origin",
              frame="origin", kind="fastened", fixed=False):
        """One occurrence, and the ONE mate that says where it belongs."""
        d = self.d
        oid = d.call("add_occurrence", part=part, name=name,
                     fixed=fixed)["id"]
        self.occ[name] = oid
        if parent is not None:
            r = d.call("add_mate", type=kind, name=name, flipped=False,
                       a={"occurrence": oid, "frame": frame},
                       b={"occurrence": self.occ[parent], "frame": socket})
            check(r, "mate " + name)
            if kind == "revolute":
                self.turning.append(r["id"])
        return oid

    def close_up(self):
        """Every hinge at zero: the rest pose the grill is drawn in.

        A revolute leaves its angle free, and free is whatever the solver
        reaches from where the occurrence happened to start — which stood the
        lid bolt upright, 800 mm above the firebox, with its thermometer and
        its handle along with it. The doors and the knobs are the same joint.
        """
        for mate in self.turning:
            self.d.call("set_mate", id=mate, rotation_deg=0.0)


def quat_to_z(v):
    """The rotation that takes +Z to `v`, as {x, y, z, w}."""
    vx, vy, vz = v
    n = math.sqrt(vx * vx + vy * vy + vz * vz)
    vx, vy, vz = vx / n, vy / n, vz / n
    if vz > 1 - 1e-9:
        return [0.0, 0.0, 0.0, 1.0]
    if vz < -1 + 1e-9:
        return [1.0, 0.0, 0.0, 0.0]          # a half turn about X
    ax, ay, az = -vy, vx, 0.0                 # (0,0,1) x v
    s = math.sqrt(ax * ax + ay * ay)
    ax, ay = ax / s, ay / s
    angle = math.acos(max(-1.0, min(1.0, vz)))
    return [ax * math.sin(angle / 2), ay * math.sin(angle / 2), 0.0,
            math.cos(angle / 2)]


def build(d, ctx):
    """Place every part, mated to what carries it."""
    a = Assembly(d)
    place = a.place

    # --- the cart, out from its left-hand side ---------------------------
    place("Cart side left", "Cart side left", fixed=True)
    place("Cart back", "Cart back", parent="Cart side left")
    place("Cart side right", "Cart side right", parent="Cart back")
    place("Cart shelf", "Cart shelf", parent="Cart side left")
    place("Control panel", "Control panel", parent="Cart side left")
    place("Firebox", "Firebox", parent="Cart side left")
    place("Hood", "Hood", parent="Firebox", socket="lid_hinge",
          frame="hinge", kind="revolute")

    for hand, side in ((-1, "left"), (1, "right")):
        cart = f"Cart side {side}"
        place(f"Side shelf {side}", "Side shelf", parent=cart,
              socket="side_shelf")
        place(f"Foot {side}", "Foot", parent=cart, socket="foot")
        place(f"Wheel {side}", "Wheel", parent=cart, socket="wheel",
              kind="revolute")
        place(f"Axle bolt {side}", "Axle bolt", parent=cart,
              socket="axle_bolt")
        door = f"Cart door {side}"
        place(door, door, parent=cart, socket="door", frame="hinge",
              kind="revolute")
        for i in (1, 2):
            place(f"Hinge leaf {side} {i} cart", "Hinge leaf", parent=cart,
                  socket=f"hinge_leaf_{i}")
            place(f"Hinge leaf {side} {i} door", "Hinge leaf", parent=door,
                  socket=f"hinge_leaf_{i}")
            place(f"Door hinge pin {side} {i}", "Door hinge pin", parent=cart,
                  socket=f"hinge_pin_{i}")

    place("Propane tank", "Propane tank", parent="Cart shelf", socket="tank")
    place("Regulator", "Regulator", parent="Propane tank", socket="outlet")

    # --- welded and resting inside the firebox ---------------------------
    for side in ("front", "back"):
        # A folded angle is mated by its HEEL, the corner its two legs
        # make: that is the line it is welded along.
        place(f"Grate ledge {side}", "Grate ledge", parent="Firebox",
              socket=f"grate_ledge_{side}", frame="heel")
        place(f"Tent ledge {side}", "Tent ledge", parent="Firebox",
              socket=f"tent_ledge_{side}", frame="heel")
    for side in ("left", "right"):
        place(f"Rack ledge {side}", "Rack ledge", parent="Firebox",
              socket=f"rack_ledge_{side}", frame="heel")
        place(f"Tray rail {side}", "Tray rail", parent="Firebox",
              socket=f"tray_rail_{side}", frame="heel")
        place(f"Hinge ear {side}", "Hinge ear", parent="Firebox",
              socket=f"box_ear_{side}")
        place(f"Lid hinge pin {side}", "Lid hinge pin", parent="Firebox",
              socket=f"hinge_pin_{side}")
        place(f"Hood end cap {side}", "Hood end cap", parent="Hood",
              socket=f"cap_{side}")
        place(f"Lid ear {side}", "Lid ear", parent="Hood",
              socket=f"lid_ear_{side}")
    place("Grease tray", "Grease tray", parent="Firebox", socket="tray",
          frame="floor")
    place("Warming rack", "Warming rack", parent="Firebox", socket="rack")
    place("Manifold", "Manifold", parent="Firebox", socket="manifold")
    for i in range(S.BURNERS):
        n = i + 1
        place(f"Burner {n}", "Burner", parent="Firebox", socket=f"burner_{n}")
        place(f"Heat tent {n}", "Heat tent", parent="Firebox",
              socket=f"tent_{n}", frame="ridge")
        place(f"Grate {n}", "Cooking grate", parent="Firebox",
              socket=f"grate_{n}")
        place(f"Valve {n}", "Valve", parent="Control panel",
              socket=f"valve_{n}")
        place(f"Knob {n}", "Knob", parent=f"Valve {n}", socket="knob",
              kind="revolute")

    place("Lid handle", "Lid handle", parent="Hood", socket="handle")
    place("Thermometer", "Thermometer", parent="Hood", socket="thermo")

    a.close_up()
    solved = d.call("solve_assembly")
    bad = [m for m in solved.get("mates", [])
           if m.get("status") not in ("satisfied", "redundant")]
    if bad:
        raise SystemExit(f"mates not solved: {bad}")
    return a


# --- the bolts -------------------------------------------------------------

def bolt(part, centre, axis, level, size, nut=True, occ=None, far=None):
    """One bolted joint, as the three things a fastener needs to be derived:
    the rim its head sits in, whether it takes a nut, and — when the axis
    carries on past the joint into something else — where the joint ENDS.

    That last one is not an optimisation. The firebox's hold-down bolts run
    down through the cart's top flange and on into the bottom shelf half a
    metre below, and without a far end the app sizes them to span the lot,
    or refuses because the far side is inside solid metal.
    """
    return {"part": part, "centre": centre, "axis": axis, "level": level,
            "size": size, "nut": nut, "occ": occ, "far": far}


def joints(ctx):
    """Every bolted joint on the grill.

    The holes themselves are cut by the two parts that share them, from the
    same table in grill_spec this reads — so a bolt cannot be fitted to a
    hole that was moved in one part and not the other.
    """
    out = []
    back_face = S.CART_BACK - 2 * S.T_CART
    panel_face = S.PANEL_FACE - S.T_PANEL
    flange_far = S.CART_H - S.T_CART           # top flange, underside
    front_far = S.CART_FRONT + S.T_CART        # front flange, back face
    for x, y in S.FIREBOX_BOLTS:
        side = "right" if x > 0 else "left"
        out.append(bolt("Firebox", (x, y, S.BASE_TOP), "z", S.BASE_TOP,
                        S.FIREBOX_BOLT,
                        far=(f"Cart side {side}", (x, y, flange_far), "z",
                             flange_far, S.CLEAR[S.FIREBOX_BOLT], None)))
    for x, z in S.BACK_BOLTS:
        out.append(bolt("Cart back", (x, back_face, z), "y", back_face,
                        S.BACK_BOLT))
    for x, y in S.SHELF_BOLTS:
        lvl = S.SHELF_Z + S.T_CART
        out.append(bolt("Cart shelf", (x, y, lvl), "z", lvl, S.SHELF_BOLT))
    for x, z in S.PANEL_BOLTS:
        side = "right" if x > 0 else "left"
        out.append(bolt("Control panel", (x, panel_face, z), "y", panel_face,
                        S.PANEL_BOLT,
                        far=(f"Cart side {side}", (x, front_far, z), "y",
                             front_far, S.CLEAR[S.PANEL_BOLT], None)))
    for x in S.PANEL_LIP_BOLTS:
        lip = S.FRONT_WALL_OUT - S.T_PANEL
        out.append(bolt("Control panel", (x, lip, S.PANEL_LIP_Z), "y", lip,
                        S.PANEL_LIP_BOLT,
                        far=("Firebox", (x, S.FRONT_WALL_IN, S.PANEL_LIP_Z),
                             "y", S.FRONT_WALL_IN,
                             S.CLEAR[S.PANEL_LIP_BOLT], None)))
    # A screw into a tapped hole ENDS at the bottom of the tapping. Without
    # saying so the app refuses it — rightly: past the blind hole the axis
    # runs on into the firebox's front wall, and a screw that "reached" that
    # would be sized to span the gas train.
    for i in range(S.BURNERS):
        for sx in S.sides():
            out.append(bolt("Control panel",
                            (S.burner_x(i) + sx * S.VALVE_SCREW_X, panel_face,
                             S.BURNER_Z), "y", panel_face, S.VALVE_BOLT,
                            nut=False,
                            far=("Valve",
                                 (sx * S.VALVE_SCREW_X, 0.0, 10.0), "z", 10.0,
                                 S.TAP[S.VALVE_BOLT], f"Valve {i + 1}")))
        out.append(bolt("Firebox",
                        (S.burner_x(i), S.BACK_WALL_OUT, S.BURNER_Z), "y",
                        S.BACK_WALL_OUT, S.BURNER_BOLT, nut=False,
                        far=("Burner",
                             (0.0, 0.0, S.BURNER_LEN - S.BURNER_TAP_DEPTH),
                             "z", S.BURNER_LEN - S.BURNER_TAP_DEPTH,
                             S.TAP[S.BURNER_BOLT], f"Burner {i + 1}")))
    for sx in S.sides():
        out.append(bolt("Hood", (sx * S.HANDLE_SPAN / 2.0,
                                 S.FB_Y + S.HANDLE_Y, ctx["z_flat"]), "z",
                        ctx["z_flat"], S.HANDLE_BOLT, nut=False))
    for hand, side in ((-1, "left"), (1, "right")):
        out.append(bolt(f"Cart side {side}",
                        (hand * S.FLANGE_BOLT_X, S.FOOT_Y, S.T_CART), "z",
                        S.T_CART, S.FOOT_BOLT, nut=False))
        # A part used more than once is addressed by its OCCURRENCE: the side
        # shelf's holes are the same two in its own coordinates, and which
        # shelf they belong to is the only thing that tells them apart.
        face = -(S.SIDE_SHELF_X / 2.0 - S.T_SHELF)
        inner = hand * S.SIDE_IN
        for y in S.SIDE_SHELF_BOLTS:
            out.append(bolt("Side shelf",
                            (face, y, -(S.SIDE_SHELF_Z - S.SIDE_SHELF_BOLT_Z)),
                            "x", face, S.SIDE_SHELF_BOLT,
                            occ=f"Side shelf {side}",
                            # The left shelf is the same part turned end
                            # for end, so its own -100 hole is the cart's
                            # +100 one: the hand is what converts between
                            # the part's coordinates and the world's.
                            far=(f"Cart side {side}",
                                 (inner, S.SIDE_SHELF_Y_C + hand * y,
                                  S.SIDE_SHELF_BOLT_Z), "x", inner,
                                 S.CLEAR[S.SIDE_SHELF_BOLT], None)))
        leaf_face = S.HINGE_KNUCKLE_D / 2.0 - S.T_BRK
        knuckle_y = S.CART_FRONT - S.T_CART - S.HINGE_TANG
        for i, z0 in enumerate(S.DOOR_HINGE_Z):
            for x in S.HINGE_BOLT_X:
                out.append(bolt("Hinge leaf",
                                (x, leaf_face, S.HINGE_LEAF[1] / 2.0), "y",
                                leaf_face, S.HINGE_BOLT,
                                occ=f"Hinge leaf {side} {i + 1} cart",
                                far=(f"Cart side {side}",
                                     (inner, knuckle_y + x,
                                      S.hinge_bolt_z(hand, z0)), "x",
                                     inner, S.CLEAR[S.HINGE_BOLT], None)))
    return out


def fasten(d, a, ctx):
    """Derive and place every bolt from the hole it goes in."""
    made = []
    for j in joints(ctx):
        # ALWAYS by occurrence, even for a part placed once. A rim is
        # {feature_id, element}, and a feature id is unique within its part,
        # not across the project — so the right-hand cart side's hole,
        # handed to the app without an occurrence, was answered with the
        # LEFT one's, 624 mm away.
        occ = a.occ[j["occ"] or j["part"]]
        kw = {"rim": rim(d, j["part"], j["centre"], j["axis"], j["level"],
                         S.CLEAR[j["size"]], occurrence=occ),
              "standard": "iso7380", "size": j["size"]}
        if j["nut"]:
            kw["nut"] = "iso4032"
        if j["far"]:
            fpart, fcentre, faxis, flevel, fdia, focc = j["far"]
            kw["far_end"] = rim(d, fpart, fcentre, faxis, flevel, fdia,
                                occurrence=a.occ[focc or fpart])
        try:
            r = d.call("fit_fastener", **kw)
        except Exception as e:
            raise SystemExit(
                f"{j['size']} in {j['part']} at "
                f"{[round(c, 1) for c in j['centre']]}: {e}") from None
        made.append(r["summary"])
    return made


def nuts(d, a, ctx):
    """The two nuts that are not derived from a hole: a wheel's axle nut on
    the shoulder bolt, and the thermometer's on its stem."""
    out = []
    # A nut BEARS at its own origin and its body runs the other way down its
    # axis, so the axis it is given is the one pointing back at the joint.
    for hand, side in ((-1, "left"), (1, "right")):
        out.append(d.call("add_fastener", standard="iso4032", size="M10",
                          position=[hand * S.SIDE_IN, S.WHEEL_AXIS_Y,
                                    S.WHEEL_AXIS_Z],
                          orientation=quat_to_z((hand, 0, 0))))
    out.append(d.call("add_fastener", standard="iso4032", size="M10",
                      position=[0.0, S.FB_Y, ctx["z_flat"]],
                      orientation=quat_to_z((0, 0, 1))))
    return out


def assemble(d, ctx):
    a = build(d, ctx)
    fasten(d, a, ctx)
    nuts(d, a, ctx)
    d.call("solve_assembly")
    return a
