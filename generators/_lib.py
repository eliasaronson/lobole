# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What every generator needs and every scratch script used to carry.

Seventeen build scripts in scratch/ each copied the same forty lines: a
plane at a height, a plane at an offset, a line, a rect, an arc through
three points, and a check that raises on a failed regeneration. This is
that block, once. Import it from a generator:

    from _lib import connect, plane_at_z, line, arc_through, check

A printed part asks the app whether it will print with `check_print(d, what,
need=...)` rather than scanning its own STL.

`connect()` finds the running app: `--port` if given, else the ONE
announced instance, else it refuses naming the ones it found — a generator
must never guess a port and must never silently attach to the wrong window
(CLAUDE.md, "Don't guess ports").
"""
import math
import os
import sys

from lobole.client import Client  # noqa: E402


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


def uv_on(face, point):
    """Where a world point lands in the frame a primitive standing on `face`
    (an SDK Face) is drawn in: `at=uv_on(face, p)`. That frame's origin is
    the face's centroid and its u the face frame's X, which a script cannot
    guess for a round face, so it is asked rather than assumed."""
    f = face.frame()
    d = [p - o for p, o in zip(point, f.origin)]
    return (sum(a * b for a, b in zip(d, f.x)),
            sum(a * b for a, b in zip(d, f.y)))


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


def circle(center, radius, r_expr=None):
    """A circle entity, optionally with its RADIUS driven by an expression.

    The field is `r_expr` — the app's own name for it (v38). It used to be
    spelled `radius_expr` here, which is not a field of any entity, so it was
    silently dropped and this generator's "live" bore was a literal. The app
    now refuses an expression field an entity does not have.
    """
    e = {"type": "circle", "center": [float(center[0]), float(center[1])],
         "radius": float(radius)}
    if r_expr:
        e["r_expr"] = r_expr
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


def fresh_project(d):
    """Start a new project, for a generator's `--fresh`.

    A generator refuses to build into an instance that already has its tab --
    a rebuild renames the elements the first job's operations point at -- so
    every comparison meant restarting an instance. This clears it instead,
    and REFUSES when the open project has unsaved changes: `new_project` does
    not ask, and the instance may be somebody's working session.
    """
    ping = d.call("ping")
    if ping.get("dirty"):
        raise SystemExit(
            f"--fresh refused: the project open on port {ping.get('port')} "
            f"({ping.get('project') or 'unsaved'}) has unsaved changes. Save "
            "it, or run against a scratch instance "
            "(tools/headless-preview.sh --start <port>).")
    d.call("new_project")


def connect(port=None):
    """The running app, by port or by discovery. Refuses to guess."""
    if port is None:
        found = Client.instances()
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
    return Client.wait_for(int(port))


# --- printing -----------------------------------------------------------------

PRINT_ITEMS = ("bed", "shells", "overhang", "bridges", "clearance", "chords")


def print_item_line(name, item):
    """One check_print item in words: its status, measured value and limit."""
    st = item["status"]
    if name == "bed":
        text = f"lowest point {item['lowest_mm']:.4f} mm"
    elif name == "shells":
        text = (f"{item['pieces']} pieces for {item['expected']} solids"
                + ("" if item["watertight"] else ", not watertight")
                + (f", joined {item['joined']}" if item["joined"] else ""))
    elif name == "overhang":
        text = (f"{item['area_mm2']:.3f} mm2 past {item['limit_deg']:g} deg "
                f"(allowed {item['allowance_mm2']:g}), worst "
                f"{item['worst_deg']:.3f} deg"
                + (f" on {item['worst']['name']} face {item['worst']['face']}"
                   if item.get("worst") else ""))
    elif name == "bridges":
        text = (f"{len(item['faces'])} ceilings, widest "
                f"{item['worst_span_mm']:.2f} mm of {item['limit_mm']:g}")
    elif name == "clearance":
        m = item.get("min_mm")
        near = item["pairs"][0] if item["pairs"] else None
        text = ((f"{m:.4f} mm" if m is not None else "no two bodies near")
                + (f" ({near['a']} / {near['b']})" if near else "")
                + f" against {item['required_mm']:g} mm a side, "
                  f"down to {item['required_mm'] - item['tolerance_mm']:.4f}")
    else:
        text = (f"{item['deflection_mm']:g} mm against "
                f"{item['limit_mm']:g} mm")
    if st == "skipped":
        text = item.get("note", "skipped")
    return f"{name} {st}: {text}"


def check_print(d, what, need=PRINT_ITEMS, quiet=False, **params):
    """Will this part print? The app's one check (check_print,
    docs/tolerances/README.md#checking-a-print) over the active part, or
    `part=...`. Raises naming each item in `need` that did not pass -- a
    skipped one too: what a generator needs checked must be checked. Prints
    every item unless `quiet`, and returns the reply, whose items go in the
    receipt (`print_receipt`)."""
    r = d.call("check_print", **params)
    if not quiet:
        for k in PRINT_ITEMS:
            print(f"  {what}: {print_item_line(k, r[k])}")
    bad = [k for k in need if r[k]["status"] != "pass"]
    if bad:
        raise SystemExit(f"{what}: will not print: "
                         + "; ".join(print_item_line(k, r[k]) for k in bad))
    return r


def print_receipt(r):
    """A check_print reply, as a receipt keeps it: each item's status,
    measured value and limit, without the per-face lists."""
    out = {"ok": r["ok"], "process": r["process"],
           "direction": r["direction"]}
    for k in PRINT_ITEMS:
        out[k] = {key: v for key, v in r[k].items()
                  if key not in ("faces", "pairs", "note", "source")}
    out["clearance"]["closest"] = r["clearance"]["pairs"][:3]
    return out
