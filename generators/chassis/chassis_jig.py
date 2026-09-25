"""The weld jig: what holds the tubes while they are tacked.

A space frame is only as repeatable as the fixture it is built in. A jig is a
flat table, and a plate at every station notched so each tube DROPS IN from
above and cannot move — not a hole it has to be threaded through, because a
frame is assembled tube by tube and a closed hole can only be loaded from the
end.

Every plate here is a flat blank with its own DXF and its own mark, cut from
the same sheet as the panels. Nothing in the app knows what a fixture is;
this is what one looks like built out of the pieces that do exist — a sheet
part, a boolean against the tube, a flat pattern, a DXF.
"""
import math

import chassis_spec as S

# The thickest steel the sheet table stocks. A jig plate would rather be 8 or
# 10: it is laser-cut PLATE that never sees a brake, and its job is to not
# flex while a frame is tacked to it. The table stops at 5 because it is a
# BRAKE's table — every row in it is a gauge something can be bent in — and
# this build has no notion of plate that is cut but never formed.
GAUGE = "5mm"
CLEARANCE = 0.5         # around the tube, so it drops in rather than presses
BOLT = 9.0              # the table's M8 grid, clearance drilled
FOOT = 40.0             # how far the bolt holes sit in from the plate's edge
BAND = 220.0            # how far above the lowest tube a plate reaches


def _sub(a, b):
    return tuple(a[i] - b[i] for i in range(3))


def _crossings(frame, x):
    """Every member crossing the plane at `x`: where, and running which way.

    Read off the metal's own polyline, so a bent member is crossed where its
    ARC is rather than where its corners would have been.
    """
    out = []
    for m in frame.members:
        line = frame._metal(m)
        for i in range(len(line) - 1):
            a, b = line[i], line[i + 1]
            if (a[0] - x) * (b[0] - x) > 0 or abs(b[0] - a[0]) < 1e-9:
                continue
            t = (x - a[0]) / (b[0] - a[0])
            if not (-1e-9 <= t <= 1 + 1e-9):
                continue
            d = _sub(b, a)
            n = math.sqrt(sum(c * c for c in d))
            # It has to cross, not graze. A tube running nearly ALONG the
            # plate cannot drop into a round seat cut in it — the seat would
            # be a slot the length of the tube, which is a plate cut in half.
            # A main hoop stands exactly in its own station's plane, and that
            # is precisely what it did.
            if abs(d[0] / n) < 0.25:
                continue
            p = tuple(a[k] + t * (b[k] - a[k]) for k in range(3))
            out.append({"member": m, "at": p,
                        "dir": tuple(c / n for c in d),
                        "od": S.OD[m["size"]]})
            break
    return out


