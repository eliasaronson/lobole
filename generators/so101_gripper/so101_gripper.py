#!/usr/bin/env python3
"""Build the SO-101 PG-52 wrist gripper: every part, then the assembly.

    python3 generators/so101_gripper/so101_gripper.py --port 8800

Everything is in the tool frame described in so101_gripper_spec.py. Parts
are built in their own local frames and PLACED by the assembly at transforms
from the link angle theta, so the whole mechanism can be swept.
"""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                          # this generator's own files
sys.path.insert(0, os.path.join(HERE, ".."))      # generators/: _lib, gear

import so101_gripper_spec as P                        # noqa: E402
from _lib import connect                              # noqa: E402
from gear import involute_profile                     # noqa: E402

# The servo comes from a local clone of the SO-ARM100 repository
# (github.com/TheRobotStudio/SO-ARM100): --so-arm100 DIR, else $SO_ARM100_DIR,
# else ~/SO-ARM100.
SO_ARM100_DEFAULT = os.environ.get("SO_ARM100_DIR",
                                   os.path.expanduser("~/SO-ARM100"))
SERVO_IN_REPO = os.path.join("STEP", "SO100", "STS3215_03a.step")
SX = (-P.A - 12.4, -P.A + 12.4)      # gripper servo case, in X
SZ = (P.ZA - 35.2, P.ZA + 10.2)      # gripper servo case, in Z
RIM = 5.8                            # material around a pivot post
PAD_FIX = (20.0, 35.0)               # the two pad fixings, in the finger's Z
XMAX = P.A + P.D + RIM               # chassis half-width in X


class Build:
    """Primitives and booleans, with a regen check on every feature."""

    def __init__(self, d):
        self.d = d

    def feat(self, **f):
        r = self.d.call("add_feature", feature=f)
        regen = r.get("regen", {})
        if regen and not regen.get("ok", True):
            raise SystemExit(f"REGEN FAILED after {f.get('name')}: {regen}")
        return r["id"]

    def box(self, name, lo, hi):
        a, c = list(lo), list(hi)
        lo = [min(a[i], c[i]) for i in range(3)]
        hi = [max(a[i], c[i]) for i in range(3)]
        return self.feat(type="Box", name=name, position=lo,
                         width=hi[0] - lo[0], height=hi[1] - lo[1],
                         depth=hi[2] - lo[2])

    def cyl(self, name, centre, direction, radius, height):
        return self.feat(type="Cylinder", name=name, center=list(centre),
                         direction=list(direction), radius=radius,
                         height=height)

    def boolean(self, op, target, tool, name):
        return self.feat(type="Boolean", name=name, target_id=target,
                         tool_id=tool, op=op)

    def unite_all(self, ids, name="body"):
        t = ids[0]
        for i, other in enumerate(ids[1:]):
            t = self.boolean("union", t, other, f"{name}+{i + 1}")
        return t

    def drill(self, target, centre, direction, radius, depth, name):
        tool = self.cyl(name + " ⌀", centre, direction, radius, depth)
        return self.boolean("subtract", target, tool, name)

    def frame(self, name, origin, z_axis=(0, 1, 0), x_axis=(1, 0, 0)):
        """A named datum frame: what a mate anchors to (docs: v86)."""
        return self.feat(type="Frame", name=name, kind="explicit",
                         origin=list(origin), z_axis=list(z_axis),
                         x_axis=list(x_axis))

    def carve(self, target, lo, hi, name):
        tool = self.box(name + " □", lo, hi)
        return self.boolean("subtract", target, tool, name)


def part(d, name, first=False):
    if first:
        d.call("rename_part", part=0, new_name=name)
    else:
        d.call("add_part", name=name)
    d.call("set_active_part", part=name)


