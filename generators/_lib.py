# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What every generator needs and every scratch script used to carry.

Seventeen build scripts in scratch/ each copied the same forty lines: a
plane at a height, a plane at an offset, a line, a rect, an arc through
three points, and a check that raises on a failed regeneration. (Its sketch
planes went to the SDK: `lobole.sdk.Plane.xy(z).json()` and friends.) This is
that block, once. Import it from a generator:

    from _lib import connect, line, arc_through, check
    from lobole.sdk import Plane        # Plane.xy(z).json() is a plane at z

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


# --- sketch frames --------------------------------------------------------
# Sketch planes are the SDK's `Plane` (Plane.xy/xz/yz, each spelling out its
# V axis: V = normal x uAxis, the V-axis gotcha).

def uv_on(face, point):
    """Where a world point lands in the frame a primitive standing on `face`
    (an SDK Face) is drawn in: `at=uv_on(face, p)`. That frame's origin is
    the face's centroid and its u the face frame's X, which a script cannot
    guess for a round face, so it is asked rather than assumed."""
    f = face.frame()
    d = [p - o for p, o in zip(point, f.origin)]
    return (sum(a * b for a, b in zip(d, f.x)),
            sum(a * b for a, b in zip(d, f.y)))


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
    every comparison meant restarting an instance. This clears it instead.
    `new_project` does not ask, and the instance may be somebody's working
    session, so a project WITH A PATH and unsaved changes is refused; an
    untitled one is scratch -- nothing on disk names it, and a generator's
    last run left it -- and is discarded, saying so.
    """
    ping = d.call("ping")
    if ping.get("dirty"):
        if ping.get("project"):
            raise SystemExit(
                f"--fresh refused: the project open on port {ping.get('port')} "
                f"({ping['project']}) has unsaved changes. Save it, or run "
                "against a scratch instance "
                "(tools/headless-preview.sh --start <port>).")
        print(f"--fresh: discarded the untitled project on port "
              f"{ping.get('port')} (unsaved, and never saved anywhere)")
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

PRINT_ITEMS = ("bed", "shells", "overhang", "bridges", "clearance", "chords",
               "stacking", "bed_fit")


def _piece(names):
    return " + ".join(names)


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
    elif name == "stacking":
        parts = [f"{_piece(l['piece'])} starts {l['height_mm']:.3f} mm off "
                 "the bed" for l in item["lifted"][:3]]
        pairs = item["pairs"]
        if pairs:
            w = pairs[0]
            parts.append(f"{_piece(w['above'])} over {_piece(w['below'])}: "
                         f"{w['min_air_mm']:.3f} mm of air, "
                         f"{w['area_mm2']:.2f} mm2 under the limit"
                         + (f" (+{len(pairs) - 1} pairs)"
                            if len(pairs) > 1 else ""))
        text = ("; ".join(parts) or f"{item['pieces']} pieces on the bed")
        text += (f" against {item['limit_mm']:g} mm of air "
                 f"({item['limit_from']})")
    elif name == "bed_fit":
        x = lambda v: " x ".join(f"{a:.1f}" for a in v)  # noqa: E731
        text = (f"{x(item['extent_mm'])} mm in {x(item['bed_mm'])} "
                f"({item.get('profile')})"
                + (f", over by {x(item['shortfall_mm'])}"
                   if st == "fail" and any(item["shortfall_mm"]) else "")
                + "".join(
                    f"; {i['body']} "
                    + ("off the bed" if i.get("off_bed") else
                       f"in keep-out zone {i['zone_mm']}")
                    for i in item.get("intrusions", [])[:3])
                ) if "bed_mm" in item else ""
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
    # A part on the built-in table is printed by no printer: its bed_fit has
    # no bed to fit and is skipped, said, and not demanded. With a profile it
    # is demanded, so a profile that states no bed_mm fails here.
    bad = [k for k in need if r[k]["status"] != "pass"
           and not (k == "bed_fit" and "profile" not in r[k])]
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
    out["stacking"]["closest"] = r["stacking"]["pairs"][:3]
    return out


# --- print jobs -----------------------------------------------------------------

def slicer_config(filament):
    """The Bambu Studio project config a job of more than one plate needs
    (docs/tolerances/README.md#print-jobs): `$LOBOLE_SLICER_CONFIGS/
    <filament>.json` (pla, petg, tpu), as tools/bambu-config.py makes it
    from your Bambu Studio's presets. None when there is none: Loböle has no
    slicer presets of its own and does not invent one."""
    d = os.environ.get("LOBOLE_SLICER_CONFIGS")
    if not d:
        return None
    p = os.path.join(d, f"{filament}.json")
    return p if os.path.exists(p) else None


def export_print_job(d, plates, path, filament, name=None):
    """Write one print job (export_print_job): `plates` a list of
    (part, plate name), every piece of each plate its own object with its
    settings. `filament` names the job's one spool, for its slicer config.
    A job of one plate needs no config; one of several without one is NOT
    written, and the reply says why ({"skipped": ...}) -- the receipt
    carries that rather than a job Bambu would read as one plate."""
    cfg = slicer_config(filament)
    if len(plates) > 1 and cfg is None:
        why = (f"{len(plates)} plates need a Bambu project config for "
               f"{filament}: set LOBOLE_SLICER_CONFIGS to a folder of "
               "<filament>.json made by tools/bambu-config.py")
        print(f"  print job {os.path.basename(path)}: skipped, {why}")
        return {"skipped": why, "path": None}
    params = {"path": str(path),
              "plates": [{"part": p, "name": n} for p, n in plates]}
    if cfg is not None:
        params["slicer_config"] = cfg
    if name:
        params["name"] = name
    r = d.call("export_print_job", **params)
    objs = sum(len(p["objects"]) for p in r["plates"])
    print(f"  print job {os.path.basename(path)}: {len(r['plates'])} "
          f"plate(s), {objs} objects, {r['filament']}, {r['form']} form")
    return r


def job_receipt(r):
    """An export_print_job reply as a receipt keeps it: per plate, each
    object's name and what it prints with."""
    if r.get("skipped"):
        return {"skipped": r["skipped"]}
    return {"path": r["path"], "form": r["form"], "filament": r["filament"],
            "profile": r["profile"], "layer_height_mm": r["layer_height_mm"],
            "slicer_config": r.get("slicer_config"),
            "plates": [{"name": p["name"], "part": p["part"],
                        "objects": [{"name": o["name"],
                                     "described": o["described"]}
                                    for o in p["objects"]]}
                       for p in r["plates"]]}


# --- the bed's keep-out zones -------------------------------------------------------

def bed_zones(prof):
    """A print profile's keep-out rectangles [x0, y0, x1, y1] (bed_exclude_mm,
    the bed's own coordinates from its front-left corner): where no piece
    may sit. Empty when the profile states none."""
    return [list(map(float, z)) for z in (prof or {}).get("bed_exclude_mm", [])]


def clear_x(x, y, w, h, zones, gap):
    """The least x' >= x at which a w x h footprint at (x', y) keeps `gap`
    clear of every zone: a shelf packer's next free place along its row."""
    moved = True
    while moved:
        moved = False
        for z in zones:
            if (x < z[2] + gap and x + w > z[0] - gap and
                    y < z[3] + gap and y + h > z[1] - gap):
                x = z[2] + gap
                moved = True
    return x


# --- printed pins across their layers -------------------------------------------

def _run_static(d, sid, timeout_s):
    import time
    run = d.call("run_study", study=sid, **{"async": True})
    t0 = time.monotonic()
    while d.call("study_status", study=sid).get("running"):
        if time.monotonic() - t0 > timeout_s:
            d.call("cancel_study", study=sid)
            raise SystemExit(f"pin_strength: the study ran past "
                             f"{timeout_s:.0f} s")
        time.sleep(0.5)
    st = d.call("study_status", study=sid)
    if st.get("error") or not st.get("has_results"):
        raise SystemExit(f"pin_strength: the study did not solve: {st}")
    return bool(run.get("reused"))


def pin_strength(d, part, face, load_n, direction, seat, layer_normal,
                 region=None, seat_region=None, name=None, mesh_size=None,
                 need=2.0, timeout_s=600.0):
    """How strong a printed pin is ACROSS its layers
    (docs/simulation/README.md#printed-layers): one static study of `part`
    as printed, held at `seat` (a face name, {face, region}, or a list of
    them), loaded with
    `load_n` newtons along `direction` on `face` -- each optionally only on
    part of its face (`region`, `seat_region`: add_load's region,
    docs/simulation/README.md#loads-on-part-of-a-face) -- read with the
    build direction `layer_normal` in the part's frame. The hook's pattern
    (generators/hook/build.py), for a pin: a pin printed standing bends
    across its layers, and the interlayer strength is what breaks it.

    Returns the factor of safety against the interlayer strength, and the
    in-layer one (von Mises against the yield), with the stresses they came
    from, for a receipt. Either under `need` STOPS the run, as the hook's
    prediction would: a generator states its pins' load and must not print
    one that this says breaks -- across its layers, or (a pin printed lying
    down, whose bending runs along them) along them."""
    d.call("set_active_part", part=part)
    st = d.call("add_study", type="static",
                name=name or f"{part}: pin across its layers")
    sid = st.get("id", st.get("study"))
    seats = [seat] if isinstance(seat, (str, dict)) else list(seat)
    for f in seats:
        # A seat is a face name, or {face, region} for part of one.
        face_f, reg = ((f["face"], f.get("region")) if isinstance(f, dict)
                       else (f, seat_region))
        kw = {"region": reg} if reg else {}
        d.call("add_load", study=sid, type="fixed", face=face_f, **kw)
    kw = {"region": region} if region else {}
    d.call("add_load", study=sid, type="force", face=face,
           magnitude=float(load_n), direction=[float(v) for v in direction],
           **kw)
    if mesh_size:
        d.call("set_study", study=sid, mesh_size=float(mesh_size))
    reused = _run_static(d, sid, timeout_s)
    r = d.call("study_results", study=sid,
               layer_normal=[float(v) for v in layer_normal])
    across = r["max_across_layers_mpa"]
    out = {"part": part, "study": sid, "load_n": float(load_n),
           "direction": list(direction), "layer_normal": list(layer_normal),
           "region": region, "seat_region": seat_region,
           "interlayer_factor_of_safety": r["interlayer_factor_of_safety"],
           "max_across_layers_mpa": across["value"],
           "max_across_layers_at_mm": across.get("at"),
           "max_von_mises_mpa": r.get("max_von_mises_mpa"),
           "max_displacement_mm": r.get("max_displacement_mm"),
           "max_principal_mpa": (r.get("max_principal_mpa") or {}).get(
               "value"),
           "factor_of_safety": r.get("factor_of_safety"),
           "nodes": r.get("nodes"), "reused": reused, "need": need}
    f = out["interlayer_factor_of_safety"]
    fy = out["factor_of_safety"]
    print(f"  {part}: {load_n:.1f} N, {across['value']:.2f} MPa across "
          f"its layers, factor {f:.2f} against the interlayer strength, "
          f"{fy:.2f} against the yield")
    if f < need:
        raise SystemExit(
            f"{part}: the pin breaks across its layers: factor {f:.2f} under "
            f"{need:g} at {load_n:.1f} N ({across['value']:.2f} MPa across "
            f"the layers at {across.get('at')})")
    if fy is not None and fy < need:
        raise SystemExit(
            f"{part}: the pin yields: factor {fy:.2f} against the yield "
            f"under {need:g} at {load_n:.1f} N")
    return out
