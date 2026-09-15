"""The half-dozen moves a bicycle is made of.

A bicycle is tubes that meet at angles no axis is square to, and rims and
tyres that are profiles turned about an axle. Everything here exists because
the alternative — a plane spelled out at every call site — is where the sign
errors live (docs/sketching: the V-axis gotcha).

`tube(part, a, b, od, wall)` is the one that matters: a pipe from one world
point to another, whatever direction they lie in, hollow if `wall` is given
and solid if it is not. It builds its own plane: u along the run, and the
normal any perpendicular, because a circle swept along a line does not care
which way round the section is.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from designer_sdk import Plane, SdkError, V, _id  # noqa: E402

# There is no paint, chrome or tyre-rubber material — these are appearance
# overrides on real slugs, so the mass still comes from the slug.
LOOK = {
    "green": dict(color=[0.035, 0.21, 0.115], metallic=0.35, roughness=0.32),
    "chrome": dict(color=[0.92, 0.93, 0.94], metallic=1.0, roughness=0.09),
    "alloy": dict(color=[0.80, 0.81, 0.82], metallic=1.0, roughness=0.30),
    "dull_alloy": dict(color=[0.72, 0.73, 0.74], metallic=1.0, roughness=0.45),
    "tyre": dict(color=[0.055, 0.055, 0.06], metallic=0.0, roughness=0.85),
    "black": dict(color=[0.08, 0.08, 0.09], metallic=0.0, roughness=0.45),
    "saddle": dict(color=[0.06, 0.06, 0.07], metallic=0.0, roughness=0.55),
    "white": dict(color=[0.88, 0.87, 0.84], metallic=0.0, roughness=0.50),
    "red": dict(color=[0.55, 0.03, 0.04], metallic=0.0, roughness=0.25),
    "orange": dict(color=[0.85, 0.35, 0.02], metallic=0.0, roughness=0.30),
    "oiled": dict(color=[0.22, 0.20, 0.18], metallic=0.9, roughness=0.55),
    "brass": dict(color=[0.80, 0.66, 0.32], metallic=1.0, roughness=0.22),
}


def look(part, name, body=None):
    """An appearance over the whole part, or over one body."""
    part.activate()
    n = len(part.bodies())
    for i in ([body] if body is not None else range(n)):
        part.app.call("set_appearance", body=i, **LOOK[name])
    return part


def finish(part, slug, appearance, body=None):
    """The two that always go together: what it is MADE of, and what it looks
    like. Mass comes from the slug; only the colour comes from the look."""
    part.material(slug) if body is None else part.material(slug, body=body)
    return look(part, appearance, body=body)


# --- tubes --------------------------------------------------------------------

def _frame_of(a, b, bulge=None):
    """A plane containing the run a->b, with V pointing along `bulge` if one
    is given (so an arc in this plane bows the way the caller meant)."""
    u = (V(b) - V(a)).unit()
    if bulge is not None:
        v = V(bulge) - u * u.dot(V(bulge))
        if v.length() < 1e-6:
            raise ValueError("bulge is parallel to the run")
        v = v.unit()
    else:
        ref = V(0, 1, 0) if abs(u.dot(V(0, 1, 0))) < 0.9 else V(1, 0, 0)
        v = u.cross(ref).unit()
    return Plane(V(a), u.cross(v), u)


def path_tube(part, sketch_id, od, wall=0.0, name="Tube", mode="new_body"):
    f = {"type": "Pipe", "name": name, "path_id": _id(sketch_id),
         "outer_diameter": float(od), "wall_thickness": float(wall),
         "mode": mode}
    return part._add(f)["id"]


def _weld_plugs(part, ids, a, b, u, od, weld, name):
    """A short SOLID plug in each welded end of a hollow tube.

    A hollow tube whose end cap lands inside another hollow tube's void
    unions correctly at most offsets and produces an invalid solid at a few
    — measured here, a seat stay at 11 mm off the seat tube's axis failed
    while 10 and 12 passed, which is a tangency between two bore surfaces and
    not something to find by nudging the number until it builds. A frame tube
    is mitred and brazed anyway: the last 25 mm of it is full of lug and
    filler, not air. Plugging it is both the true shape and the robust
    boolean, and it costs about 8 g a joint.

    The plug is a HAIR narrower than the tube. At the same diameter its
    lateral face is coincident with the tube's over the whole plug, and the
    fused body — valid in itself — then failed EVERY later boolean it was an
    operand of: the down tube unioned into the bottom-bracket shell came back
    "empty or invalid solid" at every socket depth tried. 0.6 mm off the
    diameter still leaves the plug biting into the wall, and removes the
    coincidence.
    """
    plug_od = float(od) - 0.6
    la, lb = (weld, weld) if isinstance(weld, (int, float)) else weld
    for end, at, length in (("a", V(a), la), ("b", V(b), lb)):
        if not length:
            continue
        step = u * float(length) if end == "a" else u * -float(length)
        ids.append(tube(part, at, tuple(at + step), plug_od, 0.0,
                        f"{name} weld {end}"))
    return ids


def tube(part, a, b, od, wall=0.0, name="Tube", weld=0.0):
    """A straight pipe between two WORLD points. Returns its feature id."""
    return bowed_tube(part, a, b, None, 0.0, od, wall, name, weld)


def bowed_tube(part, a, b, bulge, sagitta, od, wall=0.0, name="Tube",
               weld=0.0):
    """A pipe that bows `sagitta` toward `bulge` at its middle — a fork blade,
    a fender stay, a rack leg. Straight if the sagitta is zero.

    `weld` plugs the ends solid for that length (see `_weld_plugs`) — one
    number for both, or `(a, b)` for each. It is NOT a default: a plugged
    end is a blocked bore, and the seat tube has to take a seatpost and the
    head tube a steerer. Plug the end that is brazed into a joint, and only
    that one.
    """
    a, b = V(a), V(b)
    L = (b - a).length()
    if L < 1e-6:
        raise ValueError(f"{name}: zero-length tube")
    if abs(sagitta) < 1e-6:
        sk = part.sketch(_frame_of(a, b), name=f"{name} path")
        sk.line((0, 0), (L, 0))
    else:
        sk = part.sketch(_frame_of(a, b, bulge), name=f"{name} path")
        sk.arc_through((0, 0), (L / 2.0, float(sagitta)), (L, 0))
    ids = [path_tube(part, sk.add(), od, wall, name)]
    if wall and (weld if isinstance(weld, (int, float)) else any(weld)):
        _weld_plugs(part, ids, a, b, (b - a).unit(), od, weld, name)
        return union(part, ids, f"{name} weld")
    return ids[0]


def arc_tube(part, center, axis, radius, from_deg, to_deg, od, wall=0.0,
             ref=None, name="Arc tube", steps=None):
    """A pipe following an arc about `center` in the plane normal to `axis` —
    a fender, a rack hoop, a handlebar's bend. `ref` is the direction the
    arc's zero angle points (default: any perpendicular to the axis)."""
    n = V(axis).unit()
    if ref is None:
        r0 = V(0, 0, 1) if abs(n.dot(V(0, 0, 1))) < 0.9 else V(1, 0, 0)
        u = (r0 - n * n.dot(r0)).unit()
    else:
        u = (V(ref) - n * n.dot(V(ref))).unit()
    w = n.cross(u)
    sk = part.sketch(Plane(V(center), n, u), name=f"{name} path")
    sk.arc((0, 0), float(radius), float(from_deg), float(to_deg))
    del w
    return path_tube(part, sk.add(), od, wall, name)