# --------------------------------------------------------------------------
# link plates
# --------------------------------------------------------------------------
def wedge_body(b, radius, half_deg, thick, hand=1):
    """A pie of half-angle half_deg about +X (hand=-1: about -X)."""
    a = math.radians(half_deg)
    n = 10
    seg = a / n
    r = radius / math.cos(seg)          # circumscribe, so teeth are inside
    base = 0.0 if hand > 0 else math.pi
    pts = [(0.0, 0.0)] + [(r * math.cos(base - a + 2 * a * i / n),
                           r * math.sin(base - a + 2 * a * i / n))
                          for i in range(n + 1)]
    ents = [{"type": "line", "a": list(pts[i]), "b": list(pts[i + 1])}
            for i in range(len(pts) - 1)]
    ents.append({"type": "line", "a": list(pts[-1]), "b": list(pts[0])})
    sk = b.feat(type="Sketch", name="wedge",
                plane={"origin": [0, thick / 2.0, 0], "normal": [0, -1, 0],
                       "uAxis": [1, 0, 0]}, entities=ents)
    return b.feat(type="Extrude", name="wedge", sketch_id=sk, distance=thick,
                  mode="new_body")


def rotate_entities(ents, deg):
    """Turn a whole sketch profile about its origin."""
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)

    def rp(p):
        return [p[0] * ca - p[1] * sa, p[0] * sa + p[1] * ca]

    out = []
    for e in ents:
        e = dict(e)
        if e["type"] == "line":
            e["a"], e["b"] = rp(e["a"]), rp(e["b"])
        elif e["type"] == "arc":
            e["center"] = rp(e["center"])
            e["start_angle"] += a
            e["end_angle"] += a
        out.append(e)
    return out


def build_drive_plate(b, hand):
    """Pivot A1 at the origin, arm along +Z to A2, toothed sector toward
    hand*X. Symmetric in Y (a horn boss on each face), so the same part
    serves the +Y and -Y bands; `hand` is the only difference between the
    left and right plates, and the tooth pattern is identical in both --
    see params.GEAR for why that is what makes them mesh."""
    g, t2 = P.GEAR, P.PLATE_T / 2.0
    boss_r = P.SERVO["horn_dia"] / 2.0 + 0.5

    ids = [b.cyl("hub", (0, -t2, 0), (0, 1, 0), g["hub_r"], P.PLATE_T),
           b.box("arm", (-g["arm_half"], -t2, 0), (g["arm_half"], t2, P.L)),
           b.cyl("A2 pad", (0, -t2, P.L), (0, 1, 0), g["a2_pad_r"],
                 P.PLATE_T)]
    body = b.unite_all(ids, "plate")

    ents, radii = involute_profile(g["module"] * g["backlash_scale"],
                                   g["teeth"], g["pressure_deg"])
    if g["phase_quarter_pitch"]:
        ents = rotate_entities(ents, 360.0 / (4 * g["teeth"]))
    sk = b.feat(type="Sketch", name="gear profile",
                plane={"origin": [0, t2, 0], "normal": [0, -1, 0],
                       "uAxis": [1, 0, 0]}, entities=ents)
    gear = b.feat(type="Extrude", name="gear blank", sketch_id=sk,
                  distance=P.PLATE_T, mode="new_body")
    wedge = wedge_body(b, radii["ra"] + 1.0, g["sector_deg"], P.PLATE_T,
                       hand)
    body = b.boolean("union", body,
                     b.boolean("intersect", gear, wedge, "tooth sector"),
                     "plate+teeth")

    for s in (-1, 1):
        body = b.boolean("union", body,
                         b.cyl(f"horn boss {s:+d}", (0, s * t2, 0), (0, s, 0),
                               boss_r, P.BOSS_H), f"boss {s:+d}")
    deep = 2 * (t2 + P.BOSS_H) + 2
    p = P.SERVO["horn_hole_pitch"] / 2.0
    for sx in (-1, 1):
        for sz in (-1, 1):
            body = b.drill(body, (sx * p, -deep / 2, sz * p), (0, 1, 0), 1.7,
                           deep, f"horn hole {sx:+d}{sz:+d}")
    body = b.drill(body, (0, -deep / 2, 0), (0, 1, 0),
                   P.SERVO["horn_centre_dia"] / 2.0 + 0.4, deep,
                   "shaft screw relief")
    body = b.drill(body, (0, -t2 - 1, P.L), (0, 1, 0), 3.1, P.PLATE_T + 2,
                   "A2 bearing")
    b.frame("A1", (0, 0, 0))            # the pivot axis is Y, not the
    b.frame("A2", (0, 0, P.L))          # part origin's Z
    return body


