"""What every generator needs and every scratch script used to carry.

Seventeen build scripts in scratch/ each copied the same forty lines: a
plane at a height, a plane at an offset, a line, a rect, an arc through
three points, and a check that raises on a failed regeneration. This is
that block, once. Import it from a generator:

    from _lib import connect, plane_at_z, line, arc_through, check

`connect()` finds the running app: `--port` if given, else the ONE
announced instance, else it refuses naming the ones it found — a generator
must never guess a port and must never silently attach to the wrong window
(CLAUDE.md, "Don't guess ports").
"""
import math
import os
import sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
from designer_remote import Designer  # noqa: E402


# --- sketch planes --------------------------------------------------------
# In-plane V = normal x uAxis (docs/sketching: the V-axis gotcha), which is
# why each helper spells the axis out instead of leaving it to be guessed.

def plane_at_z(z, flip=False):
    """XY plane at height z. flip: normal -Z (V = -Y), for cutting downward."""
    return {"origin": [0, 0, z], "normal": [0, 0, -1 if flip else 1],
            "uAxis": [1, 0, 0]}


def plane_at_x(x):
    """YZ plane at x; u = world Y, v = world Z."""
    return {"origin": [x, 0, 0], "normal": [1, 0, 0], "uAxis": [0, 1, 0]}


def plane_at_y(y):
    """XZ plane at y with normal -Y, so v = world Z (not -Z)."""
    return {"origin": [0, y, 0], "normal": [0, -1, 0], "uAxis": [1, 0, 0]}


# --- entities ---------------------------------------------------------------

def line(a, b):
    return {"type": "line", "a": [float(a[0]), float(a[1])],
            "b": [float(b[0]), float(b[1])]}


def rect(c1, c2):
    return {"type": "rect", "c1": [float(c1[0]), float(c1[1])],
            "c2": [float(c2[0]), float(c2[1])]}


def circle(center, radius, radius_expr=None):
    e = {"type": "circle", "center": [float(center[0]), float(center[1])],
         "radius": float(radius)}
    if radius_expr:
        e["radius_expr"] = radius_expr
    return e


def circumcircle(p1, p2, p3):
    ax, ay = p1
    bx, by = p2
    cx, cy = p3
    d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    if abs(d) < 1e-12:
        raise ValueError("arc_through: the three points are collinear")
    ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay)
          + (cx * cx + cy * cy) * (ay - by)) / d
    uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx)
          + (cx * cx + cy * cy) * (bx - ax)) / d
    return (ux, uy), math.hypot(ax - ux, ay - uy)


def arc_through(p_start, p_mid, p_end):
    """Arc entity from p_start to p_end passing through p_mid.

    The arc entity is counter-clockwise from start to end; if the CCW sweep
    from start would not pass through mid, the ends are swapped so it does.
    """
    c, r = circumcircle(p_start, p_mid, p_end)
    a = [math.atan2(p[1] - c[1], p[0] - c[0]) for p in (p_start, p_mid, p_end)]
    tau = 2 * math.pi
    if (a[1] - a[0]) % tau < (a[2] - a[0]) % tau:
        start, end = a[0], a[2]
    else:
        start, end = a[2], a[0]
    return {"type": "arc", "center": [c[0], c[1]], "radius": r,
            "start_angle": start, "end_angle": end}


# --- the app ------------------------------------------------------------------

def check(r, what):
    """Raise on a failed regeneration; print its warnings. Returns r."""
    regen = r.get("regen", {}) if isinstance(r, dict) else {}
    if regen and not regen.get("ok", True):
        raise SystemExit(f"REGEN FAILED after {what}: {regen}")
    for w in regen.get("warnings", []) if regen else []:
        print(f"  warning after {what}: {w}")
    return r


def connect(port=None):
    """The running app, by port or by discovery. Refuses to guess."""
    if port is None:
        found = Designer.instances()
        if len(found) == 1:
            port = found[0]["port"]
        elif not found:
            raise SystemExit("no running Designer instance announced itself; "
                             "start one (tools/headless-preview.sh <port>, or "
                             "the app) or pass --port")
        else:
            raise SystemExit("several instances are running, pass --port: "
                             + ", ".join(f"{i['port']} ({i.get('project') or 'no project'})"
                                         for i in found))
    return Designer.wait_for(int(port))
