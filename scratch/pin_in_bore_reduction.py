#!/usr/bin/env python3
"""Does an exact boolean lose a bore? The reduction behind the grill round's
"an exact boolean can lose a bore" finding (docs/grill-friction-plan.md).

A pin standing in the bore of a plug welded into a tube: the truth is the
annulus between pin and bore, plus the whole pin's section wherever it passes
through solid metal. The finding was that the lid handle — a bent `Pipe` with
a plug at each end — answered with the WHOLE pin instead. This asks the same
question of every construction the handle could have had, through both paths
that answer it: a `Boolean intersect` feature, and `interference` on two
placed occurrences.

    python3 scratch/pin_in_bore_reduction.py <port> [path/to/grill.dsgn]

Nothing reproduced it (2026-09-18). Keep the script: a measurement worth
recording is worth a driver recorded with it.
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_remote import Designer                       # noqa: E402

HALF, RISE, FORMER = 200.0, 60.0, 45.0
OD, WALL = 20.0, 1.5
BORE, PIN_D, PLUG, CAP = 5.0, 6.0, 10.0, 2.0

d = Designer(port=int(sys.argv[1]))
GRILL = sys.argv[2] if len(sys.argv) > 2 else None


def add(name, feat, empty_ok=False):
    feat["name"] = name
    r = d.call("add_feature", feature=feat)
    failed = r.get("regen", {}).get("failed", [])
    if empty_ok and any("empty" in f.get("error", "") for f in failed):
        return None          # an intersection of nothing IS the right answer
    if r.get("errors") or r.get("error") or failed:
        raise SystemExit(f"{name}: {r}")
    return r


def body_of(feat):
    for b in d.call("get_state")["bodies"]:
        if b.get("created_by") == feat["id"]:
            return b["created_by"]
    raise SystemExit("no body from " + str(feat))


def volume_of(created_by):
    state = d.call("get_state")["bodies"]
    for b in d.call("mass_properties")["bodies"]:
        if state[b["body"]].get("created_by") == created_by:
            return b["volume"]
    return None


def plane(z):
    return {"origin": [0, 0, z], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}


def circle(r, at=(0.0, 0.0)):
    return {"type": "circle", "center": [at[0], at[1]], "radius": r}


def extrude(name, z, entities, distance):
    s = add(name + " sk", {"type": "Sketch", "plane": plane(z),
                           "entities": entities})
    return add(name, {"type": "Extrude", "sketch_id": s["id"],
                      "distance": distance})


def truth(depth, capped):
    """Annulus through the plug, then the pin's whole section through a cap."""
    annulus = math.pi / 4 * (PIN_D**2 - BORE**2) * min(depth, PLUG)
    through = (math.pi / 4 * PIN_D**2 *
               max(0.0, min(depth, PLUG + CAP) - PLUG)) if capped else 0.0
    return annulus + through


def leg(kind, name):
    """A vertical leg of tube from the origin, built three ways."""
    d.call("add_part", name=name)
    d.call("set_active_part", part=name)
    if kind == "bent":
        p = add("Centreline", {"type": "Curve3D", "kind": "bent_path",
                               "radius": FORMER,
                               "points": [[0, 0, 0], [0, 0, RISE],
                                          [2 * HALF, 0, RISE],
                                          [2 * HALF, 0, 0]]})
        return add("Tube", {"type": "Pipe", "path_id": p["id"],
                            "outer_diameter": OD, "wall_thickness": WALL})
    if kind == "straight":
        p = add("Centreline", {"type": "Curve3D", "kind": "bent_path",
                               "radius": FORMER,
                               "points": [[0, 0, 0], [0, 0, RISE]]})
        return add("Tube", {"type": "Pipe", "path_id": p["id"],
                            "outer_diameter": OD, "wall_thickness": WALL})
    return extrude("Tube", 0.0, [circle(OD / 2), circle(OD / 2 - WALL)], RISE)