def build_coupler_plate(b):
    t2, r = P.COUPLER_T / 2.0, P.GEAR["coupler_pad_r"]
    ids = [b.cyl("B1 end", (0, -t2, 0), (0, 1, 0), r, P.COUPLER_T),
           b.cyl("B2 end", (0, -t2, P.L), (0, 1, 0), r, P.COUPLER_T),
           b.box("bar", (-P.GEAR["coupler_half"], -t2, 0),
                 (P.GEAR["coupler_half"], t2, P.L))]
    body = b.unite_all(ids, "coupler")
    for z, nm in ((0.0, "B1"), (P.L, "B2")):
        body = b.drill(body, (0, -t2 - 1, z), (0, 1, 0), 3.1, P.COUPLER_T + 2,
                       f"{nm} bearing")
    b.frame("B1", (0, 0, 0))
    b.frame("B2", (0, 0, P.L))
    return body


# --------------------------------------------------------------------------
# fingers and pads
# --------------------------------------------------------------------------
def build_finger(b, hand):
    """A finger. Local frame: A2 at the origin, +X inboard, +Z forward;
    hand=+1 is the left finger, hand=-1 its mirror. The two ears reach back
    over BOTH pivots; the backing plate ties them together ahead of the
    links, which is why the links themselves need no yoke (and so nothing
    of the mechanism crosses the camera's view)."""
    s, c = hand, P.PAD_OFFSET
    e0, e1 = P.EAR
    tail = -(P.D + 7.0)
    ids = []
    for sy in (1, -1):
        lo, hi = (e0, e1) if sy > 0 else (-e1, -e0)
        ids += [b.box(f"ear {sy:+d}", (s * tail, lo, -5.5),
                      (s * (c - 3.0), hi, 7.0)),
                b.box(f"gusset {sy:+d}", (s * (c - 6.0), lo, 7.0),
                      (s * (c - 3.0), hi, 14.0)),
                b.cyl(f"A2 hub {sy:+d}", (0, lo, 0), (0, 1, 0), 6.0, hi - lo),
                b.cyl(f"B2 hub {sy:+d}", (s * -P.D, lo, 0), (0, 1, 0), 6.0,
                      hi - lo)]
    ids += [b.box("backing plate", (s * (c - 6.0), -e1, 7.0),
                  (s * (c - 3.0), e1, 40.0)),
            b.box("rib", (s * (c - 11.0), -3.0, 7.0),
                  (s * (c - 6.0), 3.0, 36.0)),
            b.box("rib+", (s * (c - 11.0), 12.0, 7.0),
                  (s * (c - 6.0), 18.0, 36.0)),
            b.box("rib-", (s * (c - 11.0), -18.0, 7.0),
                  (s * (c - 6.0), -12.0, 36.0))]
    body = b.unite_all(ids, "finger")

    # A2 turns on the drive plate, B2 on the coupler: two bands, so two
    # boss lengths. Each ends flush with the inboard face of its plate.
    for sy in (1, -1):
        inner = sy * P.EAR[0]
        for x, nm, band in ((0.0, "A2", P.PLATE), (s * -P.D, "B2", P.COUPLER)):
            body = b.boolean("union", body,
                             b.cyl(f"{nm} boss {sy:+d}", (x, inner, 0),
                                   (0, -sy, 0), 3.0, P.EAR[0] - band[0]),
                             f"{nm} boss {sy:+d}")
    for z in PAD_FIX:
        body = b.drill(body, (s * (c - 13.0), 0, z), (s, 0, 0), 1.7, 11.0,
                       f"pad screw {z:.0f}")
        body = b.drill(body, (s * (c - 6.5), 0, z), (s, 0, 0), 3.6, 4.0,
                       f"pad spigot {z:.0f}")
    # Thirty boolean cuts leave seams the B-Rep does not need; Refine heals
    # them (90 faces -> 55 on this one), which is what the FEM mesher and a
    # slicer both prefer.
    body = b.feat(type="Refine", name="heal", body_id=body)
    for sy in (1, -1):
        b.frame(f"A2{'+' if sy > 0 else '-'}", (0, sy * P.PLATE_MID, 0))
        b.frame(f"B2{'+' if sy > 0 else '-'}",
                (s * -P.D, sy * P.COUPLER_MID, 0))
    b.frame("pad seat", (s * (c - 3.0), 0, 0), z_axis=(0, 0, 1),
            x_axis=(s, 0, 0))
    # What the gear pair does, said as a frame: the two fingers may only
    # slide relative to each other along X. See motion.py.
    b.frame("coupling", (0, 0, 0), z_axis=(1, 0, 0), x_axis=(0, 0, 1))
    return body


