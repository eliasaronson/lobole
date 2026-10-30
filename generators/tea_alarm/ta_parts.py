# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The tea-ready alarm's parts, one function each, every one modelled in the
world frame (ta_spec's docstring) where it stands, with a Frame at each
joint: two parts joined at a joint carry the same frame, so every mate
starts at zero.

Bought, or made from bought metal: the strip (KANTHAL 94S), the frame rod,
the probe and the chime (1.4301 rod), the clip (1.4301 sheet), the pivot
(a stub of the rod), the ball (steel) and the screw (ISO 4762 M3). Printed:
the block, the ramp, the knob and the dish.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from _lib import check, circle, line  # noqa: E402

X = [1.0, 0.0, 0.0]
Y = [0.0, 1.0, 0.0]
Z = [0.0, 0.0, 1.0]


def plane_xz(y0):
    """The XZ plane at y0: u = x, v = z needs normal -Y (v = n x u)."""
    return {"origin": [0.0, y0, 0.0], "normal": [0.0, -1.0, 0.0], "uAxis": X}


def plane_xy(z0):
    return {"origin": [0.0, 0.0, z0], "normal": Z, "uAxis": X}


def polygon(pts):
    return [line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]


class Builder:
    """Features into the active part."""

    def __init__(self, d):
        self.d = d

    def add(self, spec, what=None):
        return check(self.d.call("add_feature", feature=spec),
                     what or spec.get("name"))

    def box(self, name, lo, hi):
        """A box from corner lo to hi: a rectangle on the XY plane at lo's
        z, extruded up."""
        return self.extrude(name, plane_xy(lo[2]),
                            polygon([(lo[0], lo[1]), (hi[0], lo[1]),
                                     (hi[0], hi[1]), (lo[0], hi[1])]),
                            hi[2] - lo[2])

    def cyl(self, name, c, axis, r, h):
        """A cylinder from centre c along the unit axis, r, h."""
        u = X if abs(axis[0]) < 0.9 else Y
        u = [u[i] - sum(u[k] * axis[k] for k in range(3)) * axis[i]
             for i in range(3)]
        n = math.sqrt(sum(v * v for v in u))
        pl = {"origin": [float(v) for v in c],
              "normal": [float(v) for v in axis],
              "uAxis": [v / n for v in u]}
        return self.extrude(name, pl, [circle((0.0, 0.0), r)], h)

    def sphere(self, name, c, r):
        """A sphere: a half disc turned about the vertical through c."""
        pl = {"origin": [float(v) for v in c], "normal": [0.0, -1.0, 0.0],
              "uAxis": X}
        sk = self.sketch(f"{name} section", pl, [
            {"type": "arc", "center": [0.0, 0.0], "radius": float(r),
             "start_angle": -math.pi / 2, "end_angle": math.pi / 2},
            line((0.0, r), (0.0, -r))])
        return self.add({"type": "Revolve", "name": name, "sketch_id": sk,
                         "axis": {"kind": "custom",
                                  "origin": [float(v) for v in c],
                                  "direction": Z},
                         "angle_deg": 360.0, "mode": "new_body"})["id"]

    def boolean(self, name, target, tool, op):
        return self.add({"type": "Boolean", "name": name, "target_id": target,
                         "tool_id": tool, "op": op})["id"]

    def sketch(self, name, plane, ents):
        return self.add({"type": "Sketch", "name": name, "plane": plane,
                         "entities": ents})["id"]

    def extrude(self, name, plane, ents, distance, mode="new_body"):
        sk = self.sketch(f"{name} sketch", plane, ents)
        return self.add({"type": "Extrude", "name": name, "sketch_id": sk,
                         "distance": float(distance), "mode": mode})["id"]

    def holes(self, name, origin, normal, u, pts, dia, depth,
              printed="teardrop"):
        """A Hole feature: blind holes `depth` deep drilled against `normal`
        from the plane through `origin`, at the world points `pts` on it;
        teardrop where the hole lies across the part's print direction."""
        n = normal
        v = (n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2],
             n[0] * u[1] - n[1] * u[0])
        ents = []
        for q in pts:
            dd = [q[i] - origin[i] for i in range(3)]
            ents.append({"type": "point",
                         "p": [sum(dd[i] * u[i] for i in range(3)),
                               sum(dd[i] * v[i] for i in range(3))]})
        sk = self.sketch(f"{name} centres", {
            "origin": [float(x) for x in origin],
            "normal": [float(x) for x in normal],
            "uAxis": [float(x) for x in u]}, ents)
        spec = {"type": "Hole", "name": name, "sketch_id": sk,
                "hole_kind": "simple", "diameter": float(dia),
                "depth": float(depth), "through": False}
        if printed:
            spec["printed"] = printed
        return self.add(spec)["id"]

    def frame(self, name, origin, z_axis, x_axis):
        return self.add({"type": "Frame", "name": name, "kind": "explicit",
                         "origin": [float(v) for v in origin],
                         "z_axis": z_axis, "x_axis": x_axis})["id"]


