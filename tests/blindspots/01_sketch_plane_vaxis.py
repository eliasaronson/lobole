"""Blind spot: the sketch-plane V axis is normal x uAxis, NOT uAxis x
normal. An XZ plane with normal +Y and uAxis +X maps sketch +v to world
-Z, so a profile authored "above" the part lands behind it and a cut
removes nothing — silently, before the no-op warning existed.

Incident: the eye-bracket demo ramp cut (memory: designer-sketch-plane-
vaxis), re-hit in cut form on the Alpair enclosure.
"""


def run(d, expect):
    d.call("add_feature", feature={
        "type": "Box", "name": "Block", "position": [0, 0, 0],
        "width": 20, "height": 20, "depth": 20})

    # WRONG: top plane with normal +Y -> V = -Z; the circle at v=+10 lands
    # at world z=-10, off the block, and the cut extrudes upward besides.
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "Wrong plane",
        "plane": {"origin": [0, 20, 0], "normal": [0, 1, 0],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [10, 10], "radius": 4}]})
    r = d.call("add_feature", feature={
        "type": "Extrude", "name": "Cut", "sketch_id": sk["id"],
        "distance": 25, "mode": "cut"})
    expect(abs(r["digest"]["delta"]["volume"]) < 1e-6,
           f"the miss must be visible as a zero delta: {r['digest']}")
    expect(r["regen"]["warnings"],
           f"a no-op cut must WARN, not pass silently: {r['regen']}")
    d.call("undo")
    d.call("undo")

    # RIGHT: normal -Y makes V = +Z; same sketch coordinates land on the
    # block and the cut (along -Y, into it) removes the cylinder.
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "Right plane",
        "plane": {"origin": [0, 20, 0], "normal": [0, -1, 0],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [10, 10], "radius": 4}]})
    r = d.call("add_feature", feature={
        "type": "Extrude", "name": "Cut", "sketch_id": sk["id"],
        "distance": 25, "mode": "cut"})
    import math
    expect(abs(r["digest"]["delta"]["volume"] + math.pi * 16 * 20) < 1.0,
           f"flipped plane must cut the full bore: {r['digest']['delta']}")
    expect(not r["regen"]["warnings"], f"clean cut: {r['regen']}")
