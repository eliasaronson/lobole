#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""check_insertion and the `insertion` requirement over the RPC
(docs/inspect/README.md#insertion).

A Ø8 shaft and two rings 0.05 a side too small for it, one fastened at the
shaft's end and one in its middle:

- the end ring, slid on from the end it sits at, passes, pressed its own
  length; from the other end, and the middle ring from either end, fail,
  forced along the rest of the shaft;
- the axis comes from the one mate joining the pair, a named mate, or an
  explicit axis, and says which;
- an `insertion` requirement passes and fails as the RPC does;
- an unknown entry, a mate without an axis, a pair no mate joins with no
  axis given, an option out of range and a bad requirement key are
  refused, naming what is accepted.

Usage: insertion_smoke.py <path-to-designer-binary>
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lobole.client import Client  # noqa: E402
from smoke_app import launch  # noqa: E402
from smoke_shapes import cylinder  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **params):
    try:
        r = d.call(method, **params)
    except Exception as e:  # noqa: BLE001
        expect(needle in str(e), f"{method}: refusal names {needle!r}: {e}")
        return
    raise AssertionError(f"{method} {params} was not refused: {r}")


def ring(d, name, z0, bore_r):
    """A Ø16 ring 10 long from z0, its bore `bore_r`: one annulus sketch."""
    d.call("add_part", name=name)
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "S",
        "plane": {"origin": [0, 0, z0], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [0, 0], "radius": 8.0},
                     {"type": "circle", "center": [0, 0],
                      "radius": bore_r}]})
    d.call("add_feature", feature={
        "type": "Extrude", "name": "Ring", "sketch_id": sk["id"],
        "distance": 10.0, "mode": "new_body"})
    v = d.call("mass_properties")["total"]["volume_mm3"]
    expect(abs(v - 3.14159265 * (64.0 - bore_r ** 2) * 10.0) < 1.0,
           f"{name} is a ring: {v}")


def main(binary):
    with launch(binary) as app:
        d = Client(app.port, timeout=300)
        d.call("new_project")
        d.call("add_part", name="shaft")
        cylinder(d, (0, 0, 0), "z", 4.0, 30.0, "Shaft")
        ring(d, "end ring", 20.0, 3.95)
        ring(d, "middle ring", 10.0, 3.95)
        d.call("add_assembly", name="rig")
        asm = [p for p in d.call("list_parts")["parts"]
               if p["type"] == "assembly"][0]["id"]
        shaft = d.call("add_occurrence", part="shaft", fixed=True)["id"]
        occ = {}
        for name in ("end ring", "middle ring"):
            occ[name] = d.call("add_occurrence", part=name, name=name,
                               transform={"position": [0, 0, 0],
                                          "orientation": [0, 0, 0, 1]})["id"]
        mate = d.call("add_mate", type="fastened", name="end ring on shaft",
                      a={"occurrence": shaft, "origin": True},
                      b={"occurrence": occ["end ring"], "origin": True},
                      flipped=False)
        mate_id = mate.get("id", mate.get("mate", {}).get("id"))
        d.call("add_mate", type="fastened", name="middle ring on shaft",
               a={"occurrence": shaft, "origin": True},
               b={"occurrence": occ["middle ring"], "origin": True},
               flipped=False)

        # ---- The RPC -----------------------------------------------------
        r = d.call("check_insertion", a="end ring", b=shaft,
                   entry="positive")
        expect(r["pass"], f"the end ring from its own end: {r}")
        expect(abs(r["press_mm"] - 10.0) <= 2 * r["step_mm"],
               f"pressed its own length: {r}")
        expect(r["axis_from"] == "mate 'end ring on shaft'",
               f"the axis from the one mate joining them: {r}")
        expect(abs(r["axis_dir"][2]) > 0.999, f"along Z: {r}")
        r = d.call("check_insertion", a="end ring", b=shaft,
                   entry="negative")
        expect(not r["pass"] and r["excess_mm"] > 15.0,
               f"the end ring forced the shaft's length from below: {r}")
        r = d.call("check_insertion", a="middle ring", b=shaft,
                   entry="either", mate="middle ring on shaft")
        expect(not r["pass"] and abs(r["excess_mm"] - 10.0) <= 2 * r["step_mm"],
               f"the middle ring is forced half the shaft from either end: "
               f"{r}")
        r = d.call("check_insertion", a="end ring", b=shaft,
                   entry="positive",
                   axis={"origin_mm": [0, 0, 0], "direction": [0, 0, 1]})
        expect(r["pass"] and r["axis_from"] == "axis",
               f"an explicit axis: {r}")

        # ---- Refusals: the vocabulary is named ---------------------------
        refused(d, "check_insertion", "positive, negative, either",
                a="end ring", b=shaft, entry="top")
        refused(d, "check_insertion", "entry", a="end ring", b=shaft)
        refused(d, "check_insertion", "same occurrence", a=shaft, b=shaft,
                entry="positive")
        refused(d, "check_insertion", "no mate joins a and b",
                a="end ring", b="middle ring", entry="positive")
        refused(d, "check_insertion", "samples is a whole number",
                a="end ring", b=shaft, entry="positive", samples=1)
        refused(d, "check_insertion", "ONE of `mate` and `axis`",
                a="end ring", b=shaft, entry="positive", mate=mate_id,
                axis={"origin_mm": [0, 0, 0], "direction": [0, 0, 1]})
        refused(d, "check_insertion", "no mate", a="end ring", b=shaft,
                entry="positive", mate="no such mate")

        # ---- The requirement ---------------------------------------------
        good = d.call("add_requirement", name="end ring goes on",
                      measure={"kind": "insertion", "document": asm,
                               "a": occ["end ring"], "b": shaft,
                               "entry": "positive"},
                      bound={"kind": "pass"})["id"]
        bad = d.call("add_requirement", name="middle ring goes on",
                     measure={"kind": "insertion", "document": asm,
                              "a": occ["middle ring"], "b": shaft,
                              "entry": "either"},
                     bound={"kind": "pass"})["id"]
        res = d.call("check_requirements", ids=[good, bad])
        state = {r["id"]: r for r in res["requirements"]}
        expect(state[good]["state"] == "pass", f"end ring: {state[good]}")
        expect(state[bad]["state"] == "fail", f"middle ring: {state[bad]}")
        refused(d, "add_requirement", "accepted: positive, negative, either",
                name="x", measure={"kind": "insertion", "document": asm,
                                   "a": occ["end ring"], "b": shaft,
                                   "entry": "up"},
                bound={"kind": "pass"})
        refused(d, "add_requirement", "entry", name="x",
                measure={"kind": "insertion", "document": asm,
                         "a": occ["end ring"], "b": shaft},
                bound={"kind": "pass"})
        print("insertion smoke: ok")


if __name__ == "__main__":
    main(sys.argv[1])
