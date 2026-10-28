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
  an `assembly_sequence` job;
- two wheels whose disks overlap in plan at two heights block each other
  one at a time, and go home as a GROUP along their common seat axis; a
  group whose members' seat axes are not parallel is refused; the groups
  survive save and reopen;
- a cap fastened on a crank's pin, lowered under a bar, fails with the
  crank at rest and passes with it turned by a POSE step; a pose of a mate
  with no such freedom is refused.

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


def frames(d, specs):
    """Explicit frames, Z up: {name: origin}."""
    for name, origin in specs.items():
        d.call("add_feature", feature={
            "type": "Frame", "name": name, "kind": "explicit",
            "origin": list(origin), "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]})


def frame_mate(d, kind, name, a, fa, b, fb):
    return d.call("add_mate", type=kind, name=name,
                  a={"occurrence": a, "frame": fa},
                  b={"occurrence": b, "frame": fb}, flipped=False)["id"]


def groups(d):
    """Two wheels in a plate, each disk over the other's at another height:
    one at a time each is blocked by the other, together they go home."""
    d.call("add_part", name="gplate")
    box(d, (-20, -20, -10), (60, 40, 10), "gplate")
    cylinder(d, (0, 0, -11), "z", 3.2, 12, "hole a", mode="cut")
    cylinder(d, (20, 0, -11), "z", 3.2, 12, "hole b", mode="cut")
    frames(d, {"a": (0, 0, 0), "b": (20, 0, 0)})
    d.call("add_feature", feature={
        "type": "Frame", "name": "c", "kind": "explicit",
        "origin": [0, 40, 20], "z_axis": [1, 0, 0], "x_axis": [0, 1, 0]})
    d.call("add_part", name="wheel a")
    cylinder(d, (0, 0, -10), "z", 3.0, 40, "arbor a")
    cylinder(d, (0, 0, 20), "z", 14.0, 3, "disk a", mode="add")
    frames(d, {"seat": (0, 0, 0)})
    d.call("add_part", name="wheel b")
    cylinder(d, (20, 0, -10), "z", 3.0, 40, "arbor b")
    cylinder(d, (20, 0, 25), "z", 14.0, 3, "upper b", mode="add")
    cylinder(d, (20, 0, 12), "z", 14.0, 3, "lower b", mode="add")
    frames(d, {"seat": (20, 0, 0)})
    d.call("add_part", name="crossbar")
    cylinder(d, (-5, 40, 20), "x", 2.0, 10, "crossbar")
    d.call("add_feature", feature={
        "type": "Frame", "name": "seat", "kind": "explicit",
        "origin": [0, 40, 20], "z_axis": [1, 0, 0], "x_axis": [0, 1, 0]})
    d.call("add_assembly", name="wheels")
    d.call("set_active_part", part="wheels")
    pl = d.call("add_occurrence", part="gplate", name="gplate",
                fixed=True)["id"]
    wa = d.call("add_occurrence", part="wheel a", name="wheel a")["id"]
    wb = d.call("add_occurrence", part="wheel b", name="wheel b")["id"]
    cb = d.call("add_occurrence", part="crossbar", name="crossbar")["id"]
    frame_mate(d, "revolute", "a in plate", pl, "a", wa, "seat")
    frame_mate(d, "revolute", "b in plate", pl, "b", wb, "seat")
    d.call("add_mate", type="fastened", name="crossbar on plate",
           a={"occurrence": pl, "frame": "c"},
           b={"occurrence": cb, "frame": "seat"}, flipped=False)
    tail = {"occurrence": "crossbar", "direction": [0, 0, -1]}
    for first, second in (("wheel a", "wheel b"), ("wheel b", "wheel a")):
        d.call("set_assembly_sequence",
               steps=[{"occurrence": "gplate"}, {"occurrence": first},
                      {"occurrence": second}, tail])
        r = check(d)
        expect(r["steps"][1]["pass"], f"{first} alone goes in: {r}")
        blocked = r["steps"][2]
        expect(not r["pass"] and not blocked["pass"]
               and blocked.get("worst_against") == first,
               f"{second} after {first} is blocked by it: {blocked}")
        # The plate's sections were cut for the first wheel's step and
        # moved to the second's path, not cut again.
        expect(blocked["solids"]["reused"] > 0,
               f"the plate's sections reused: {blocked}")
    d.call("set_assembly_sequence",
           steps=[{"occurrence": "gplate"},
                  {"occurrences": ["wheel a", "wheel b"],
                   "note": "meshed, together"}, tail])
    r = check(d)
    g = r["steps"][1]
    expect(r["pass"] and g["name"] == "wheel a + wheel b"
           and g["against"] == ["gplate"] and "share" in g["path"]["from"]
           and abs(abs(g["path"]["moves"][2]) - 1.0) < 1e-6,
           f"together they go home along their common axis: {r}")

    # The crossbar's seat axis is X: no axis is common to it and a wheel.
    d.call("set_assembly_sequence",
           steps=[{"occurrence": "gplate"},
                  {"occurrences": ["wheel a", "crossbar"]},
                  {"occurrence": "wheel b", "direction": [0, 0, -1]}])
    refused(d, "check_assembly_sequence", "no common seat axis")
    refused(d, "set_assembly_sequence", "a group of two or more",
            steps=[{"occurrence": "gplate"}, {"occurrences": ["wheel a"]}])
    refused(d, "set_assembly_sequence", "states one of",
            steps=[{"occurrence": "gplate", "occurrences": ["wheel a"]}])
    # Saved and opened again.
    steps = [{"occurrence": "gplate"},
             {"occurrences": ["wheel a", "wheel b"]}, tail]
    d.call("set_assembly_sequence", steps=steps)
    path = os.path.join(tempfile.mkdtemp(), "groups.lob")
    d.call("save_project", path=path)
    d.call("open_project", path=path)
    d.call("set_active_part", part="wheels")
    got = d.call("get_assembly_sequence")
    expect(got["steps"][1].get("names") == ["wheel a", "wheel b"],
           f"the group comes back: {got}")
    expect(check(d)["pass"], "and still goes home")


def poses(d):
    """A crank with a pin, a cap fastened on the pin, and a bar over the pin
    at rest: the cap lowered at rest hits the bar; with the crank turned a
    quarter by a pose step it goes home."""
    d.call("add_part", name="frame")
    box(d, (-50, -50, -10), (110, 100, 10), "frame base")
    box(d, (45, -10, 0), (10, 20, 35), "pillar", mode="add")
    box(d, (20, -10, 30), (35, 20, 5), "bar", mode="add")
    d.call("add_part", name="crank")
    cylinder(d, (0, 0, 0), "z", 8.0, 5, "hub")
    box(d, (0, -4, 0), (30, 8, 5), "arm", mode="add")
    cylinder(d, (30, 0, 5), "z", 3.0, 10, "crank pin", mode="add")
    d.call("add_part", name="pin cap")
    cylinder(d, (30, 0, 15), "z", 5.0, 5, "pin cap")
    d.call("add_assembly", name="cranked")
    d.call("set_active_part", part="cranked")
    fr = d.call("add_occurrence", part="frame", name="frame",
                fixed=True)["id"]
    cr = d.call("add_occurrence", part="crank", name="crank")["id"]
    cp = d.call("add_occurrence", part="pin cap", name="cap")["id"]
    d.call("add_mate", type="revolute", name="crank bearing",
           a={"occurrence": fr, "origin": True},
           b={"occurrence": cr, "origin": True}, flipped=False)
    fastened(d, "cap on pin", cr, cp)
    down = [0, 0, -1]
    turned = {"pose": [{"mate": "crank bearing", "rotation_deg": 90.0}],
              "note": "the crank a quarter turned"}
    rest = {"pose": [{"mate": "crank bearing", "rotation_deg": 0.0}]}
    d.call("set_assembly_sequence",
           steps=[{"occurrence": "frame"}, turned,
                  {"occurrence": "crank", "direction": down}, rest,
                  {"occurrence": "cap", "direction": down}])
    r = check(d)
    cap = r["steps"][4]
    expect(r["steps"][2]["pass"], f"the crank goes in turned: {r}")
    expect(not r["pass"] and not cap["pass"]
           and cap.get("worst_against") == "frame",
           f"the cap at rest hits the bar: {cap}")
    expect(r["steps"][1]["pose"][0]["mate"] == "crank bearing"
           and "crank" in r["steps"][1]["moved"],
           f"a pose step says what it moved: {r['steps'][1]}")
    d.call("set_assembly_sequence",
           steps=[{"occurrence": "frame"}, turned,
                  {"occurrence": "crank", "direction": down},
                  {"occurrence": "cap", "direction": down}])
    r = check(d)
    expect(r["pass"] and r["steps"][3].get("at_pose") == 1,
           f"with the crank turned the cap goes home: {r}")
    refused(d, "set_assembly_sequence", "has no rotation to pose",
            steps=[{"occurrence": "frame"},
                   {"pose": [{"mate": "cap on pin", "rotation_deg": 10.0}]}])
    refused(d, "set_assembly_sequence", "has no `direction`",
            steps=[{"occurrence": "frame"},
                   {"pose": [{"mate": "crank bearing",
                              "rotation_deg": 10.0}],
                    "direction": down}])
    # Saved and opened again, the pose is still there and still holds.
    path = os.path.join(tempfile.mkdtemp(), "poses.lob")
    d.call("save_project", path=path)
    d.call("open_project", path=path)
    d.call("set_active_part", part="cranked")
    got = d.call("get_assembly_sequence")
    expect(got["steps"][1]["pose"][0]["rotation_deg"] == 90.0
           and got["steps"][1]["pose"][0]["mate_name"] == "crank bearing",
           f"the pose comes back: {got}")
    expect(check(d)["pass"], "and the cap still goes home at it")


def without_reused(r):
    return {k: v for k, v in r.items() if k != "reused"}


def kept_answers(d, req, right, wrong):
    """The check's answer is kept under everything it reads
    (docs/assemblies/README.md#a-check-answered-again): asked again of the
    same assembly it is answered from that, the requirement too; an edit to
    the assembly, to the sequence or to a placed part checks again."""
    d.call("set_assembly_sequence", steps=right, allowance_mm=0.03)
    first = check(d)
    expect(first["pass"] and "reused" not in first,
           f"a sequence not checked before is checked: {first}")
    again = check(d)
    expect(again.get("reused") is True,
           f"asked again unchanged, the answer is reused: {again}")
    expect(without_reused(again) == first,
           f"and it is the same answer: {again} vs {first}")
    row = d.call("check_requirements", ids=[req])["requirements"][0]
    expect(row["state"] == "pass"
           and row.get("detail", {}).get("reused") is True,
           f"the requirement reads the kept answer: {row}")
    # The assembly: another washer, which no step places.
    extra = d.call("add_occurrence", part="washer", name="washer 2")["id"]
    r = check(d)
    expect("reused" not in r and not r["pass"]
           and r["missing"] == ["washer 2"],
           f"an occurrence added is checked again: {r}")
    d.call("remove_occurrence", id=extra)
    r = check(d)
    expect(r.get("reused") is True and without_reused(r) == first,
           f"removed again, it is the assembly answered before: {r}")
    # The sequence: the pin before the fork, which its head blocks.
    d.call("set_assembly_sequence", steps=wrong, allowance_mm=0.03)
    r = check(d)
    expect("reused" not in r and not r["pass"],
           f"the order changed is checked again: {r}")
    # A placed part: the pin's head cut away, the fork comes down past it.
    d.call("set_active_part", part="pin")
    cylinder(d, (0, 0, 24.5), "z", 6.0, 5, "no head", mode="cut")
    d.call("set_active_part", part="clevis")
    r = check(d)
    expect("reused" not in r and r["pass"],
           f"the pin edited is checked again, and now goes: {r}")


def assembly_snapshot(d):
    """What a check must leave alone: the tabs, the occurrences and where
    they are, the mates, the sequence and the requirements."""
    snap = {m: d.call(m) for m in ("list_parts", "list_occurrences",
                                    "list_mates", "list_requirements")}
    try:
        snap["sequence"] = d.call("get_assembly_sequence")
    except RuntimeError as e:
        snap["sequence"] = str(e)
    return snap


def cancel_assembly_sequence(d, port):
    """An `assembly_sequence` job cancelled while check_assembly_sequence
    waits on it (held by LOBOLE_TEST_INSERTION_MS): the call is REFUSED,
    saying it was cancelled, and nothing in the assembly changed."""
    other = Client.wait_for(port)
    before = assembly_snapshot(other)
    out = {}

    def go():
        try:
            out["answer"] = d.call("check_assembly_sequence")
        except RuntimeError as e:
            out["error"] = str(e)

    worker = threading.Thread(target=go)
    worker.start()
    job, deadline = None, time.time() + 30
    while job is None:
        job = next((j for j in other.call("background_jobs")["jobs"]
                    if j["kind"] == "assembly_sequence"), None)
        if job is None:
            expect(worker.is_alive() and time.time() < deadline,
                   f"no assembly_sequence job while check_assembly_sequence waited: {out}")
            time.sleep(0.02)
    expect(job["cancellable"] is True,
           f"an assembly_sequence job says it can be cancelled: {job}")
    asked = other.call("cancel_background_job", kind="assembly_sequence")
    expect(asked.get("asked", 0) >= 1, f"the cancel reached the job: {asked}")
    worker.join(60)
    expect(not worker.is_alive(), "the cancelled check_assembly_sequence never answered")
    expect("cancelled" in out.get("error", ""),
           f"a cancelled check_assembly_sequence is refused, saying so: {out}")
    after = assembly_snapshot(other)
    expect(after == before,
           f"a cancelled check_assembly_sequence changed the assembly:\n{before}\n{after}")
    deadline = time.time() + 5
    while any(j["kind"] == "assembly_sequence"
              for j in other.call("background_jobs")["jobs"]):
        expect(time.time() < deadline,
               "the cancelled assembly_sequence job is still listed 5 s later")
        time.sleep(0.05)
    other.close()


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
        cancel_assembly_sequence(d, app.port)
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
        path = os.path.join(tempfile.mkdtemp(), "sequence.lob")
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

        kept_answers(d, req, right, wrong)

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
        groups(d)
        poses(d)
        print("assembly sequence smoke: ok")


if __name__ == "__main__":
    main(sys.argv[1])