def polypath(part, points, od, wall=0.0, name="Path tube"):
    """A pipe along a chain of WORLD points. The corners come out rounded —
    a swept spine rounds them (SweepFeature's RoundCorner), which is exactly
    what a bent handlebar or a fender stay does."""
    pts = [V(p) for p in points]
    u = (pts[1] - pts[0]).unit()
    # A plane that contains the whole chain, if one does; the caller is
    # responsible for that, and gets a REFUSAL rather than a silently
    # flattened bar if it does not.
    nrm = None
    for i in range(1, len(pts) - 1):
        c = (pts[i] - pts[i - 1]).cross(pts[i + 1] - pts[i])
        if c.length() > 1e-6:
            nrm = c.unit()
            break
    if nrm is None:
        return tube(part, pts[0], pts[-1], od, wall, name)
    for p in pts:
        if abs((p - pts[0]).dot(nrm)) > 1e-6:
            raise ValueError(f"{name}: the points are not coplanar — "
                             "build it as several tubes")
    u = (u - nrm * nrm.dot(u)).unit()
    plane = Plane(pts[0], nrm, u)
    sk = part.sketch(plane, name=f"{name} path")
    uv = [plane.uv(p) for p in pts]
    for i in range(len(uv) - 1):
        sk.line(uv[i], uv[i + 1])
    return path_tube(part, sk.add(), od, wall, name)


