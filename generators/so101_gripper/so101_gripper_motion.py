#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Give the assembly its joints, then drive them.

    python3 generators/so101_gripper/so101_gripper_motion.py --port 8800            # mate, solve, study, run, export

Every pivot becomes a real revolute between two named datum frames, so the
mechanism is a kinematic model rather than sixteen parts posed by a script:
the solver closes the loops, a motion study drives ONE joint through the
whole stroke, and the app checks every occurrence pair for a clash at every
sample.

The gear pair is a RELATION, not a mate: `add_relation {type: "gear",
a, b: the two drive pivots, teeth_a, teeth_b}` couples the two revolutes'
rotations at -20/20, which is what meshing teeth do. That is the whole
coupling — one degree of freedom for the mechanism, no stand-in, and no
branch to pick.

(The first version of this file used a SLIDER between the fingers instead,
because nothing in add_mate or describe pointed at relations. It is
kinematically the same constraint and it has a second solution the gears do
not — both fingers sliding the same way — which the solver took the moment a
sweep crossed mid-stroke, driving tooth through tooth. Two lessons, and the
better one is: look for a relation.)
"""
import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                          # this generator's own files
sys.path.insert(0, os.path.join(HERE, ".."))      # generators/: _lib, gear
import so101_gripper_spec as P                         # noqa: E402
from _lib import connect                               # noqa: E402
from so101_gripper_assembly import ASM, placements     # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                   "scratch", "so101-pg52-gripper")
# The driver mate measures its rotation the other way round from the link
# angle theta this design is parameterised in: mate_rotation = -theta.
MATE_SIGN = -1.0


def occ(name, frame=None):
    """A mate connector, by occurrence NAME — which add_mate resolves."""
    c = {"occurrence": name}
    if frame:
        c["frame"] = frame
    else:
        c["origin"] = True
    return c


def mate_list():
    """(name, type, a, b) for every joint in the gripper."""
    m = []
    for side, sn in ((-1, "L"), (1, "R")):
        for sy, yn in ((1, "+1"), (-1, "-1")):
            n = "+" if sy > 0 else "-"
            m.append((f"drive pivot {sn}{n}", "revolute",
                      occ(f"DrivePlate {sn}{yn}", "A1"),
                      occ("Chassis", f"A1 {sn}{n}")))
            m.append((f"coupler pivot {sn}{n}", "revolute",
                      occ(f"CouplerPlate {sn}{yn}", "B1"),
                      occ("Chassis", f"B1 {sn}{n}")))
            m.append((f"finger drive {sn}{n}", "revolute",
                      occ(f"Finger {sn}", f"A2{n}"),
                      occ(f"DrivePlate {sn}{yn}", "A2")))
            m.append((f"finger keeper {sn}{n}", "revolute",
                      occ(f"Finger {sn}", f"B2{n}"),
                      occ(f"CouplerPlate {sn}{yn}", "B2")))
    # (the gear pair is a relation over two of these mates, added below)
    # everything that is simply bolted on
    m.append(("servo in its cradle", "fastened", occ("STS3215"),
              occ("Chassis", "servo seat")))
    m.append(("camera pod", "fastened", occ("Camera Pod"),
              occ("Chassis", "pod seat")))
    m.append(("camera module", "fastened", occ("Camera Module"),
              occ("Camera Pod", "camera seat")))
    for sn in ("L", "R"):
        m.append((f"jaw pad {sn}", "fastened", occ(f"Jaw Pad {sn}"),
                  occ(f"Finger {sn}", "pad seat")))
    return m


def build_relations(d, ids):
    """The gear pair, as what it is. Both plates carry the same 20-tooth
    sector on the same pitch radius, so the ratio is -20/20: meshing teeth
    counter-rotate, which `gear` says by sign and `chain` would not."""
    for r in d.call("list_relations").get("relations", []):
        d.call("remove_relation", id=r["id"])
    out = []
    for band in ("+", "-"):
        rel = d.call("add_relation", type="gear",
                     a=ids[f"drive pivot L{band}"],
                     b=ids[f"drive pivot R{band}"],
                     teeth_a=P.GEAR["teeth"], teeth_b=P.GEAR["teeth"],
                     name=f"gear mesh {band}Y")
        out.append(rel)
    return out


def build_mates(d):
    d.call("set_active_part", part=ASM)
    for mt in d.call("list_mates").get("mates", []):
        d.call("remove_mate", id=mt["id"])
    ids = {}
    for name, kind, a, b in mate_list():
        r = d.call("add_mate", type=kind, a=a, b=b, flipped=False, name=name)
        ids[name] = r.get("id", r)
    return ids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--samples", type=int, default=49)
    a = ap.parse_args()
    d = connect(a.port)
    d.sock.settimeout(1800)

    # Mid-stroke is fine to mate at. It was not when the coupling was a
    # slider: that had a second branch meeting this one exactly here, and
    # mating at the meeting point let the solver take it. A gear relation
    # has no second branch.
    d.call("set_active_part", part=ASM)
    by = {o["name"]: o["id"] for o in d.call("list_occurrences")["occurrences"]}
    d.call("batch", calls=[
        {"method": "set_occurrence_transform",
         "params": {"id": by[n], "transform": {"position": list(p),
                                               "orientation": list(q)}}}
        for n, (p, q) in placements(0.0).items() if n in by])

    ids = build_mates(d)
    rels = build_relations(d, ids)
    print(f"{len(ids)} mates, {len(rels)} gear relations")
    for r in rels:
        if r.get("warnings"):
            print("   relation warning:", r["warnings"])
    r = d.request("solve_assembly", {}, timeout=600)
    print("solve:", json.dumps({k: v for k, v in r.items()
                                if k != "mates"})[:400])
    bad = [m["name"] for m in r.get("mates", []) if m.get("error")]
    print("mates with an error:", bad or "none")

    for st in d.call("list_motion_studies").get("studies", []):
        d.call("remove_motion_study", study=st["id"])

    # The servo's own joint drives it. With the gear pair modelled AS a gear
    # there is only one solution to fall into, so the driver can be the thing
    # the servo actually turns.
    drive = ids["drive pivot L+"]
    study = d.call("add_motion_study", name="open and close", driver=drive,
                   dof="rotation",
                   **{"from": MATE_SIGN * P.THETA_OPEN,
                      "to": MATE_SIGN * P.THETA_CLOSED},
                   samples=a.samples, check_clash=True, clash_mode="exact")
    sid = study.get("id", study)
    print("study:", json.dumps(study)[:300])
    run = d.request("run_motion_study", {"study": sid, "leave_at": "start",
                                        "include_poses": True}, timeout=1800)
    # Does the solved motion match the motion this design was computed for?
    names = {o["id"]: o["name"]
             for o in d.call("list_occurrences")["occurrences"]}
    worst, where = 0.0, None
    for smp in run.get("samples", []):
        want = dict(placements(MATE_SIGN * smp["value"]))
        for pose in smp.get("poses") or []:
            name = names.get(pose["occurrence"])
            if name not in want:
                continue
            err = max(abs(pose["position"][i] - want[name][0][i])
                      for i in range(3))
            if err > worst:
                worst, where = err, (name, round(smp["value"], 2))
    print(f"solved motion vs the computed one: worst position error "
          f"{worst:.4f} mm over {len(run.get('samples', []))} samples"
          f" ({where})")
    keep = {k: run[k] for k in ("clean", "pairs_checked", "worst_sample",
                                "seated_contacts", "appeared", "samples")
            if k in run}
    print("run:", json.dumps(keep)[:700])
    if run.get("table"):
        print(run["table"][:1500])
    if not run.get("clean"):
        raise SystemExit("the open-and-close study is not clean: "
                         f"{json.dumps(run.get('rigid_clashes') or run.get('appeared'))[:400]}")
    # A fresh checkout has no scratch/ tree yet; the export script makes its
    # own directories, and this runs before it.
    os.makedirs(os.path.join(OUT, "renders"), exist_ok=True)
    d.call("export_motion", study=sid, path=f"{OUT}/renders/gripper_cycle.glb")
    print("wrote", f"{OUT}/renders/gripper_cycle.glb")
    d.close()


if __name__ == "__main__":
    main()