def build_pad(b):
    """A replaceable jaw pad: 3 mm of grip, on two spigots that take the
    shear off the screws and locate it. Local X = 0 is the finger's face."""
    ids = [b.box("pad", (0, -22.0, 7.0), (3.0, 22.0, 40.0))]
    for z in PAD_FIX:
        ids.append(b.cyl(f"spigot {z:.0f}", (0, 0, z), (-1, 0, 0), 3.5, 2.5))
    body = b.unite_all(ids, "pad")
    for z in PAD_FIX:
        body = b.drill(body, (-3.0, 0, z), (1, 0, 0), 1.3, 5.5,
                       f"screw {z:.0f}")
    for i in range(11):
        z = 9.5 + 2.6 * i
        body = b.carve(body, (2.3, -22.0, z), (3.1, 22.0, z + 1.3),
                       f"groove {i}")
    return body


# --------------------------------------------------------------------------
# chassis
# --------------------------------------------------------------------------
def build_chassis(b):
    """The palm: it bolts to the wrist-roll horn, holds the servo, carries
    the four pivots and the camera pod.

    Everything except the hub starts at Z0, because the arm's wrist bracket
    comes 4.7 mm past the horn face and the roll joint turns nearly all the
    way round (params.MOUNT)."""
    m, w, z0 = P.MOUNT, P.WALL, P.Z0
    # The hub is STEPPED, to the shape of the room the arm leaves: only
    # Ø20 (no wider than the horn) down at the face, widening to Ø28 above
    # the bracket's 1.6 mm ring, and the plate proper above all of it.
    ids = [b.cyl("horn hub", (0, 0, 0), (0, 0, 1), m["hub_dia"] / 2.0, 2.5),
           b.cyl("hub step", (0, 0, 2.5), (0, 0, 1), 14.0, z0 - 2.5),
           b.box("mount plate", (-XMAX, -20.0, z0), (XMAX, 20.0,
                                                     z0 + m["plate_t"]))]
    for sy in (1, -1):
        lo, hi = (w[0], w[1]) if sy > 0 else (-w[1], -w[0])
        ids += [b.box(f"wall servo {sy:+d}", (-XMAX, lo, z0),
                      (SX[1] + 3.0, hi, SZ[1] - 4.0)),
                b.box(f"wall base {sy:+d}", (SX[1] + 3.0, lo, z0),
                      (XMAX, hi, z0 + 22.0)),
                b.box(f"wall arch {sy:+d}", (P.A - 6.0, lo, z0 + 22.0),
                      (XMAX, hi, P.ZA)),
                b.cyl(f"A1R rim {sy:+d}", (P.A, lo, P.ZA), (0, 1, 0), 14.0,
                      hi - lo)]
        for s in (-1, 1):
            ids.append(b.cyl(f"B1 rim {s:+d}{sy:+d}",
                             (s * (P.A + P.D), lo, P.ZA), (0, 1, 0), RIM,
                             hi - lo))
    ids += [b.box("servo seat", (SX[0], -16.0, z0 + m["plate_t"]),
                  (SX[1], 16.0, SZ[0])),
            b.box("servo back wall", (SX[0] - 3.0, -w[0], z0 + m["plate_t"]),
                  (SX[0], w[0], SZ[1] - 4.0))]
    for sy in (1, -1):
        lo, hi = (10.0, w[1]) if sy > 0 else (-w[1], -10.0)
        ids.append(b.box(f"servo stop {sy:+d}", (SX[1], lo,
                                                 z0 + m["plate_t"]),
                         (SX[1] + 3.0, hi, z0 + 27.0)))
    for s in (-1, 1):
        ids.append(b.cyl(f"pod boss {s:+d}", (8.0, s * 9.0, z0), (0, 0, 1),
                         4.5, P.CAM["pod_base_z"] - z0))
    body = b.unite_all(ids, "chassis")

    # coupler pivot posts: the plates turn on these, an M3 retains them
    for s in (-1, 1):
        for sy in (1, -1):
            y = sy * P.PLATE[0]
            body = b.boolean("union", body,
                             b.cyl(f"B1 post {s:+d}{sy:+d}",
                                   (s * (P.A + P.D), y, P.ZA), (0, sy, 0),
                                   3.0, P.COUPLER[1] - P.PLATE[0]),
                             f"B1 post {s:+d}{sy:+d}")
            body = b.drill(body, (s * (P.A + P.D), sy * (P.COUPLER[1] + 0.5),
                                  P.ZA), (0, -sy, 0), 1.25,
                           P.COUPLER[1] - P.WALL[0] + 1.0,
                           f"B1 core {s:+d}{sy:+d}")
    # wrist-roll horn: four M3 through the hub, heads sunk in the hub
    p = m["hole_pitch"] / 2.0
    for sx in (-1, 1):
        for sz in (-1, 1):
            body = b.drill(body, (sx * p, sz * p, -1), (0, 0, 1),
                           m["hole_dia"] / 2.0, 5.0,
                           f"mount {sx:+d}{sz:+d}")
            body = b.drill(body, (sx * p, sz * p, 2.5), (0, 0, 1), 3.25, 3.0,
                           f"mount cbore {sx:+d}{sz:+d}")
    body = b.drill(body, (0, 0, -1), (0, 0, 1), m["centre_relief_dia"] / 2.0,
                   z0 + m["plate_t"] + 2, "horn screw relief")
    # journals: the drive plates' bosses turn in these
    for s in (-1, 1):
        for sy in (1, -1):
            body = b.drill(body, (s * P.A, sy * (w[1] + 1), P.ZA),
                           (0, -sy, 0), m["horn_clear_dia"] / 2.0,
                           w[1] - w[0] + 2, f"journal {s:+d}{sy:+d}")
    # cable window (-Y is where the servo's connectors are), both sides
    for sy in (1, -1):
        lo, hi = (w[0] - 1, w[1] + 1) if sy > 0 else (-w[1] - 1, -w[0] + 1)
        body = b.carve(body, (SX[0] + 3.0, lo, z0 + 9.0),
                       (SX[1] - 3.0, hi, z0 + 30.0), f"window {sy:+d}")
    for s in (-1, 1):
        body = b.drill(body, (8.0, s * 9.0, z0 - 1), (0, 0, 1), 1.25, 10.0,
                       f"pod screw {s:+d}")
    # the servo's own two case screws, through the +Y wall
    for s in (-1, 1):
        body = b.drill(body, (-P.A + s * 10.25, w[1] + 1, SZ[0] + 2.0),
                       (0, -1, 0), 1.1, w[1] - w[0] + 2, f"servo screw {s:+d}")
        body = b.drill(body, (-P.A + s * 10.25, w[1] + 0.1, SZ[0] + 2.0),
                       (0, -1, 0), 2.1, 1.6, f"servo cbore {s:+d}")
    # --- what the mates anchor to: every pivot axis, by name
    for s, sn in ((-1, "L"), (1, "R")):
        for sy, yn in ((1, "+"), (-1, "-")):
            b.frame(f"A1 {sn}{yn}", (s * P.A, sy * P.PLATE_MID, P.ZA))
            b.frame(f"B1 {sn}{yn}",
                    (s * (P.A + P.D), sy * P.COUPLER_MID, P.ZA))
    b.frame("servo seat", (-P.A, 0.0, P.ZA - 12.5), z_axis=(0, 1, 0),
            x_axis=(0, 0, 1))
    b.frame("pod seat", (4.0, 0.0, P.CAM["pod_base_z"]), z_axis=(0, 0, 1),
            x_axis=(1, 0, 0))
    return body