# --- solids -------------------------------------------------------------------

def rod(part, a, b, d, name="Rod"):
    return tube(part, a, b, d, 0.0, name)


def disc(part, center, axis, d, t, name="Disc"):
    """A disc of thickness `t` CENTRED on `center`, normal to `axis`.

    Returns the feature id, like everything else here — `part.cylinder`
    returns the Body, which costs a body query per call and is the wrong
    currency when the next move is a union.
    """
    n = V(axis).unit()
    base = V(center) - n * (float(t) / 2.0)
    return part._add({"type": "Cylinder", "name": name, "center": list(base),
                      "direction": list(n), "radius": float(d) / 2.0,
                      "height": float(t)})["id"]


AXES = {(1, 0, 0): "x", (0, 1, 0): "y", (0, 0, 1): "z"}


def revolved(part, profile, center, axis, name="Revolved", ref=None,
             angle_deg=360.0):
    """A closed profile, given in (radius, along-axis) pairs, revolved into a
    solid of revolution — a rim, a tyre, a hub shell.

    The profile's first coordinate is the DISTANCE FROM THE AXIS and the
    second is the distance along it, which is how a turned part is dimensioned
    on a drawing and not how a sketch plane is spelled.

    `angle_deg` short of a full turn sweeps from the profile's own plane, so
    `ref` is where the sweep STARTS — which is how a mudguard says where it
    begins and how far round it goes.

    A Revolve turns about one of the PART's own axes (or a sketch line, or a
    datum axis) — there is no arbitrary axis — so a part that has to be turned
    about something else is built on its own axis and mated, which is what
    every wheel, gear and hub here does. Anything else is refused rather than
    quietly turned about the wrong line.
    """
    n = V(axis).unit()
    key = tuple(int(round(c)) for c in n)
    if key not in AXES or V(center).length() > 1e-9:
        raise ValueError(
            f"{name}: revolve takes one of the part's own axes through its "
            f"origin (x, y or z), not {tuple(round(c, 3) for c in n)} at "
            f"{tuple(center)} — build the part on its axis and mate it")
    r0 = V(0, 0, 1) if abs(n.dot(V(0, 0, 1))) < 0.9 else V(1, 0, 0)
    u = (V(ref) if ref is not None else r0)
    u = (u - n * n.dot(u)).unit()
    # The sketch plane contains the axis: U is the radial direction and V is
    # the axis, so the profile's (r, z) pairs go in unchanged.
    plane = Plane(V(center), u.cross(n), u)
    sk = part.sketch(plane, name=f"{name} profile")
    sk.polyline([(float(r), float(z)) for r, z in profile], close=True)
    f = {"type": "Revolve", "name": name, "sketch_id": _id(sk.add()),
         "mode": "new_body", "angle_deg": float(angle_deg),
         "axis": {"kind": AXES[key]}}
    return part._add(f)["id"]


# --- putting them together --------------------------------------------------

