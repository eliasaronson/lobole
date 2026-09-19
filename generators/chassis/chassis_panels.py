"""The flat parts: the panels a frame is skinned with, and the tabs welded
to its tubes.

Both are the same move — a flat blank, cut from plate, that has to fit round
a round tube — and both leave as a DXF. A tab differs from a panel only in
that its edge is SADDLED to the tube it is welded to, so it sits down on the
tube instead of bridging it, which is the difference between a weld with a
root and a weld with a gap.

What the saddle looks like depends on how the plate meets the tube, and the
two cases are not the same part:

- a plate ACROSS the tube is scalloped, and the scallop is an arc in the
  flat pattern;
- a plate STANDING ON the tube — which is what a suspension pickup is, so
  the rod end pulls along the tube instead of levering on it — lies in a
  plane through the tube's axis. Its seat is a cylinder cut ACROSS the 4 mm
  thickness, so the flat pattern's edge is STRAIGHT: the laser cannot cut a
  relief that runs through the plate, and does not need to. The blank is cut
  to the FAR side of the seat (12.264 mm off the axis of a 25.4 mm tube
  rather than 12.9), so it never fouls the tube and the 0.64 mm crescent
  left at the near face is the weld's root gap.
"""
import chassis_spec as S

PANEL_GAUGE = "1.5mm"   # floor and firewall
TAB_GAUGE = "4mm"       # a suspension pickup carries a rod end
SADDLE_GAP = 0.2        # the tab sits ON the tube, not pressed into it


def _check(result, what):
    failed = result.get("regen", {}).get("failed", [])
    if result.get("error") or failed:
        raise SystemExit(f"{what}: {failed or result}")


def _blank(d, part, gauge, plane, outline, holes=()):
    """A flat part: its own sheet rules, its own blank."""
    d.call("add_part", name=part)
    d.call("set_active_part", part=part)
    d.call("set_sheet_metal_rules", material=S.PANEL_MATERIAL, gauge=gauge)
    ents = []
    for i in range(len(outline)):
        ents.append({"type": "line", "a": list(outline[i]),
                     "b": list(outline[(i + 1) % len(outline)])})
    for c, r in holes:
        ents.append({"type": "circle", "center": list(c), "radius": r})
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "Blank", "plane": plane, "entities": ents})
    _check(sk, f"{part} blank")
    bf = d.call("add_feature", feature={
        "type": "BaseFlange", "name": part, "sketch_id": sk["id"]})
    _check(bf, part)
    return bf


def _saddle(d, part, at, direction, od, i):
    """Cut this blank to sit ON a tube rather than through it."""
    r = od / 2.0 + SADDLE_GAP
    base = tuple(at[k] - direction[k] * 200.0 for k in range(3))
    cyl = d.call("add_feature", feature={
        "type": "Cylinder", "name": f"Tube {i}", "center": list(base),
        "direction": list(direction), "radius": r, "height": 400.0})
    _check(cyl, f"{part} tube {i}")
    st = d.call("get_state")["bodies"]
    target = [b["created_by"] for b in st if b["created_by"] != cyl["id"]][0]
    r2 = d.call("add_feature", feature={
        "type": "Boolean", "name": f"Saddle {i}", "op": "subtract",
        "target_id": target, "tool_id": cyl["id"]})
    _check(r2, f"{part} saddle {i}")


# --- the panels -----------------------------------------------------------

def floor(d):
    """The floor, on top of the lower rails from the bulkhead to the seat."""
    # Sketch on the XY plane at the rails' height: u is +X, so V is +Y.
    plane = {"origin": [0.0, 0.0, S.Z_FLOOR + 12.7],
             "normal": [0.0, 0.0, 1.0], "uAxis": [1.0, 0.0, 0.0]}
    out = [(S.X_BULKHEAD, S.W_BULKHEAD), (S.X_MAIN_HOOP, S.W_MAIN_HOOP),
           (S.X_MAIN_HOOP, -S.W_MAIN_HOOP), (S.X_BULKHEAD, -S.W_BULKHEAD)]
    return _blank(d, "Floor", PANEL_GAUGE, plane, out)


def firewall(d):
    """Behind the driver, closing the cockpit off from the engine bay."""
    # Normal +X, u +Y, so V is +Z: it stands up.
    plane = {"origin": [S.X_MAIN_HOOP, 0.0, 0.0],
             "normal": [1.0, 0.0, 0.0], "uAxis": [0.0, 1.0, 0.0]}
    out = [(-S.W_MAIN_HOOP, S.Z_FLOOR), (S.W_MAIN_HOOP, S.Z_FLOOR),
           (S.W_MAIN_HOOP, S.Z_SIDE_UPPER), (-S.W_MAIN_HOOP, S.Z_SIDE_UPPER)]
    return _blank(d, "Firewall", PANEL_GAUGE, plane, out)


# --- the tabs -------------------------------------------------------------

def _pickup(d, name, at, along, od, reach=70.0, bolt=8.0):
    """A suspension pickup: a plate standing off a tube, with its bolt hole.

    Its plane CONTAINS the tube, which keeps the rod end pulling along the
    tube instead of levering on it, and makes the seat a cylinder cut across
    the plate's thickness (see the module docstring — the flat pattern's
    bottom edge is straight, and that is correct). A plate whose plane
    ignored the tube's direction came out not flat: the cylinder cut it at a
    grazing angle and left a curved wall.
    """
    up = (0.0, 0.0, 1.0)
    n = (along[1] * up[2] - along[2] * up[1],
         along[2] * up[0] - along[0] * up[2],
         along[0] * up[1] - along[1] * up[0])
    mag = sum(c * c for c in n) ** 0.5
    if mag < 1e-6:                      # a vertical tube: any plane will do
        n, mag = (0.0, 1.0, 0.0), 1.0
    plane = {"origin": list(at), "normal": [c / mag for c in n],
             "uAxis": list(along)}
    out = [(-reach * 0.45, 0.0), (reach * 0.45, 0.0),
           (reach * 0.45, reach), (-reach * 0.45, reach)]
    _blank(d, name, TAB_GAUGE, plane, out,
           holes=[((0.0, reach * 0.62), bolt / 2.0)])
    _saddle(d, name, at, along, od, 0)
    return name


def tabs(d, frame):
    """Where the suspension and the engine hang off the frame."""
    made = []
    for m in frame.members:
        # The lower rails carry the front suspension; the engine bay members
        # carry the engine.
        if m["name"] in ("Lower rail L", "Lower rail R"):
            line = frame._metal(m)
            for k, frac in ((0, 0.18), (1, 0.42)):
                i = int(frac * (len(line) - 1))
                a, b = line[i], line[min(i + 1, len(line) - 1)]
                d3 = [b[j] - a[j] for j in range(3)]
                n = sum(c * c for c in d3) ** 0.5 or 1.0
                made.append(_pickup(
                    d, f"Pickup {m['name'][-1]}{k + 1}", a,
                    [c / n for c in d3], S.OD[m["size"]]))
    return made
