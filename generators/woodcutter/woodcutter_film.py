#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The woodcutter's films (generators/woodcutter/README.md#the-films).

- **The chop**: the crank turned `TURNS` times at the hand's speed
  (`HAND_RPM`, one turn a second), the camera on the figures.
- **The chop in section**: the same turns with the box cut down the axe
  cam's middle (`set_section`), seen from the cut side, so the cam, its rod,
  the link and the arm read as one chain.
- **Going together**: the assembly sequence filmed
  (`export_assembly_video`), each part going home along the way the
  sequence check slid it in.

The two chops play one study of `TURNS` crank turns at a sample every
`STEP_DEG`, filmed at `FPS` with as many frames as the turns take at the
hand's speed, so the film's time is the crank's. A sample every 2 degrees
puts the drop's samples a third of a frame apart (6 degrees a frame at 60
rpm and 60 fps): the drop is a drop, not a blend across one long interval.

Checked, not looked at only: the film's study repeats itself turn for turn
(the crank-turn study, checked for clashes with every part of the box, is
then every turn of the film), the blade comes down to where it rests over
the log at every turn, and the angle it lands at is set beside
check_followers' drop at the hand's speed: the study is kinematic, so its
rod comes down the step face with the cam, sooner than it falls. Stills of
the frames around the first drop are written beside the films.

    python3 generators/woodcutter/woodcutter_film.py --port 8800

films the project build.py saved (`scratch/woodcutter/woodcutter.lob`, its
receipt beside it) and saves it with the film's study; `build.py --film`
films the one it has just built.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import woodcutter_spec as S  # noqa: E402

ASM = "woodcutter automaton"
TURNS = 3
STEP_DEG = 2.0
FPS = 60
WIDTH, HEIGHT = 1280, 720
STUDY = "the film"


def fail(msg):
    raise SystemExit(f"woodcutter film: {msg}")


def by_name(rows, name, what):
    for r in rows:
        if r.get("name") == name:
            return r["id"]
    fail(f"no {what} named '{name}': {[r.get('name') for r in rows]}")


def frames_for(turns):
    """The frames `turns` crank turns take at the hand's speed, both ends
    included: the film's time is the crank's."""
    return int(round(turns * 60.0 / S.HAND_RPM * FPS)) + 1


def frame_at(deg, frames):
    """The frame showing the crank at `deg` (from the study's start)."""
    return deg / (360.0 * TURNS) * (frames - 1)


def film_study(d, s):
    """The study the chops play, run and traced: the blade's low corner
    (as build.py traces it) and the axe rod."""
    occ = {o["name"]: o["id"] for o in d.call("list_occurrences")[
        "occurrences"]}
    crank = by_name(d.call("list_mates")["mates"], "camshaft", "mate")
    for st in d.call("list_motion_studies").get("studies", []):
        if st.get("name") == STUDY:
            d.call("remove_motion_study", study=st["id"])
    n = int(round(360.0 * TURNS / STEP_DEG)) + 1
    st = d.call("add_motion_study", driver=crank, dof="rotation",
                **{"from": 0.0}, to=360.0 * TURNS, samples=n,
                check_clash=False, name=STUDY)
    if st.get("why_not"):
        fail(f"the film's study: {st['why_not']}")
    sid = st["id"]
    x_arm = sum(s["axe_slabs"]["lever"]) / 2.0
    low = s["blade_rest"]
    r = d.request("run_motion_study", {"study": sid, "trace": [
        {"name": "blade", "occurrence": occ["arms"],
         "point_mm": [x_arm, low[0], low[1]]},
        {"name": "rod", "occurrence": occ["axe rod"],
         "point_mm": [0.0, 0.0, 0.0]}]}, timeout=900.0)
    r = r.get("result", r)
    bad = [smp["value"] for smp in r["samples"] if not smp.get("solved")]
    if bad or r.get("branch_jumps"):
        fail(f"the film's study does not solve: unsolved at {bad[:5]}, "
             f"{r.get('branch_jumps')} branch jumps")
    return sid, r


