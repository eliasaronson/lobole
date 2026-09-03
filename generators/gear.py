#!/usr/bin/env python3
"""A spur gear: an involute computed here, a part in the app.

    python3 generators/gear.py --module 2 --teeth 24 --thickness 8 --bore 6
    python3 generators/gear.py --port 8799 ...   # into a specific instance

What this is (docs/capability-review-2026-08-31.md, "scripted features"):
the kind of thing other CADs grow a workbench or a FeatureScript for, done
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


def involute_profile(module, teeth, pressure_deg, samples=5):
    """Closed CCW outline of the whole gear, as sketch entities."""
    m, z = float(module), int(teeth)
    alpha = math.radians(pressure_deg)
    rp = m * z / 2.0          # pitch radius
    rb = rp * math.cos(alpha)  # base radius
    ra = rp + m                # tip (addendum m)
    rf = rp - 1.25 * m         # root (dedendum 1.25 m)
    if rf <= 0:
        raise SystemExit("root radius is not positive: more teeth or a "
                         "smaller module")

    def inv(a):
        return math.tan(a) - a

    half_pitch = math.pi / (2 * z)  # half the tooth's angular thickness at rp

    def psi(r):
        """Half-angle of the tooth at radius r (r >= rb)."""
        return half_pitch + inv(alpha) - inv(math.acos(rb / r))

    def pt(r, theta):
        return (r * math.cos(theta), r * math.sin(theta))

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


def build(d, module, teeth, pressure_deg, thickness, bore, name):
    t0 = time.perf_counter()
    ents, radii = involute_profile(module, teeth, pressure_deg)
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
    if bore > 0:
        skb = check(d.call("add_feature", feature={
            "type": "Sketch", "name": f"{name} bore",
            "plane": plane_at_z(thickness, flip=True),
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
    ap.add_argument("--name", default="Gear")
    a = ap.parse_args()
    if a.teeth < 6:
        raise SystemExit("fewer than 6 teeth is not a gear this draws")
    d = connect(a.port)
    try:
        r = build(d, a.module, a.teeth, a.pressure_angle, a.thickness, a.bore,
                  a.name)
    finally:
        d.close()
    print(f"{a.name}: m={a.module} z={a.teeth} -> pitch dia {2 * r['rp']:.3f}, "
          f"tip dia {2 * r['ra']:.3f}, root dia {2 * r['rf']:.3f}; "
          f"{r['entities']} sketch entities; sketch {r['sketch_s'] * 1e3:.0f} ms, "
          f"extrude {r['extrude_s'] * 1e3:.0f} ms, total {r['total_s'] * 1e3:.0f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