def union(part, ids, name="Weld"):
    """Fuse a list of feature-made bodies into one. Returns the last id."""
    ids = [i for i in ids if i]
    target = ids[0]
    for k, tool in enumerate(ids[1:], 1):
        target = part._add({"type": "Boolean", "name": f"{name} {k}",
                            "target_id": target, "tool_id": tool,
                            "op": "union"})["id"]
    return target


def cut(part, target, tool, name="Cut"):
    return part._add({"type": "Boolean", "name": name, "target_id": target,
                      "tool_id": tool, "op": "subtract"})["id"]


class Weldment:
    """Build a frame as SOLID tubes, weld them, then drill their bores.

    Three constructions were tried for a mitred tube joint and this is the
    one that always builds:

    1. Hollow pipes, unioned. A tube whose end cap lands inside another
       tube's VOID is a cap between two surfaces, and the union is a
       tangency lottery — a seat stay 11 mm off the seat tube's axis failed
       while 10 and 12 passed.
    2. Hollow pipes with a solid plug plugging each welded end. Better, but
       the plug's own lateral face leaves a sliver inside the wall, and that
       body then fails the NEXT boolean it is an operand of: a chainstay at
       a dropout spacing of 65.5 mm failed at a bow of 7 and 11 mm while 5
       and 9 passed. Nothing about a bicycle should depend on that.
    3. This. Solid rods union at any angle — there is no void for a cap to
       land in — and a bore drilled afterwards is a cylinder subtracted from
       a big solid, which is the easiest thing a kernel does. The tubes come
       out hollow between their joints and SOLID through them, which is what
       a brazed lug is, and the mass is right: 3.5 kg of frame.

    Usage: `w.tube(...)` for every tube, `w.solid(id)` for anything already
    made, `w.weld()` for the body.
    """

    def __init__(self, part, name="Weld"):
        self.part, self.name = part, name
        self.solids, self.bores = [], []

    def solid(self, feature_id):
        self.solids.append(feature_id)
        return feature_id

    def tube(self, a, b, od, wall=0.0, name="Tube", weld=(25.0, 25.0),
             bulge=None, sagitta=0.0):
        """A solid tube now, and a bore to drill later.

        `weld` is how much of each end stays SOLID — the length that is lug
        and filler rather than air. An end that has to stay open (a seat tube
        for its post, a head tube for its steerer, a shell for its spindle)
        takes 0, and then the bore runs out past that end.
        """
        a, b = V(a), V(b)
        u = (b - a).unit()
        length = (b - a).length()
        self.solids.append(bowed_tube(self.part, a, b, bulge, sagitta, od,
                                      0.0, name))
        if wall <= 0:
            return self.solids[-1]
        la, lb = (weld, weld) if isinstance(weld, (int, float)) else weld
        ta, tb = float(la) / length, 1.0 - float(lb) / length
        if tb <= ta:
            return self.solids[-1]        # welded end to end: no bore at all
        if sagitta:
            start = arc_point(a, b, bulge, sagitta, ta)
            end = arc_point(a, b, bulge, sagitta, tb)
            sag = float(sagitta) * (tb - ta) ** 2
        else:
            start = tuple(a + u * float(la))
            end = tuple(b - u * float(lb))
            sag = 0.0
        self.bores.append((start, end, float(od) - 2 * float(wall), sag,
                           bulge, f"{name} bore"))
        return self.solids[-1]

    def weld(self):
        body = union(self.part, self.solids, self.name)
        for start, end, bore_d, sag, bulge, name in self.bores:
            tool = bowed_tube(self.part, start, end, bulge, sag, bore_d, 0.0,
                              name)
            body = cut(self.part, body, tool, name)
        return body


def arc_point(a, b, bulge, sagitta, t):
    """A point on a bowed tube's axis, `t` of the way along it."""
    a, b = V(a), V(b)
    u = (b - a).unit()
    v = V(bulge) - u * u.dot(V(bulge)) if bulge is not None else V(0, 0, 0)
    v = v.unit() if v.length() > 1e-9 else V(0, 0, 0)
    return tuple(a + (b - a) * float(t)
                 + v * (4.0 * float(sagitta) * t * (1.0 - t)))