def drop_timing(d, s, sid, run):
    """Where the film's blade lands, against where check_followers says it
    lands at the hand's speed; and the film's turns against each other."""
    vals = [smp["value"] for smp in run["samples"]]
    tr = {t["name"]: t["points_mm"] for t in run["trace"]}
    blade = [p[2] for p in tr["blade"]]
    rod = [p[2] for p in tr["rod"]]
    per = int(round(360.0 / STEP_DEG))
    # Every turn the same: the clash-checked crank turn is every turn here.
    repeat = max(abs(rod[i] - rod[i + per]) for i in range(len(rod) - per))
    repeat_blade = max(abs(blade[i] - blade[i + per])
                       for i in range(len(blade) - per))
    rest = blade[0]
    rel = by_name(d.call("list_relations")["relations"], "axe follower",
                  "relation")
    fr = d.request("check_followers", {"study": sid, "relations": [rel],
                                       "speed_rpm": S.HAND_RPM},
                   timeout=900.0)
    fr = fr.get("result", fr)
    drops = fr["relations"][0]["gravity"].get("drops", [])
    if len(drops) != 1:
        fail(f"the axe's cam has {len(drops)} drops, not one: {drops}")
    dr = drops[0]
    if not dr.get("lands") or dr.get("jams"):
        fail(f"at {S.HAND_RPM:g} rpm the axe does not land in its dwell: "
             f"{dr}")
    tol = 1e-3
    turns = []
    for k in range(TURNS):
        lo = k * 360.0 + dr["at_cam_deg"] - STEP_DEG
        top = next(i for i, v in enumerate(vals) if v >= lo)
        # The film's rod starts down at the first sample under its top, and
        # is home at the first one back at the rest.
        start = next(i for i in range(top, len(vals))
                     if rod[i] < rod[top] - tol)
        home = next(i for i in range(start, len(vals))
                    if abs(blade[i] - rest) < tol)
        turns.append({"top_deg": vals[start - 1], "home_deg": vals[home],
                      "blade_home_mm": blade[home]})
    t0 = turns[0]
    falls = dr["at_cam_deg"] + dr["turn_deg"]
    film_turn = t0["home_deg"] - t0["top_deg"]
    return {
        "speed_rpm": S.HAND_RPM,
        "drop_at_deg": dr["at_cam_deg"], "height_mm": dr["height_mm"],
        "fall_s": dr["fall_s"], "fall_turn_deg": dr["turn_deg"],
        "lands_deg": falls, "dwell_deg": dr["dwell_deg"],
        "drop_rpm": dr["drop_rpm"], "lands": dr["lands"],
        "film": turns, "film_turn_deg": film_turn,
        "film_fall_s": film_turn / (6.0 * S.HAND_RPM),
        "film_early_deg": falls - t0["home_deg"],
        "film_early_s": (falls - t0["home_deg"]) / (6.0 * S.HAND_RPM),
        "blade_rest_mm": rest, "blade_top_mm": max(blade),
        "repeat_mm": max(repeat, repeat_blade)}


def export(d, path, **kw):
    r = d.request("export_motion_video" if "study" in kw
                  else "export_assembly_video",
                  dict(path=str(path), width=WIDTH, height=HEIGHT, fps=FPS,
                       quality="high", **kw), timeout=3600.0)
    return {k: r.get(k) for k in ("path", "frames", "fps", "width", "height",
                                  "bytes", "seconds")}


def stills(video, frames, out, stem):
    """The frames named in `frames` ({label: index}) out of the video, by
    ffmpeg (what the export encoded with). Without one, none."""
    ff = shutil.which("ffmpeg")
    if ff is None:
        return {"skipped": "no ffmpeg on PATH"}
    done = {}
    for label, n in frames.items():
        p = out / f"{stem}_{label}.png"
        subprocess.run([ff, "-v", "error", "-y", "-i", str(video), "-vf",
                        f"select=eq(n\\,{n})", "-frames:v", "1", str(p)],
                       check=True)
        done[label] = {"frame": n, "path": str(p)}
    return done


