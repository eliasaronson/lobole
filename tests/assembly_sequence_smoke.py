#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Assembly sequences over the RPC (docs/assemblies/README.md#assembly-sequences).

A clevis -- a lug, a fork stacked on it, and a headed pin through both --
and a split bearing -- a lower block, a collared shaft laid in its
half-bore, and a cap over it:

- the pin goes in last and passes; put in before the fork, the fork is
  blocked by the pin's head, an EARLIER part that is not its mate partner,
  and the sequence fails at the fork, naming what it was checked against;
- the cap slid along its bore (the default path: the axis of its own
  cylinder that its mate seats) is stopped by the shaft's collars; lowered
  along its bolts (a `direction`) it goes home;
- an occurrence no step places is listed in `missing` and fails the
  sequence; a step with no mate to anything placed before it and no
  direction, an unknown step key and an occurrence named twice are refused;
- the sequence is saved and opened again; the requirement kind
  `assembly_sequence` passes and fails as the RPC does; the check runs as
  an `assembly_sequence` job.

Usage: assembly_sequence_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lobole.client import Client  # noqa: E402
from smoke_app import launch  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402


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


def plate(d, name, z0, hole_r):
    """A 40 x 40 plate 10 thick from z0, a hole `hole_r` through it at the
    origin."""
    d.call("add_part", name=name)
    box(d, (-20, -20, z0), (40, 40, 10), f"{name} plate")
    cylinder(d, (0, 0, z0 - 1), "z", hole_r, 12, f"{name} hole", mode="cut")


def fastened(d, name, a, b):
    d.call("add_mate", type="fastened", name=name,
           a={"occurrence": a, "origin": True},
           b={"occurrence": b, "origin": True}, flipped=False)


def check(d):
    return d.call("check_assembly_sequence")