def plate(d, frame, name, mark, x, log=None):
    """One jig plate: the station at `x`, notched for every tube through it."""
    cross = _crossings(frame, x)
    if not cross:
        return None
    # A jig plate is LOW. It holds the tubes near the table and nothing else:
    # a plate tall enough to reach a tube near the roll hoop, slotted from
    # its top edge down to one near the floor, is two flapping fingers with a
    # gap between them. The photographs of a real fixture are a row of short
    # plates and blocks, and this is why.
    floor = min(c["at"][2] for c in cross)
    cross = [c for c in cross if c["at"][2] <= floor + BAND]
    half = max(abs(c["at"][1]) for c in cross) + 60.0
    top = max(c["at"][2] + c["od"] for c in cross) + 25.0

    d.call("add_part", name=name)
    d.call("set_active_part", part=name)
    d.call("set_sheet_metal_rules", material=S.PANEL_MATERIAL, gauge=GAUGE)

    # The blank: a rectangle standing on the table, with the bolt holes that
    # hold it there. The sketch plane's V is +Z here (normal +X, u +Y), so
    # the plate stands UP rather than hanging below the table.
    ents = [
        {"type": "line", "a": [-half, 0.0], "b": [half, 0.0]},
        {"type": "line", "a": [half, 0.0], "b": [half, top]},
        {"type": "line", "a": [half, top], "b": [-half, top]},
        {"type": "line", "a": [-half, top], "b": [-half, 0.0]},
    ]
    for sx in (-1.0, 1.0):
        ents.append({"type": "circle",
                     "center": [sx * (half - FOOT), FOOT * 0.6],
                     "radius": BOLT / 2.0})
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "Blank",
        "plane": {"origin": [x, 0.0, 0.0], "normal": [1.0, 0.0, 0.0],
                  "uAxis": [0.0, 1.0, 0.0]},
        "entities": ents})
    _check(sk, f"{name} blank")
    bf = d.call("add_feature", feature={
        "type": "BaseFlange", "name": "Plate", "sketch_id": sk["id"]})
    _check(bf, f"{name} plate")

    # Every tube that passes through, as a notch OPEN AT THE TOP: the round
    # seat it sits in, and the slot above it that lets it drop in.
    retried = 0
    for i, c in enumerate(cross):
        seat_r = c["od"] / 2.0 + CLEARANCE
        # A CYLINDER, not a swept pipe. A pipe's surface is a spline even
        # when its path is straight, so the notch it leaves comes out of the
        # DXF as three hundred short lines instead of an arc — and a machine
        # cuts a polygon three hundred times where it could cut one curve.
        a = tuple(c["at"][k] - c["dir"][k] * 200.0 for k in range(3))
        seat = d.call("add_feature", feature={
            "type": "Cylinder", "name": f"Seat {i}", "center": list(a),
            "direction": list(c["dir"]), "radius": seat_r, "height": 400.0})
        _check(seat, f"{name} seat {i}")
        retried += _cut(d, f"Notch {i}", seat)

        # The slot: from the tube's centre up past the top edge, as wide as
        # the seat. Its walls meet the seat's cylinder exactly along the
        # floor's edges, which OpenCASCADE's exact cut gets wrong (it came
        # back UNCUT); the app refuses that result and retries the cut with
        # the kernel's fuzzy option, and a feature that needed it carries a
        # warning (docs/modeling/README.md#a-refused-boolean-gets-one-fuzzy-retry).
        slot_w = seat_r
        rise = top - c["at"][2] + 20.0
        box = d.call("add_feature", feature={
            "type": "Box", "name": f"Slot {i}",
            "position": [x - 50.0, c["at"][1] - slot_w, c["at"][2]],
            "width": 100.0, "height": slot_w * 2.0, "depth": rise})
        _check(box, f"{name} slot {i}")
        retried += _cut(d, f"Open {i}", box)

    if log:
        log(f"    {name}: {len(cross)} tubes, {2 * half:.0f} x {top:.0f} mm"
            + (f", {retried} cut(s) through the fuzzy retry" if retried else ""))
    return {"name": name, "mark": mark, "tubes": len(cross),
            "fuzzy_retries": retried}


def _cut(d, name, tool):
    st = d.call("get_state")["bodies"]
    plate_id = [b["created_by"] for b in st if b["name"] not in
                ("Seat", "Slot") and b["created_by"] != tool["id"]][0]
    r = d.call("add_feature", feature={
        "type": "Boolean", "name": name, "op": "subtract",
        "target_id": plate_id, "tool_id": tool["id"]})
    _check(r, name)
    # 1 when the exact cut was refused and the fuzzy retry made it. The
    # regen block lists every feature's warning, so this one's by name.
    return int(any(w.get("name") == name and "fuzzy option" in w["warning"]
                   for w in r.get("regen", {}).get("warnings", [])))


def _check(result, what):
    failed = result.get("regen", {}).get("failed", [])
    if result.get("error") or failed:
        raise SystemExit(f"{what}: {failed or result}")


def build(d, frame, log=None):
    """A plate at every station where the frame needs holding."""
    made = []
    stations = [
        ("Jig bulkhead", "BH", S.X_BULKHEAD + 40.0),
        ("Jig nose ring", "NR", S.X_BULKHEAD_SUPPORT),
        # Offset from the hoops' own planes: a hoop stands IN its station,
        # so a plate there would have to be the hoop's shape. Just behind it,
        # the plate catches everything running into the hoop instead.
        ("Jig front hoop", "FH", S.X_FRONT_HOOP + 60.0),
        ("Jig main hoop", "MH", S.X_MAIN_HOOP + 60.0),
        ("Jig rear", "RR", S.X_REAR - 40.0),
    ]
    for name, mark, x in stations:
        got = plate(d, frame, name, mark, x, log)
        if got:
            made.append(got)
    return made