# ---- the joints ------------------------------------------------------------------

def joint_frame(s, name):
    """A frame two parts share: (origin, z, x)."""
    if name == "guide":      # the probe slides up its guide, along +Z
        return (0.0, 0.0, s["z_top"]), Z, X
    if name == "pivot":      # the ramp turns about +Y: a positive turn
        # takes +X (the probe's end) DOWN, so the probe lifting it is
        # a negative turn
        return (s["r_arm"], 0.0, s["z_pivot"]), Y, X
    if name == "screw":
        return (0.0, 0.0, s["z_arm"] + s["arm_t"]), Z, X
    if name == "ball":
        return (s["x_ball"], 0.0, s["z_ball"]), Z, X
    if name == "chime":
        return (s["x_block1"], 0.0, s["z_chime"]), X, Z
    if name == "dish":
        return (s["x_block1"], 0.0, s["z_top"] - s["block_h"]), Z, X
    raise KeyError(name)


def frames(b, s, names):
    for n in names:
        o, z, x = joint_frame(s, n)
        b.frame(n, o, z, x)


# ---- the metal ------------------------------------------------------------------------

def strip(b, s):
    """The KANTHAL 94S strip, straight (flat as bought), from its clamp on
    the frame rod to just past the probe's axis, high-expansion side up."""
    b.box("Strip", (s["x_clamp"], -s["b"] / 2, s["z_strip"] - s["s"]),
          (s["d"] / 2.0, s["b"] / 2, s["z_strip"]))


def rod_points(s):
    """The frame rod's centreline corners: the foot against the strip's
    clamp, up the inside of the wall to the riser's top, and across to the
    block, where it ends in the block's mount face."""
    return [[s["x_rod"], 0.0, s["z_strip"] - s["s"] / 2.0],
            [s["x_rod"], 0.0, s["z_run"]],
            [s["x_block0"] + s["mount_depth"], 0.0, s["z_run"]]]


def frame_rod(b, s):
    path = b.add({"type": "Curve3D", "name": "Centreline", "kind": "bent_path",
                  "radius": s["bend_r"], "points": rod_points(s)})["id"]
    b.add({"type": "Pipe", "name": "Rod", "path_id": path,
           "outer_diameter": s["d"], "wall_thickness": 0.0})
    return path


def probe(b, s):
    """A straight stub of the rod: from the strip's tip up the guide, its
    top under the screw when the ramp rests (at contact)."""
    b.cyl("Probe", (0.0, 0.0, s["z_strip"]), Z, s["d"] / 2.0,
          s["probe_len"])
    frames(b, s, ["guide"])


def chime(b, s):
    """A cantilever of the rod, pressed into the block's far face, under
    the ramp's lip: struck by the ball, it rings at its first mode."""
    b.cyl("Chime", (s["x_block1"] - s["chime_in"], 0.0, s["z_chime"]), X,
          s["d"] / 2.0, s["chime_len"] + s["chime_in"])


def pivot(b, s):
    """A stub of the rod across the block's cheeks: the ramp turns on it."""
    w = s["block_w"]
    b.cyl("Pivot", (s["r_arm"], -w / 2.0, s["z_pivot"]), Y, s["d"] / 2.0, w)
    frames(b, s, ["pivot"])


def ball(b, s):
    b.sphere("Ball", (s["x_ball"], 0.0, s["z_ball"]), s["ball_d"] / 2.0)


