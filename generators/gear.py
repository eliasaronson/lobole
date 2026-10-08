#!/usr/bin/env python3
"""A spur gear: an involute computed here, a part in the app.

    python3 generators/gear.py --module 2 --teeth 24 --thickness 8 --bore 6
    python3 generators/gear.py --port 8799 ...   # into a specific instance

What this is (docs/roadmap.md#not-planned, "an interpreter inside regeneration"):
the kind of thing other CADs grow a workbench or a feature language for, done
as an ordinary script against the API. The result is an ordinary part:
thickness and bore stay LIVE as project variables driving expressions; the
tooth profile is computed from module, teeth and pressure angle and is
frozen — change those and run again.

The profile: each flank is the involute of the base circle, sampled at five
radii and drawn as two arcs through three points (the usual approximation;
well inside a hobbed gear's tolerance at these sizes). Tip and root are
arcs on their circles. A root fillet is not drawn. Undercut is not
corrected: below ~17 teeth at 20 degrees the flank simply runs radially
from the base circle down to the root, as an uncorrected cutter would.
"""
import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _lib import arc_through, check, circle, connect, line, plane_at_z  # noqa


def involute_profile(module, teeth, pressure_deg, samples=5, backlash=0.0,
                     phase_deg=0.0, addendum=1.0, dedendum=1.25):
    """Closed CCW outline of the whole gear, as sketch entities.

    `backlash` (mm, measured on the pitch circle) THINS every tooth by half
    of it per flank. A nominal involute pair at nominal centres has none, so
    two printed gears generated from this function used to be a press fit of
    tooth into space; the number to want is about 0.1-0.2 mm at these sizes.

    `phase_deg` turns the whole pattern. It is what makes a pair MESH: two
    gears whose teeth both point at the line of centres jam tip to tip, and
    the fix is a quarter pitch on each (or half a pitch on one).

    `addendum` and `dedendum` are in modules. An INTERNAL gear is cut by
    this outline with them swapped (its tooth spaces are this gear's teeth):
    a cutter's addendum is the internal gear's root depth, and a NEGATIVE
    backlash widens the spaces it cuts.
    """
    m, z = float(module), int(teeth)
    alpha = math.radians(pressure_deg)
    rp = m * z / 2.0          # pitch radius
    rb = rp * math.cos(alpha)  # base radius
    ra = rp + addendum * m     # tip (addendum m by default)
    rf = rp - dedendum * m     # root (dedendum 1.25 m by default)
    if rf <= 0:
        raise SystemExit("root radius is not positive: more teeth or a "
                         "smaller module")

    def inv(a):
        return math.tan(a) - a

    # Half the tooth's angular thickness at rp, less half the backlash.
    half_pitch = math.pi / (2 * z) - float(backlash) / (2.0 * rp)

    def psi(r):
        """Half-angle of the tooth at radius r (r >= rb)."""
        return half_pitch + inv(alpha) - inv(math.acos(rb / r))

    if psi(ra) <= 0:
        raise SystemExit(f"the tooth comes to a point below its tip "
                         f"(z={z}, addendum {addendum} m): a smaller addendum "
                         f"or less backlash")

    phase = math.radians(float(phase_deg))

    def pt(r, theta):
        return (r * math.cos(theta + phase), r * math.sin(theta + phase))

    r_lo = max(rf, rb)
    radii = [r_lo + (ra - r_lo) * i / (samples - 1) for i in range(samples)]
    ents = []
    starts = []  # each tooth's first point, for the root arcs
    ends = []    # each tooth's last point
    for k in range(z):
        c = 2 * math.pi * k / z
        # Right flank (clockwise side), root to tip, at angles c - psi(r).
        right = [pt(r, c - psi(r)) for r in radii]
        left = [pt(r, c + psi(r)) for r in reversed(radii)]  # tip to root
        first = pt(rf, c - psi(r_lo)) if rf < rb else right[0]
        last = pt(rf, c + psi(r_lo)) if rf < rb else left[-1]
        if rf < rb:
            ents.append(line(first, right[0]))
        ents.append(arc_through(right[0], right[1], right[2]))
        ents.append(arc_through(right[2], right[3], right[4]))
        # Tip arc, through the tooth's centre line at ra.
        ents.append(arc_through(right[4], pt(ra, c), left[0]))
        ents.append(arc_through(left[0], left[1], left[2]))
        ents.append(arc_through(left[2], left[3], left[4]))
        if rf < rb:
            ents.append(line(left[-1], last))
        starts.append(first)
        ends.append(last)
    # Root arcs between consecutive teeth, through the gap's centre at rf.
    for k in range(z):
        c_mid = 2 * math.pi * (k + 0.5) / z
        ents.append(arc_through(ends[k], pt(rf, c_mid), starts[(k + 1) % z]))
    return ents, {"rp": rp, "rb": rb, "ra": ra, "rf": rf}


def wedge(d, radius, half_deg, thickness, name):
    """A pie about +X, as a body — what a SECTOR gear keeps."""
    n = 12
    a = math.radians(half_deg)
    r = radius / math.cos(a / n)  # circumscribe, so no tooth is clipped
    pts = [(0.0, 0.0)] + [(r * math.cos(-a + 2 * a * i / n),
                           r * math.sin(-a + 2 * a * i / n))
                          for i in range(n + 1)]
    ents = [line(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
    ents.append(line(pts[-1], pts[0]))
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": f"{name} wedge", "plane": plane_at_z(0),
        "entities": ents}), "wedge sketch")
    return check(d.call("add_feature", feature={
        "type": "Extrude", "name": f"{name} wedge", "sketch_id": sk["id"],
        "distance": thickness, "mode": "new_body"}), "wedge")["id"]