def case(kind, depth, capped):
    body = body_of(leg(kind, f"{kind}-{int(depth)}-{'cap' if capped else 'open'}"))
    plug = extrude("Plug", 0.0, [circle(OD / 2 - WALL), circle(BORE / 2)], PLUG)
    body = body_of(add("With plug", {"type": "Boolean", "op": "union",
                                     "target_id": body,
                                     "tool_id": body_of(plug)}))
    if capped:
        cap = extrude("Cap", PLUG, [circle(OD / 2 - WALL)], CAP)
        body = body_of(add("With cap", {"type": "Boolean", "op": "union",
                                        "target_id": body,
                                        "tool_id": body_of(cap)}))
    pin = extrude("Pin", 0.0, [circle(PIN_D / 2)], depth)
    common = add("Common", {"type": "Boolean", "op": "intersect",
                            "target_id": body, "tool_id": body_of(pin)})
    got = volume_of(body_of(common)) or 0.0
    want = truth(depth, capped)
    whole = math.pi / 4 * PIN_D**2 * depth
    # A 40 mm pin up the BENT leg leaves the tube through the bend's wall,
    # so it picks up real material the plug arithmetic does not count.
    slack = 2.0 if (kind == "bent" and depth > RISE - FORMER + PLUG) else 1.0
    verdict = ("OK" if abs(got - want) < slack else
               "WHOLE PIN" if abs(got - whole) < 1.0 else "??")
    print(f"  {kind:9s} {'capped' if capped else 'open':6s} pin {depth:5.1f}"
          f"  common={got:8.1f}  truth={want:7.1f}  whole={whole:7.1f}"
          f"  {verdict}")


print("A Boolean intersect feature, in the part:")
d.call("new_project")
for kind in ("bent", "straight", "extruded"):
    for depth, capped in ((10.0, True), (12.0, True), (20.0, True),
                          (12.0, False), (20.0, False), (40.0, False)):
        case(kind, depth, capped)
# A pin standing wholly inside the hollow tube, no plug: the truth is nothing.
for kind in ("bent", "straight", "extruded"):
    tube = leg(kind, "void-" + kind)
    pin = extrude("Pin", 1.0, [circle(PIN_D / 2)], PLUG)
    common = add("Common", {"type": "Boolean", "op": "intersect",
                            "target_id": body_of(tube),
                            "tool_id": body_of(pin)}, empty_ok=True)
    empty = volume_of(body_of(common)) if common else None
    print(f"  {kind:9s} pin inside the hollow tube: "
          f"{'empty, as it should be' if not empty else f'{empty:.1f} mm3 !!'}")

if GRILL:
    print("\nThe real handle, through `interference` on placed occurrences:")
    d.call("open_project", path=GRILL)
    d.call("add_part", name="Probe pin")
    d.call("set_active_part", part="Probe pin")
    extrude("Pin", 0.0, [circle(PIN_D / 2)], PLUG + CAP)
    for label, rotate in (("identity", None),
                          ("30deg about X", {"axis": [1, 0, 0],
                                             "angle_deg": 30.0}),
                          ("off-axis", {"axis": [0.3, 0.7, 0.2],
                                        "angle_deg": 47.0})):
        d.call("add_assembly", name="Probe " + label)
        d.call("set_active_part", part="Probe " + label)
        # The handle's bore is at (200, 0, 0..10), axis -Z. Turn the pin's
        # origin by the same rotation so it stays in it.
        at = [200.0, 0.0, 0.0]
        handle = {"part": "Lid handle", "name": "handle", "fixed": True}
        if rotate:
            handle["transform"] = {"position": [0, 0, 0], "rotate": rotate}
            n = math.sqrt(sum(c * c for c in rotate["axis"]))
            ux, uy, uz = [c / n for c in rotate["axis"]]
            th = math.radians(rotate["angle_deg"])
            ct, st = math.cos(th), math.sin(th)
            px, py, pz = at
            dot = ux * px + uy * py + uz * pz
            at = [px * ct + (uy * pz - uz * py) * st + ux * dot * (1 - ct),
                  py * ct + (uz * px - ux * pz) * st + uy * dot * (1 - ct),
                  pz * ct + (ux * py - uy * px) * st + uz * dot * (1 - ct)]
        pin = {"part": "Probe pin", "name": "pin", "fixed": True,
               "transform": {"position": at}}
        if rotate:
            pin["transform"]["rotate"] = rotate
        d.call("add_occurrence", **handle)
        d.call("add_occurrence", **pin)
        pairs = d.request("interference", {"mode": "exact"}).get("pairs", [])
        volumes = [round(p.get("volume_mm3", p.get("volume", 0.0)), 1)
                   for p in pairs]
        print(f"  {label:14s} {volumes}  truth={truth(PLUG + CAP, True):.1f}")