def clip(b, s):
    """The 1.4301 spring clip over the rim, as it stands opened by the wall:
    a bridge across the rim's top and a leaf down either side, each leaf's
    inner face on the wall's face (the inner leaf where the wall's
    concave face meets its edges). The inner leaf holds the rod against
    the wall; the outer one is the spring. A sheet-metal part
    (docs/sheetmetal/README.md): a base flange and an edge flange each
    side, bent down with the material outside the bridge's outline.
    Returns (the base flange's id, the flat pattern's id)."""
    w = s["clip_w"]
    xi, xo = s["x_leaf_in"], s["x_leaf_out"]
    sk = b.sketch("Bridge profile", plane_xy(0.0),
                  polygon([(xo, -w / 2), (xi, -w / 2), (xi, w / 2),
                           (xo, w / 2)]))
    base = b.add({"type": "BaseFlange", "name": "Bridge",
                  "sketch_id": sk})["id"]
    edges = [e for e in b.d.call("list_elements")["bodies"][0]["edges"]
             if abs(e["midpoint"][1]) < 1e-6 and abs(e["midpoint"][2]) < 1e-6]
    if len(edges) != 2:
        raise SystemExit(f"tea alarm: the clip's bridge has {len(edges)} "
                         "edges along the rim, not two")
    for e in sorted(edges, key=lambda e: e["midpoint"][0]):
        side = "outer" if e["midpoint"][0] < (xi + xo) / 2 else "inner"
        b.add({"type": "EdgeFlange", "name": f"{side.capitalize()} leaf",
               "length": s["leaf"], "angle_deg": 90.0,
               "position": "material_outside",
               "edges": [{"feature_id": base, "element": e["name"]}]})
    flat = b.add({"type": "FlatPattern", "name": "Flat pattern",
                  "body_id": base})["id"]
    b.d.call("suppress_feature", feature_id=flat, suppressed=True)
    return base, flat


def vessel(b, s):
    """The mug's wall where the clip grips it: a sector of the wall either
    side of the clip, as deep as the leaves and a wall more, its top a full
    round (radius half the wall), turned about the mug's axis. It stands
    in for the vessel in the clip's contact study; the whole mug would
    only mesh to no purpose."""
    R, W, rr = s["R"], s["W"], s["rim_r"]
    depth = s["leaf"] + 2.0 * W
    xm = -(R + W / 2.0)
    prof = [line((-R, -depth), (-R, -rr)),
            {"type": "arc", "center": [xm, -rr], "radius": rr,
             "start_angle": 0.0, "end_angle": math.pi},
            line((-(R + W), -rr), (-(R + W), -depth)),
            line((-(R + W), -depth), (-R, -depth))]
    sk = b.sketch("Wall section", plane_xz(0.0), prof)
    return b.add({"type": "Revolve", "name": "Wall", "sketch_id": sk,
                  "axis": {"kind": "z"}, "angle_deg": s["sector_deg"],
                  "symmetric": True, "mode": "new_body"})["id"]


# ---- the printed parts ----------------------------------------------------------------

def block(b, s):
    """The printed block over the mug: the rod's end in its near face (the
    mount face), the probe's guide through it on the mug's axis, two
    cheeks carrying the pivot, and the chime pressed into its far face."""
    w, x0, x1 = s["block_w"], s["x_block0"], s["x_block1"]
    zb = s["z_top"] - s["block_h"]
    body = b.box("Block", (x0, -w / 2, zb), (x1, w / 2, s["z_top"]))
    for k, sgn in (("near", -1), ("far", 1)):
        y0 = sgn * (w / 2 - s["cheek_t"]) if sgn > 0 else -w / 2
        cheek = b.box(f"Cheek {k}", (s["r_arm"] - s["cheek_l"] / 2, y0,
                                     s["z_top"]),
                      (s["r_arm"] + s["cheek_l"] / 2, y0 + s["cheek_t"],
                       s["z_pivot"] + s["cheek_l"] / 2))
        body = b.boolean(f"Cheek {k} on", body, cheek, "union")
    guide = b.cyl("Guide", (0.0, 0.0, zb - 1.0), Z, s["guide_d"] / 2.0,
                  s["block_h"] + 2.0)
    body = b.boolean("Guide bored", body, guide, "subtract")
    # The bores across the print direction, as teardrops: the rod's end
    # into the near face, the chime into the far one, the pivot through
    # both cheeks.
    b.holes("Rod bore", (x0, 0.0, 0.0), [-1.0, 0.0, 0.0], Y,
            [(x0, 0.0, s["z_run"])], s["d"], s["mount_depth"])
    b.holes("Chime bore", (x1, 0.0, 0.0), X, [0.0, -1.0, 0.0],
            [(x1, 0.0, s["z_chime"])], s["d"], s["chime_in"])
    b.holes("Pivot bore", (0.0, w / 2, 0.0), Y, X,
            [(s["r_arm"], w / 2, s["z_pivot"])], s["d"], w)
    frames(b, s, ["guide", "pivot"])
    return body