def build_camera_pod(b):
    """A wedge that aims the 32x32 module at the grasp point. Local origin
    sits on the chassis pedestal; the seat is tilted by CAM.tilt and
    recessed, so the module's components hang inside it and the PCB lands
    flat on the rim."""
    c = P.CAM
    t = math.radians(c["tilt_deg"])
    n = (-math.sin(t), 0.0, math.cos(t))      # seat normal, points out
    u = (math.cos(t), 0.0, math.sin(t))       # in-plane, up the slope
    cen = (12.0, 0.0, c["centre"][2] - c["pod_base_z"])   # seat centre
    z0 = cen[2] - cen[0] * math.tan(t)        # seat height at x = 0
    z1 = cen[2] + (26.0 - cen[0]) * math.tan(t)
    ents = [{"type": "line", "a": [0.0, 0.0], "b": [26.0, 0.0]},
            {"type": "line", "a": [26.0, 0.0], "b": [26.0, z1]},
            {"type": "line", "a": [26.0, z1], "b": [0.0, z0]},
            {"type": "line", "a": [0.0, z0], "b": [0.0, 0.0]}]
    sk = b.feat(type="Sketch", name="pod profile",
                plane={"origin": [0, 16.5, 0], "normal": [0, -1, 0],
                       "uAxis": [1, 0, 0]}, entities=ents)
    body = b.feat(type="Extrude", name="pod", sketch_id=sk, distance=33.0,
                  mode="new_body")

    # the component recess, square, sunk along the seat normal
    h = c["pocket"] / 2.0
    sq = [{"type": "line", "a": [-h, -h], "b": [h, -h]},
          {"type": "line", "a": [h, -h], "b": [h, h]},
          {"type": "line", "a": [h, h], "b": [-h, h]},
          {"type": "line", "a": [-h, h], "b": [-h, -h]}]
    sk2 = b.feat(type="Sketch", name="recess",
                 plane={"origin": [cen[0] + n[0], 0.0, cen[2] + n[2]],
                        "normal": [-n[0], 0, -n[2]], "uAxis": [0, 1, 0]},
                 entities=sq)
    body = b.boolean("subtract", body,
                     b.feat(type="Extrude", name="recess", sketch_id=sk2,
                            distance=7.0, mode="new_body"), "recess")

    hp = c["hole_pitch"] / 2.0
    for su in (-1, 1):
        for sv in (-1, 1):
            o = (cen[0] + su * hp * u[0], sv * hp, cen[2] + su * hp * u[2])
            body = b.drill(body, (o[0] + n[0] * 2, o[1], o[2] + n[2] * 2),
                           (-n[0], 0, -n[2]), 0.85, 14.0,
                           f"M2 {su:+d}{sv:+d}")
    body = b.drill(body, (cen[0] + n[0] * 2, 0.0, cen[2] + n[2] * 2),
                   (-n[0], 0, -n[2]), 8.0, 30.0, "lens bore")
    for s in (-1, 1):
        body = b.drill(body, (4.0, s * 9.0, -1.0), (0, 0, 1), 1.7, 12.0,
                       f"chassis screw {s:+d}")
        body = b.drill(body, (4.0, s * 9.0, 3.2), (0, 0, 1), 3.2, 6.0,
                       f"chassis cbore {s:+d}")
    b.frame("camera seat", cen, z_axis=n, x_axis=u)
    return body