def build(d, module, teeth, pressure_deg, thickness, bore, name,
          backlash=0.0, phase_deg=0.0, sector_deg=0.0):
    t0 = time.perf_counter()
    ents, radii = involute_profile(module, teeth, pressure_deg,
                                   backlash=backlash, phase_deg=phase_deg)
    # The live half: what a user adjusts after the fact drives expressions.
    for var, value, comment in (("gear_thickness", thickness, "face width, mm"),
                                ("gear_bore", bore, "bore diameter, mm")):
        d.call("set_variable", name=var, expression=str(value), comment=comment)
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": f"{name} profile", "plane": plane_at_z(0),
        "entities": ents}), "profile sketch")
    t_sketch = time.perf_counter()
    body = check(d.call("add_feature", feature={
        "type": "Extrude", "name": name, "sketch_id": sk["id"],
        "distance": thickness, "distance_expr": "gear_thickness",
        "mode": "new_body"}), "extrude")
    t_extrude = time.perf_counter()
    if sector_deg > 0:
        # Teeth only where they are needed: keep the wedge, and a hub disc
        # out to the root circle so the sector stays one body. A sector gear
        # is what a lever, a quadrant or a gripper's drive plate carries --
        # a full gear there is material in the way of the next part.
        keep = wedge(d, radii["ra"] + 1.0, sector_deg, thickness, name)
        hub_sk = check(d.call("add_feature", feature={
            "type": "Sketch", "name": f"{name} hub", "plane": plane_at_z(0),
            "entities": [circle((0, 0), radii["rf"])]}), "hub sketch")
        hub = check(d.call("add_feature", feature={
            "type": "Extrude", "name": f"{name} hub", "sketch_id": hub_sk["id"],
            "distance": thickness, "mode": "new_body"}), "hub")["id"]
        sector = check(d.call("add_feature", feature={
            "type": "Boolean", "name": f"{name} sector", "target_id": body["id"],
            "tool_id": keep, "op": "intersect"}), "sector")
        body = check(d.call("add_feature", feature={
            "type": "Boolean", "name": name, "target_id": sector["id"],
            "tool_id": hub, "op": "union"}), "sector+hub")
    if bore > 0:
        skb = check(d.call("add_feature", feature={
            "type": "Sketch", "name": f"{name} bore",
            "plane": plane_at_z(thickness, flip=True),
            # The app's expression field for a circle's radius is `r_expr`
            # (v38). This said `radius_expr`, which is not a field of any
            # entity, so it was DROPPED — and the bore this file advertises
            # as a live variable was the literal it happened to start at
            # (docs/variables/README.md#concepts; the app now refuses an
            # expression field the entity does not have).
            "entities": [circle((0, 0), bore / 2.0, "gear_bore / 2")]}),
            "bore sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": f"{name} bore cut",
            "sketch_id": skb["id"], "distance": thickness,
            "end_mode": "through_all", "mode": "cut"}), "bore cut")
    t_done = time.perf_counter()
    return {"feature_id": body["id"], "entities": len(ents), **radii,
            "sketch_s": t_sketch - t0, "extrude_s": t_extrude - t_sketch,
            "total_s": t_done - t0}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--module", type=float, default=2.0, help="mm")
    ap.add_argument("--teeth", type=int, default=24)
    ap.add_argument("--pressure-angle", type=float, default=20.0, help="deg")
    ap.add_argument("--thickness", type=float, default=8.0, help="mm")
    ap.add_argument("--bore", type=float, default=6.0, help="mm, 0 = none")
    ap.add_argument("--backlash", type=float, default=0.0,
                    help="mm on the pitch circle; a printed pair wants ~0.15")
    ap.add_argument("--phase", type=float, default=0.0,
                    help="deg to turn the tooth pattern — a quarter pitch on "
                         "each of two mirrored sectors is what makes them mesh")
    ap.add_argument("--sector", type=float, default=0.0,
                    help="deg half-angle about +X: teeth only there, hub "
                         "elsewhere (0 = a whole gear)")
    ap.add_argument("--name", default="Gear")
    a = ap.parse_args()
    if a.teeth < 6:
        raise SystemExit("fewer than 6 teeth is not a gear this draws")
    d = connect(a.port)
    try:
        r = build(d, a.module, a.teeth, a.pressure_angle, a.thickness, a.bore,
                  a.name, backlash=a.backlash, phase_deg=a.phase,
                  sector_deg=a.sector)
    finally:
        d.close()
    print(f"{a.name}: m={a.module} z={a.teeth} -> pitch dia {2 * r['rp']:.3f}, "
          f"tip dia {2 * r['ra']:.3f}, root dia {2 * r['rf']:.3f}; "
          f"{r['entities']} sketch entities; sketch {r['sketch_s'] * 1e3:.0f} ms, "
          f"extrude {r['extrude_s'] * 1e3:.0f} ms, total {r['total_s'] * 1e3:.0f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