def films(d, s, out):
    """Every film and its checks. The film's study stays in the project,
    so the Motion window plays the chop at the hand's speed; the caller
    saves it."""
    d.call("set_active_part", part=ASM)
    out.mkdir(parents=True, exist_ok=True)
    W, stage = s["width"], s["stage"]
    sid, run = film_study(d, s)
    timing = drop_timing(d, s, sid, run)
    if timing["repeat_mm"] > 1e-3:
        fail(f"the film's turns differ by {timing['repeat_mm']} mm")
    for t in timing["film"]:
        if abs(t["blade_home_mm"] - s["blade_rest"][1]) > 1e-3:
            fail(f"the blade comes home at {t['blade_home_mm']} mm, not "
                 f"over the log at {s['blade_rest'][1]}")
    frames = frames_for(TURNS)
    res = {"study_samples": len(run["samples"]), "timing": timing}

    # The chop: on the figures, from the open front, a little toward the
    # right wall and above, the axe's whole swing in the frame.
    top = s["arm"]["P"][1] + s["arm_reach"] + s["axe_head"]
    x_mid = (sum(s["axe_slabs"]["body"]) + sum(s["bird_slabs"]["body"])) / 4
    target = [x_mid, 0.0, (stage + top) / 2.0]
    d.call("set_camera", target=target,
           eye=[target[0] + 0.6 * W, 1.5 * W, target[2] + 0.4 * W],
           up=[0, 0, 1])
    v = out / "woodcutter_chop.mp4"
    res["chop"] = export(d, v, study=sid, frames=frames)

    # Its stills: the first drop's frames, and the frame the drop lands on
    # by check_followers at the hand's speed.
    t0 = timing["film"][0]
    marks = {"top": t0["top_deg"],
             "falling": (t0["top_deg"] + t0["home_deg"]) / 2.0,
             "home": t0["home_deg"], "falls": timing["lands_deg"],
             "raised": S.BIRD_PECK_FROM_DEG + S.BIRD_PECK_DEG + 60.0,
             "peck": S.BIRD_PECK_FROM_DEG + S.BIRD_PECK_DEG}
    idx = {k: int(math.ceil(frame_at(a, frames) - 1e-9))
           for k, a in marks.items()}
    res["chop"]["stills"] = stills(v, idx, out, "woodcutter_chop")
    res["chop"]["still_deg"] = {k: frame_at_deg(idx[k], frames)
                                for k in idx}

    # In section, down the axe cam's middle, the crank's half kept and seen
    # from the cut side: the cam, the rod, the link and the arm in one plane.
    cut = d.call("set_section", base_plane="yz", offset=s["x_axe"])
    if not cut.get("keeps", "").startswith("x <="):
        fail(f"the section keeps {cut.get('keeps')}, not the crank's side")
    target = [s["x_axe"], 0.0, top / 2.0]
    d.call("set_camera", target=target,
           eye=[target[0] + 2.8 * W, 0.55 * W, target[2] + 0.45 * W],
           up=[0, 0, 1])
    v = out / "woodcutter_chop_section.mp4"
    try:
        res["section"] = export(d, v, study=sid, frames=frames)
        res["section"]["stills"] = stills(
            v, {k: idx[k] for k in ("top", "falling", "home")}, out,
            "woodcutter_section")
    finally:
        d.call("set_section", enabled=False)

    # Going together, from the front and above, the whole box in the
    # frame.
    d.call("set_camera", target=[W / 2.0, 0.0, top / 2.0],
           eye=[W / 2.0 + 0.9 * W, 1.6 * W, top / 2.0 + 0.9 * W],
           up=[0, 0, 1])
    d.call("fit_view", width=WIDTH, height=HEIGHT)
    res["sequence"] = export(d, out / "woodcutter_sequence.mp4")
    return res


def frame_at_deg(n, frames):
    return n * 360.0 * TURNS / (frames - 1)


def report(res):
    t = res["timing"]
    print(f"  film study: {res['study_samples']} samples over {TURNS} "
          f"turns, every {STEP_DEG:g} deg; the turns repeat to "
          f"{t['repeat_mm']:.1e} mm")
    for k in ("chop", "section", "sequence"):
        r = res[k]
        print(f"  film {k}: {r['frames']} frames, {r['width']} x "
              f"{r['height']} at {r['fps']:g} fps "
              f"({r['frames'] / r['fps']:.2f} s), "
              f"{r['bytes'] / 1e6:.1f} MB in {r['seconds']:.1f} s -> "
              f"{Path(r['path']).name}")
    f0 = t["film"][0]
    print(f"  the drop at {t['speed_rpm']:g} rpm: check_followers has it "
          f"leave the top at {t['drop_at_deg']:.1f} deg and fall "
          f"{t['height_mm']:.1f} mm in {1000 * t['fall_s']:.0f} ms, "
          f"{t['fall_turn_deg']:.1f} deg of turn, landing at "
          f"{t['lands_deg']:.1f} deg within its {t['dwell_deg']:.1f} deg "
          f"dwell; the film's blade leaves the top at "
          f"{f0['top_deg']:g} and is on the log at {f0['home_deg']:g} "
          f"({1000 * t['film_fall_s']:.0f} ms), "
          f"{t['film_early_deg']:.1f} deg "
          f"({1000 * t['film_early_s']:.0f} ms) before it would land")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--out", default=None,
                    help="where build.py wrote woodcutter.lob and its "
                         "receipt (scratch/woodcutter/)")
    args = ap.parse_args()
    from _lib import connect
    from build import OUT
    out = Path(args.out) if args.out else OUT
    receipt = out / "woodcutter.json"
    if not receipt.exists():
        fail(f"no {receipt}: run build.py first")
    s = json.loads(receipt.read_text())["sizes"]
    d = connect(args.port)
    if d.call("ping").get("mode") == "headless":
        fail("a headless instance has no renderer to film with")
    d.call("open_project", path=str(out / "woodcutter.lob"))
    res = films(d, s, out)
    d.call("save_project", path=str(out / "woodcutter.lob"))
    report(res)
    (out / "woodcutter_film.json").write_text(json.dumps(res, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