def build_camera_mock(b):
    c = P.CAM
    h = c["board"] / 2.0
    ids = [b.box("pcb", (-h, -h, 0.0), (h, h, c["pcb_t"])),
           b.box("components", (-11.0, -11.0, -c["back_t"]),
                 (11.0, 11.0, 0.0)),
           b.cyl("lens", (0, 0, c["pcb_t"]), (0, 0, 1), c["lens_dia"] / 2.0,
                 c["lens_len"])]
    return b.unite_all(ids, "camera")


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--so-arm100", default=SO_ARM100_DEFAULT,
                    help="a clone of the SO-ARM100 repository (default: "
                         "$SO_ARM100_DIR, else ~/SO-ARM100)")
    a = ap.parse_args()
    servo_step = os.path.join(a.so_arm100, SERVO_IN_REPO)
    if not os.path.isfile(servo_step):
        raise SystemExit(f"no servo STEP at {servo_step}: pass --so-arm100 "
                         f"with a clone of the SO-ARM100 repository")
    d = connect(a.port)
    d.sock.settimeout(900)
    b = Build(d)
    d.call("new_project")
    for name, expr, comment in (
            ("pivot_offset", P.A, "drive pivot in X = gear pitch radius, mm"),
            ("pivot_height", P.ZA, "drive pivot, Z, mm"),
            ("link_length", P.L, "both links, mm"),
            ("base_spacing", P.D, "parallelogram base, mm"),
            ("max_opening", P.OPEN_MAX, "pad to pad, mm"),
            ("plate_thickness", P.PLATE_T, "link plates, mm")):
        d.call("set_variable", name=name, expression=str(expr),
               comment=comment, scope="project")

    jobs = [("Chassis", build_chassis),
            ("DrivePlate L", lambda bb: build_drive_plate(bb, +1)),
            ("DrivePlate R", lambda bb: build_drive_plate(bb, -1)),
            ("CouplerPlate", build_coupler_plate),
            ("Finger L", lambda bb: build_finger(bb, +1)),
            ("Finger R", lambda bb: build_finger(bb, -1)),
            ("Jaw Pad", build_pad), ("Camera Pod", build_camera_pod),
            ("Camera Module", build_camera_mock)]
    for i, (name, fn) in enumerate(jobs):
        part(d, name, first=(i == 0))
        fn(b)
        print(f"  built {name}")
    part(d, "STS3215")
    # Centred, which is the Import default — and every servo dimension in
    # so101_gripper_spec.py was measured off this import, so the two agree.
    # `keep_origin: True` would move the whole body by its bbox centre and
    # invalidate all of them (add_feature reports the shift as
    # `recentred_by`).
    b.feat(type="Import", name="STS3215", path=servo_step)

    for name, mat in (("Chassis", "plastic_black"),
                      ("DrivePlate L", "plastic_black"),
                      ("DrivePlate R", "plastic_black"),
                      ("CouplerPlate", "plastic_black"),
                      ("Finger L", "plastic_black"),
                      ("Finger R", "plastic_black"),
                      ("Jaw Pad", "rubber"),
                      ("Camera Pod", "plastic_black"),
                      ("Camera Module", "plastic_white"),
                      ("STS3215", "plastic_black")):
        d.call("set_active_part", part=name)
        d.call("set_material", body=0, material=mat)
    print("parts built")
    d.close()


if __name__ == "__main__":
    main()
