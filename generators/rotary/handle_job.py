#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The handle as a CUTTABLE 4-axis job on the 6090.

`handle.py` makes the part; this makes the program. It is the first
continuous 4-axis job this app has produced, so what it is really for is
finding out what is missing -- see docs/cam/rotary.md#limits for what is
still missing.

The whole program:

  1..3  rough    6 mm ball, stock_to_leave 4.0 / 2.0 / 0.8, coarse stepover
  4     finish   6 mm ball, stock_to_leave 0, fine stepover

Four passes of the SAME strategy at decreasing offsets, because there is no
rotary roughing strategy: `surface_rotary` is a finishing pass, and offsetting
it is the only way to take a Phi 40 dowel down to a Phi 14 waist without
asking a 6 mm ball to remove eleven millimetres of radius in one go. It is
not the same thing as roughing -- a constant-engagement clearing pass keeps
the chip load steady and this keeps only the depth steady -- and that is the
first finding of the round rather than a detail of this script.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), "tools"))

from _lib import connect, fresh_project  # noqa: E402
import handle  # noqa: E402

# Phi 40 oak dowel: the nearest stock size above the Phi 36 swell.
BLANK_R = 20.0
ROUGH = (4.0, 2.0, 0.8)   # mm of stock each roughing pass leaves


def tools(d, sid):
    """One cutter, a 6 mm two-flute ball.

    A ball because a rotary finishing pass reads a SURFACE and the derivation
    refuses a cone for it -- and two flutes for the reason the waveguide job
    learned: the 6090 tops out at 1500 mm/min, so four flutes cannot reach a
    sane chip load at any spindle speed this machine runs.

    50 mm of flute is not what it needs -- the deepest radial cut here is
    about four millimetres -- but the STICKOUT is, because the work is held
    in a chuck on the trunnion and the collet nut has to clear a Phi 40 blank
    turning beside it. Those two numbers are what the holder check reads.
    """
    d.call("set_cam_setup", id=sid, tool={
        "number": 1, "name": "6 mm ball 2F", "diameter": 6.0,
        "tool_type": "ball", "flutes": 2, "flute_length": 25.0,
        "overall_length": 45.0, "collar_diameter": 34.0,
        "feed_cut": 1500.0, "feed_plunge": 400.0, "spindle_rpm": 12000.0})


def build_job(d):
    # Refuse a second run rather than half-building one. The waveguide job
    # claimed a guard here and did not have one: it picked the first PART tab
    # and rebuilt the model into it, which left the first job's targets
    # pointing at element names the rebuild had renamed. Nothing said so --
    # the ops simply produced no toolpath -- so this checks by NAME, which is
    # what `list_parts` addresses tabs by, and says what to do.
    named = {p.get("name") for p in d.call("list_parts")["parts"]}
    if "Handle job" in named or "Handle" in named:
        raise SystemExit(
            "this instance already has a Handle tab: pass --fresh to start "
            "a new project first (or run against a fresh instance) rather "
            "than building a second one into it — a rebuild renames the "
            "elements the first job's operations point at")
    parts = [p for p in d.call("list_parts")["parts"] if p.get("type") == "part"]
    if not parts:
        d.call("add_part")
        parts = [p for p in d.call("list_parts")["parts"]
                 if p.get("type") == "part"]
    d.call("set_active_part", part=parts[0]["id"])
    d.call("rename_part", part=parts[0]["id"], new_name="Handle")
    handle.build(d)
    # Oak, before the job exists: without a material `FeedsFor` reports
    # "nobody looked" and the chip-load half of the check never runs.
    d.call("set_material", body=0, material="wood_oak")

    job = d.call("machine_part")
    if isinstance(job, dict) and "id" in job:
        d.call("rename_part", part=job["id"], new_name="Handle job")
    sid = d.call("add_cam_setup", name="Handle - on the trunnion",
                 post="mach3")["id"]
    d.call("set_cam_setup", id=sid, machine="6090")
    tools(d, sid)
    # A Phi 40 dowel, and nothing over the ends: it is held in a chuck at one
    # end and the other is cut off afterwards.
    d.call("set_cam_setup", id=sid, stock={
        "mode": "relative",
        "margin": [0.0, BLANK_R - handle.SWELL_R, BLANK_R - handle.SWELL_R],
        "bottom_margin": 0.0})
    # Zero on the AXIS. `stock_centre` gives it because the blank is
    # symmetric about the rotary axis in Y and Z -- which is true rather than
    # designed, and is the second finding: there is no origin mode that MEANS
    # "the rotary centreline", so a job whose blank is not centred would have
    # to say it some other way.
    d.call("set_cam_setup", id=sid, origin_at="stock_centre")

    for i, leave in enumerate(ROUGH):
        d.call("add_cam_op", setup=sid, type="surface_rotary", tool=1,
               name=f"Rough {i + 1} (leave {leave})",
               stock_to_leave=leave, stepover=0.5)
    d.call("add_cam_op", setup=sid, type="surface_rotary", tool=1,
           name="Finish", stepover=0.12)
    return sid


def report(d, sid, path):
    r = d.request("cam_post", {"setup": sid, "path": path}, timeout=1800)
    v = d.request("cam_verify", {"setup": sid}, timeout=1800)
    su = d.call("list_cam_setups")["setups"][0]
    travel = su["machine"]["travel"]
    print(f"  part         Phi {handle.SWELL_R * 2:.0f} x {handle.LENGTH:.0f} mm"
          f" from a Phi {BLANK_R * 2:.0f} dowel")
    print(f"  travel       {travel['max'][0] - travel['min'][0]:.0f} mm in X"
          f"  -> {'FITS' if handle.LENGTH <= travel['max'][0] else 'TOO LONG'}")
    secs = 0.0
    feed = {t["number"]: t["feed_cut"] for t in su["tools"]}
    for o in r["operations"]:
        secs += o["cut_length_mm"] / max(1.0, feed.get(o["tool"], 1000.0)) * 60.0
        if o.get("error"):
            print(f"  ! {o['error']}")
    print(f"  cutting      {r['cut_length_mm'] / 1000.0:.1f} m, "
          f"{secs / 60.0:.0f} min at feed")
    print(f"  program      {r['lines']} lines, "
          f"{os.path.getsize(path) / 1024:.0f} kB")
    print(f"  checked as   {'ROTARY r(x, theta)' if v.get('rotary') else '3-AXIS height map'}")
    print(f"  max gouge    {v['max_gouge_mm'] * 1000:.0f} um")
    print(f"  max leftover {v['max_leftover_mm']:.2f} mm")
    print(f"  rapids in    {v['rapids_through_stock']} "
          f"(worst {v['worst_rapid_depth_mm']:.2f} mm)")
    print(f"  holder       {v['holder_collisions']} collisions")
    print(f"  ok           {v['ok']}")
    for line in v.get("says", []):
        print(f"   - {line[:150]}")
    for w in r.get("warnings", []):
        print(f"  ! {w}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--post", default="/tmp/handle.nc")
    ap.add_argument("--save", default=None)
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first, so the instance can be "
                         "reused (refused when its project has unsaved "
                         "changes)")
    a = ap.parse_args()
    d = connect(a.port)
    if a.fresh:
        fresh_project(d)
    sid = build_job(d)
    report(d, sid, a.post)
    if a.save:
        d.call("save_project", path=a.save, message="rotary handle dogfood")
        print(f"  project      {a.save}")


if __name__ == "__main__":
    main()