def ramp(b, s):
    """The balanced ramp: a bar on the pivot, its probe arm (with the
    knob's screw) heavier than its ball arm, so it rests on the block until
    the probe lifts it. The ball arm's top rises by phi toward its lip, and
    a stop by the pivot holds the ball: when the probe has lifted the arm
    by phi the slope is level and the ball goes."""
    w, t = s["arm_w"], s["arm_t"]
    za = s["z_arm"]
    x0, x1 = -s["arm_tail"], s["x_lip"]
    # The bar's side profile in XZ: flat under, its top level over the
    # probe arm and rising by phi along the ball arm.
    top_lip = za + t + (x1 - s["r_arm"]) * math.tan(s["phi"])
    prof = polygon([(x0, za), (x1, za), (x1, top_lip),
                    (s["r_arm"], za + t), (x0, za + t)])
    bar = b.extrude("Bar", plane_xz(w / 2), prof, w)   # extrudes -Y
    stop = b.box("Ball stop", (s["x_stop"], -w / 2, za + t),
                 (s["x_stop"] + s["stop_t"], w / 2, za + t + s["stop_h"]))
    bar = b.boolean("Stop on", bar, stop, "union")
    bar = b.boolean("Pivot bore", bar,
                    b.cyl("Pivot bore tool", (s["r_arm"], -w, s["z_pivot"]),
                          Y, s["pivot_bore"] / 2.0, 2 * w), "subtract")
    # The screw's tapping hole lies across the ramp's print direction.
    b.holes("Screw bore", (0.0, 0.0, za + t), Z, X, [(0.0, 0.0, za + t)],
            s["screw_bore"], t)
    frames(b, s, ["pivot"])
    return bar


def knob(b, s):
    """A printed knob over the screw's head, marked in degrees C round its
    rim (the marks are the drawing's)."""
    knob_id = b.cyl("Knob", (0.0, 0.0, s["z_knob"]), Z, s["knob_d"] / 2.0,
                    s["knob_h"])
    head = b.cyl("Head pocket", (0.0, 0.0, s["z_knob"] - 1.0), Z,
                 s["head_d"] / 2.0, s["head_h"] + 1.0)
    body = b.boolean("Pocket", knob_id, head, "subtract")
    return body


def dish(b, s):
    """A printed dish under the chime that catches the ball."""
    r, h, wall = s["dish_r"], s["dish_h"], s["dish_wall"]
    cup = b.cyl("Dish", (s["x_dish"], 0.0, s["z_dish"]), Z, r, h)
    hollow = b.cyl("Hollow", (s["x_dish"], 0.0, s["z_dish"] + wall), Z,
                   r - wall, h)
    body = b.boolean("Hollowed", cup, hollow, "subtract")
    arm = b.box("Hanger", (s["x_block1"] - s["hanger_t"], -s["hanger_t"],
                           s["z_dish"]),
                (s["x_dish"], s["hanger_t"], s["z_dish"] + wall))
    body = b.boolean("Hanger on", body, arm, "union")
    up = b.box("Hanger up", (s["x_block1"] - s["hanger_t"], -s["hanger_t"],
                             s["z_dish"]),
               (s["x_block1"], s["hanger_t"], s["z_top"] - s["block_h"] + 1.0))
    body = b.boolean("Hanger up on", body, up, "union")
    return body