def main(binary):
    env = dict(os.environ)
    # Holds each check's job before it computes (tests only), so the smoke
    # sees it running off the frame.
    env["LOBOLE_TEST_INSERTION_MS"] = "600"
    with launch(binary, env=env) as app:
        d = Client(app.port, timeout=300)
        d.call("new_project")
        # ---- The clevis --------------------------------------------------
        plate(d, "lug", 0.0, 3.0)
        plate(d, "fork", 10.0, 3.0)
        d.call("add_part", name="pin")
        cylinder(d, (0, 0, -5), "z", 2.9, 30, "shank")
        cylinder(d, (0, 0, 25), "z", 5.0, 3, "head", mode="add")
        d.call("add_part", name="washer")
        cylinder(d, (40, 0, 0), "z", 4.0, 1, "washer")
        d.call("add_assembly", name="clevis")
        asm = [p for p in d.call("list_parts")["parts"]
               if p["type"] == "assembly"][0]["id"]
        lug = d.call("add_occurrence", part="lug", name="lug",
                     fixed=True)["id"]
        occ = {"lug": lug}
        for name in ("fork", "pin", "washer"):
            occ[name] = d.call("add_occurrence", part=name, name=name)["id"]
        fastened(d, "fork on lug", lug, occ["fork"])
        fastened(d, "pin in lug", lug, occ["pin"])
        fastened(d, "washer by lug", lug, occ["washer"])

        refused(d, "set_assembly_sequence", "unknown key 'dir'",
                steps=[{"occurrence": "lug"},
                       {"occurrence": "fork", "dir": [0, 0, -1]}])
        refused(d, "set_assembly_sequence", "placed by an earlier step",
                steps=[{"occurrence": "lug"}, {"occurrence": "lug"}])
        refused(d, "set_assembly_sequence", "direction has no length",
                steps=[{"occurrence": "lug"},
                       {"occurrence": "fork", "direction": [0, 0, 0]}])
        refused(d, "check_assembly_sequence", "no sequence")

        right = [{"occurrence": "lug", "note": "the base"},
                 {"occurrence": "fork"}, {"occurrence": "pin"},
                 {"occurrence": "washer", "direction": [0, 0, -1]}]
        d.call("set_assembly_sequence", steps=right)
        other = Client(app.port, timeout=60)
        answer = {}
        worker = threading.Thread(target=lambda: answer.update(check(d)))
        worker.start()
        kinds = set()
        while worker.is_alive():
            for j in other.call("background_jobs")["jobs"]:
                kinds.add(j["kind"])
            time.sleep(0.05)
        worker.join()
        expect("assembly_sequence" in kinds,
               f"the check runs as an assembly_sequence job: {kinds}")
        r = answer
        expect(r["pass"], f"the pin last goes home: {r}")
        pin = r["steps"][2]
        expect(pin["against"] == ["lug", "fork"],
               f"the pin is checked against both plates: {pin}")
        expect("cylinder" in pin["path"]["from"]
               and abs(abs(pin["path"]["moves"][2]) - 1.0) < 1e-6,
               f"its path is its own cylinder's axis: {pin}")
        expect(pin["path"]["moves"][2] < 0,
               f"and it goes in from above, head last: {pin}")
        expect(r["steps"][0].get("base") is True, f"the base: {r}")

        # The pin before the fork: the fork cannot come down past its head.
        wrong = [right[0], right[2], right[1], right[3]]
        d.call("set_assembly_sequence", steps=wrong)
        r = check(d)
        expect(not r["pass"], f"the pin before the fork fails: {r}")
        fork = r["steps"][2]
        expect(fork["name"] == "fork" and not fork["pass"]
               and "pin" in fork["against"],
               f"at the fork, checked against the pin: {fork}")
        expect(r["steps"][1]["pass"],
               f"the pin itself goes into the lug alone: {r['steps'][1]}")

        # A step with no mate to anything placed before it, and no path.
        d.call("set_assembly_sequence",
               steps=[{"occurrence": "fork"}, {"occurrence": "pin"}])
        refused(d, "check_assembly_sequence", "step 2 ('pin')")
        # The washer missed.
        d.call("set_assembly_sequence", steps=right[:3])
        r = check(d)
        expect(not r["pass"] and r["missing"] == ["washer"],
               f"the washer is missed and says so: {r}")

        # ---- Saved and opened again; the requirement ---------------------
        d.call("set_assembly_sequence", steps=right, allowance_mm=0.05)
        path = os.path.join(tempfile.mkdtemp(), "sequence.dsgn")
        d.call("save_project", path=path)
        d.call("open_project", path=path)
        d.call("set_active_part", part="clevis")
        got = d.call("get_assembly_sequence")
        expect([s["name"] for s in got["steps"]]
               == ["lug", "fork", "pin", "washer"]
               and got["allowance_mm"] == 0.05
               and got["steps"][3]["direction"] == [0.0, 0.0, -1.0]
               and got["steps"][0]["note"] == "the base",
               f"the sequence comes back as it was set: {got}")
        req = d.call("add_requirement", name="the clevis goes together",
                     measure={"kind": "assembly_sequence", "document": asm},
                     bound={"kind": "pass"})["id"]
        row = d.call("check_requirements", ids=[req])["requirements"][0]
        expect(row["state"] == "pass", f"the requirement passes: {row}")
        d.call("set_assembly_sequence", steps=wrong)
        row = d.call("check_requirements", ids=[req])["requirements"][0]
        expect(row["state"] == "fail",
               f"and fails once the order is wrong: {row}")

        # ---- The split bearing -------------------------------------------
        d.call("add_part", name="block")
        box(d, (-15, -15, -10), (30, 30, 10), "block")
        cylinder(d, (-16, 0, 0), "x", 5.2, 32, "lower bore", mode="cut")
        d.call("add_part", name="cap")
        box(d, (-15, -15, 0), (30, 30, 10), "cap")
        cylinder(d, (-16, 0, 0), "x", 5.2, 32, "upper bore", mode="cut")
        d.call("add_part", name="shaft")
        cylinder(d, (-30, 0, 0), "x", 5.0, 60, "journal")
        cylinder(d, (18, 0, 0), "x", 8.0, 4, "collar", mode="add")
        cylinder(d, (-22, 0, 0), "x", 8.0, 4, "other collar", mode="add")
        d.call("add_assembly", name="bearing")
        d.call("set_active_part", part="bearing")
        blk = d.call("add_occurrence", part="block", name="block",
                     fixed=True)["id"]
        shaft = d.call("add_occurrence", part="shaft", name="shaft")["id"]
        cap = d.call("add_occurrence", part="cap", name="cap")["id"]
        d.call("add_mate", type="revolute", name="shaft in block",
               a={"occurrence": blk, "origin": True},
               b={"occurrence": shaft, "origin": True}, flipped=False)
        fastened(d, "cap on block", blk, cap)
        down = [0, 0, -1]
        d.call("set_assembly_sequence",
               steps=[{"occurrence": "block"},
                      {"occurrence": "shaft", "direction": down},
                      {"occurrence": "cap"}])
        r = check(d)
        capr = r["steps"][2]
        expect(not r["pass"] and not capr["pass"]
               and "cylinder" in capr["path"]["from"]
               and abs(abs(capr["path"]["moves"][0]) - 1.0) < 1e-6,
               f"the cap slid along its bore hits a collar: {capr}")
        d.call("set_assembly_sequence",
               steps=[{"occurrence": "block"},
                      {"occurrence": "shaft", "direction": down},
                      {"occurrence": "cap", "direction": down,
                       "note": "down its bolts"}])
        r = check(d)
        expect(r["pass"], f"the cap lowered along its bolts goes home: {r}")
        print("assembly sequence smoke: ok")


if __name__ == "__main__":
    main(sys.argv[1])