def axle_frame(part, name, origin, axis, x=None):
    """A frame on an axle: local Z along the axle, so a revolute mate to it
    turns about the axle and nothing else."""
    z = V(axis).unit()
    if x is None:
        r = V(1, 0, 0) if abs(z.dot(V(1, 0, 0))) < 0.9 else V(0, 0, 1)
        x = (r - z * z.dot(r)).unit()
    return part.frame(name, origin=V(origin), z=list(z), x=list(V(x).unit()))


def seat_frame(part, name="seat"):
    """The frame every part that does not move carries: the world origin,
    world axes. A part drawn in world coordinates fastens by this and lands
    exactly where it was drawn."""
    return part.frame(name, origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))


def verified(part, solids=1):
    """A part is finished when it BUILT, has the solids it should, and is a
    valid B-Rep — checked on `check_geometry`'s receipt, not on its `ok`.

    The third of those is not redundant, and this is the round that proves
    it. The frame passed "no feature errored" and "one solid" with a body
    OpenCASCADE calls badly nested, and an invalid body poisons every
    question asked of it afterwards: the assembly's interference sweep
    reported 17,297 mm3 of the rear cog buried in a frame that does not
    reach it, and the hunt for that phantom clash cost more than the whole
    rear triangle did to build.
    """
    part.check(solids=solids)
    part.activate()
    receipt = part.app.call("check_geometry")
    standing = receipt["receipt"]["standing"]
    if standing != "verified":
        bad = [f"{b.get('name')}: {', '.join(b.get('reasons', []))}"
               for b in receipt.get("bodies", []) if not b.get("valid", True)]
        raise SdkError(f"{part.name}: {standing} — "
                       f"{receipt['receipt']['summary']}"
                       + (f" ({'; '.join(bad)})" if bad else ""))
    return part


def deg(a):
    return math.degrees(a)


# --- the two outlines a chain drive needs ------------------------------------

def sprocket_outline(teeth, pitch, roller_d, tip=3.5, flank=0.13, root=0.35):
    """A sprocket's outline as a closed point list, in its own plane.

    Four points a tooth: up the flank to the tip, across, and back down to
    the root circle, which sits a roller's radius inside the pitch circle.
    It is not an ISO 606 profile and does not claim to be — it is the shape
    that makes a 46-tooth ring read as a chainring and a chain sit on it.
    """
    z = int(teeth)
    pitch_r = float(pitch) / (2 * math.sin(math.pi / z))
    root_r = pitch_r - float(roller_d) / 2.0
    tip_r = pitch_r + float(tip)
    step = 2 * math.pi / z
    pts = []
    for i in range(z):
        a = i * step
        for r, off in ((root_r, -root), (tip_r, -flank),
                       (tip_r, +flank), (root_r, +root)):
            t = a + off * step
            pts.append((r * math.cos(t), r * math.sin(t)))
    return pts


def tangent_hull(c1, r1, c2, r2, steps=48):
    """The outline that wraps two circles: the two outer tangents and the arc
    at each end — a chain's path, and a teardrop chainguard's edge.

    Both are the same shape, which is not a coincidence: the guard is drawn
    round the chain.
    """
    dx, dy = c2[0] - c1[0], c2[1] - c1[1]
    span = math.hypot(dx, dy)
    if span <= abs(r1 - r2):
        raise ValueError("tangent_hull: one circle contains the other")
    base = math.atan2(dy, dx)
    # The tangent touches this far round from the centre line on both circles
    # — the same angle, which is what makes the tangent common to them.
    off = math.acos((r1 - r2) / span)
    pts = []
    n = max(6, int(steps))
    for k in range(n + 1):                      # round the far side of c1
        a = base + off + (2 * math.pi - 2 * off) * k / n
        pts.append((c1[0] + r1 * math.cos(a), c1[1] + r1 * math.sin(a)))
    for k in range(n + 1):                      # and back round c2
        a = base - off + (2 * off) * k / n
        pts.append((c2[0] + r2 * math.cos(a), c2[1] + r2 * math.sin(a)))
    return pts
