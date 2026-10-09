#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Place the gripper's parts in an assembly, at a given link angle.

    python3 generators/so101_gripper/so101_gripper_assembly.py --port 8800 [--theta 0] [--create]

theta is the link angle from +Z, positive = closing; params.THETA_CLOSED and
THETA_OPEN are the ends of the stroke. Re-running with a new --theta moves
the existing occurrences, which is how the clash sweep works.
"""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                          # this generator's own files
sys.path.insert(0, os.path.join(HERE, ".."))      # generators/: _lib, gear
import so101_gripper_spec as P                         # noqa: E402
from _lib import connect                               # noqa: E402

ASM = "Gripper"


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qaxis(axis, deg):
    a = math.radians(deg) / 2.0
    n = math.sqrt(sum(c * c for c in axis))
    s = math.sin(a) / n
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(a))


IDENT = (0.0, 0.0, 0.0, 1.0)
# The servo's STEP frame -> the tool frame: its local x (the case's long
# axis, toward the shaft end) becomes tool Z, local y becomes tool X, and
# local z (the shaft) becomes tool Y. That is the cyclic x->z->y->x, a 120
# degree turn about (1,1,1).
Q_SERVO = (-0.5, -0.5, -0.5, 0.5)


def qrot(q, v):
    x, y, z, w = q
    t = (2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]),
         2 * (x * v[1] - y * v[0]))
    return tuple(v[i] + w * t[i] + (y * t[2] - z * t[1] if i == 0 else
                                    z * t[0] - x * t[2] if i == 1 else
                                    x * t[1] - y * t[0]) for i in range(3))


for _v, _want in (((1, 0, 0), (0, 0, 1)), ((0, 1, 0), (1, 0, 0)),
                  ((0, 0, 1), (0, 1, 0))):
    _got = qrot(Q_SERVO, _v)
    assert max(abs(_got[i] - _want[i]) for i in range(3)) < 1e-9, (_v, _got)


def placements(theta_deg):
    """{occurrence name: (position, quaternion)} for one link angle."""
    th = math.radians(theta_deg)
    a2 = (-P.A + P.L * math.sin(th), P.ZA + P.L * math.cos(th))
    out = {
        "Chassis": ((0, 0, 0), IDENT),
        "STS3215": ((-P.A, 0.0, P.ZA - 12.5), Q_SERVO),
        "Camera Pod": ((4.0, 0.0, P.CAM["pod_base_z"]), IDENT),
        "Camera Module": (P.CAM["centre"], qaxis((0, 1, 0), -P.CAM["tilt_deg"])),
    }
    for sy in (1, -1):
        y, yc = sy * P.PLATE_MID, sy * P.COUPLER_MID
        out[f"DrivePlate L{sy:+d}"] = ((-P.A, y, P.ZA),
                                       qaxis((0, 1, 0), theta_deg))
        out[f"DrivePlate R{sy:+d}"] = ((P.A, y, P.ZA),
                                       qaxis((0, 1, 0), -theta_deg))
        out[f"CouplerPlate L{sy:+d}"] = ((-P.A - P.D, yc, P.ZA),
                                         qaxis((0, 1, 0), theta_deg))
        out[f"CouplerPlate R{sy:+d}"] = ((P.A + P.D, yc, P.ZA),
                                         qaxis((0, 1, 0), -theta_deg))
    out["Finger L"] = ((a2[0], 0.0, a2[1]), IDENT)
    out["Finger R"] = ((-a2[0], 0.0, a2[1]), IDENT)
    pad_x = P.PAD_OFFSET - 3.0
    out["Jaw Pad L"] = ((a2[0] + pad_x, 0.0, a2[1]), IDENT)
    out["Jaw Pad R"] = ((-a2[0] - pad_x, 0.0, a2[1]), qaxis((0, 0, 1), 180))
    return out


def part_for(name):
    if name.startswith("DrivePlate"):
        return "DrivePlate R" if name.startswith("DrivePlate R") else \
            "DrivePlate L"
    if name.startswith("CouplerPlate"):
        return "CouplerPlate"
    if name.startswith("Jaw Pad"):
        return "Jaw Pad"
    return name


def create(d, theta):
    d.call("add_assembly", name=ASM)
    d.call("set_active_part", part=ASM)
    for name, (pos, q) in placements(theta).items():
        d.call("add_occurrence", part=part_for(name), name=name,
               transform={"position": list(pos), "orientation": list(q)},
               fixed=(name == "Chassis"))


def move(d, theta):
    d.call("set_active_part", part=ASM)
    by_name = {o["name"]: o["id"] for o in d.call("list_occurrences")["occurrences"]}
    calls = []
    for name, (pos, q) in placements(theta).items():
        calls.append({"method": "set_occurrence_transform",
                      "params": {"id": by_name[name],
                                 "transform": {"position": list(pos),
                                               "orientation": list(q)}}})
    d.call("batch", calls=calls)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--theta", type=float, default=0.0)
    ap.add_argument("--create", action="store_true")
    a = ap.parse_args()
    d = connect(a.port)
    d.sock.settimeout(600)
    if a.create:
        create(d, a.theta)
    else:
        move(d, a.theta)
    print(f"theta {a.theta:+.2f} deg -> opening "
          f"{P.opening_for_theta(a.theta):.1f} mm")
    d.close()


if __name__ == "__main__":
    main()
