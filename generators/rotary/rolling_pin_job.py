#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The barley-twist pin as a CUTTABLE 4-axis job on the 6090.

`rolling_pin.py` makes the part; this makes the program.

  1  rough    6 mm ball, stock_to_leave 1.5, coarse stepover
  2  finish   6 mm ball, stock_to_leave 0, fine stepover

Two passes and not four, unlike the handle job: a Phi 54 dowel is only two
millimetres of radius above the pin's own surface, and the flutes are four
more. Nothing here asks a finishing pass to take a big bite.

That is the parallel job, the default. `--spiral` and `--helical` cut the same
pin continuously -- one helix per layer, or passes that follow the flutes --
roughing in 2 mm layers first (docs/cam/rotary.md#patterns-rotary_pattern,
docs/cam/rotary.md#layers-rest-and-the-rough-verb).
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
import rolling_pin as pin  # noqa: E402

# Phi 54 dowel: the nearest stock size above the Phi 50 pin.
BLANK_R = 27.0
ROUGH = (1.5,)   # mm of stock the one roughing pass leaves


def tools(d, sid):
    """One cutter, a 6 mm two-flute ball.

    A ball because a rotary finishing pass reads a SURFACE and the derivation
    refuses a cone for it -- and two flutes for the reason the waveguide job
    learned: the 6090 tops out at 1500 mm/min, so four flutes cannot reach a
    sane chip load at any spindle speed this machine runs.

    The flute length is not what it needs -- the deepest radial cut here is
    six millimetres -- but the STICKOUT is, because the work is held in a
    chuck on the trunnion and the collet nut has to clear a Phi 54 blank
    turning beside it. Those two numbers are what the holder check reads.
    """
    d.call("set_cam_setup", id=sid, tool={
        "number": 1, "name": "6 mm ball 2F", "diameter": 6.0,
        "tool_type": "ball", "flutes": 2, "flute_length": 25.0,
        "overall_length": 45.0, "collar_diameter": 40.0,
        "feed_cut": 1500.0, "feed_plunge": 400.0, "spindle_rpm": 12000.0})


def build_job(d, spiral=False, helical=False):
    # Refuse a second run rather than half-building one. The waveguide job
    # claimed a guard here and did not have one: it picked the first PART tab
    # and rebuilt the model into it, which left the first job's targets
    # pointing at element names the rebuild had renamed. Nothing said so --
    # the ops simply produced no toolpath -- so this checks by NAME, which is
    # what `list_parts` addresses tabs by, and says what to do.
    named = {p.get("name") for p in d.call("list_parts")["parts"]}
    if "Rolling pin job" in named or "Rolling pin" in named:
        raise SystemExit(
            "this instance already has a Rolling pin tab: pass --fresh to start "
            "a new project first (or run against a fresh instance) rather "
            "than building a second one into it — a rebuild renames the "
            "elements the first job's operations point at")
    parts = [p for p in d.call("list_parts")["parts"] if p.get("type") == "part"]
    if not parts:
        d.call("add_part")
        parts = [p for p in d.call("list_parts")["parts"]
                 if p.get("type") == "part"]
    d.call("set_active_part", part=parts[0]["id"])
    d.call("rename_part", part=parts[0]["id"], new_name="Rolling pin")
    pin.build(d)
    # Oak, before the job exists: without a material `FeedsFor` reports
    # "nobody looked" and the chip-load half of the check never runs.
    d.call("set_material", body=0, material="wood_oak")

    job = d.call("machine_part")
    if isinstance(job, dict) and "id" in job:
        d.call("rename_part", part=job["id"], new_name="Rolling pin job")
    sid = d.call("add_cam_setup", name="Rolling pin - on the trunnion",
                 post="mach3")["id"]
    d.call("set_cam_setup", id=sid, machine="6090")
    tools(d, sid)
    # A Phi 54 dowel, and nothing over the ends: it is held in a chuck at one
    # end and parted off afterwards.
    d.call("set_cam_setup", id=sid, stock={
        "mode": "relative",
        "margin": [0.0, BLANK_R - pin.R, BLANK_R - pin.R],
        "bottom_margin": 0.0})
    # Zero on the AXIS. `stock_centre` gives it because the blank is
    # symmetric about the rotary axis in Y and Z -- which is true rather than
    # designed, and is the second finding: there is no origin mode that MEANS
    # "the rotary centreline", so a job whose blank is not centred would have
    # to say it some other way.
    d.call("set_cam_setup", id=sid, origin_at="stock_centre")

    if helical:
        # Passes that FOLLOW the flutes (docs/cam/rotary.md#patterns-rotary_pattern):
        # helices at the twist's own lead -- measured from the surface, so
        # lead_mm stays 0 -- stepped round the pin. EXACTLY the parallel
        # job's operations with only the pattern changed, so the two compare
        # like for like.
        for i, leave in enumerate(ROUGH):
            d.call("add_cam_op", setup=sid, type="surface_rotary", tool=1,
                   name=f"Rough {i + 1} (helical, leave {leave})",
                   rotary_pattern="helical", stock_to_leave=leave,
                   stepover=0.5)
        d.call("add_cam_op", setup=sid, type="surface_rotary", tool=1,
               name="Finish (helical)", rotary_pattern="helical",
               stepover=0.10)
        return sid
    if spiral:
        # Continuous 4-axis (docs/cam/rotary.md#patterns-rotary_pattern): one helix
        # per layer, the part turning while it cuts. The rough comes down in
        # 2 mm layers from the dowel and leaves half a millimetre; the finish
        # follows the flutes' surface in turns 0.6 mm apart.
        d.call("add_cam_op", setup=sid, type="surface_rotary", tool=1,
               name="Rough (spiral, 2 mm layers)", rotary_pattern="spiral",
               stepdown=2.0, stock_to_leave=0.5, stepover=0.5)
        d.call("add_cam_op", setup=sid, type="surface_rotary", tool=1,
               name="Finish (spiral)", rotary_pattern="spiral", stepover=0.10)
        return sid
    for i, leave in enumerate(ROUGH):
        d.call("add_cam_op", setup=sid, type="surface_rotary", tool=1,
               name=f"Rough {i + 1} (leave {leave})",
               stock_to_leave=leave, stepover=0.5)
    # 0.10 of a 6 mm ball is 0.6 mm of arc, which leaves a 15 um scallop
    # between passes -- sandpaper's job to remove, not a second operation's.
    d.call("add_cam_op", setup=sid, type="surface_rotary", tool=1,
           name="Finish", stepover=0.10)
    return sid


def report(d, sid, path):
    r = d.request("cam_post", {"setup": sid, "path": path}, timeout=1800)
    v = d.request("cam_verify", {"setup": sid}, timeout=1800)
    su = d.call("list_cam_setups")["setups"][0]
    travel = su["machine"]["travel"]
    print(f"  part         Phi {pin.R * 2:.0f} x {pin.LENGTH:.0f} mm"
          f" from a Phi {BLANK_R * 2:.0f} dowel")
    print(f"  travel       {travel['max'][0] - travel['min'][0]:.0f} mm in X"
          f"  -> {'FITS' if pin.LENGTH <= travel['max'][0] else 'TOO LONG'}")
    secs = 0.0
    feed = {t["number"]: t["feed_cut"] for t in su["tools"]}
    for o in r["operations"]:
        secs += o["cut_length_mm"] / max(1.0, feed.get(o["tool"], 1000.0)) * 60.0
        if o.get("error"):
            print(f"  ! {o['error']}")
        if o.get("strategy"):
            print(f"  ~ op {o.get('id', '?')}: cut as {o['strategy']}")
    print(f"  cutting      {r['cut_length_mm'] / 1000.0:.1f} m, "
          f"{secs / 60.0:.0f} min at feed, "
          f"{r.get('cutting_minutes', 0.0):.0f} min as the controller runs it")
    print(f"  program      {r['lines']} lines, "
          f"{os.path.getsize(path) / 1024:.0f} kB")
    print(f"  checked as   {'ROTARY r(x, theta)' if v.get('rotary') else '3-AXIS height map'}")
    print(f"  max gouge    {v['max_gouge_mm'] * 1000:.0f} um")
    print(f"  max leftover {v['max_leftover_mm']:.2f} mm")
    if "leftover_p95_mm" in v:
        print(f"  p95 leftover {v['leftover_p95_mm'] * 1000:.0f} um "
              f"(the typical scallop)")
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
    ap.add_argument("--post", default="/tmp/rolling_pin.nc")
    ap.add_argument("--save", default=None)
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first, so the instance can be "
                         "reused (refused when its project has unsaved "
                         "changes)")
    ap.add_argument("--spiral", action="store_true",
                    help="continuous 4-axis: spiral rough in layers + spiral "
                         "finish, instead of parallel passes")
    ap.add_argument("--helical", action="store_true",
                    help="continuous 4-axis that FOLLOWS the twist: helical "
                         "rough in layers + helical finish")
    a = ap.parse_args()
    d = connect(a.port)
    if a.fresh:
        fresh_project(d)
    sid = build_job(d, spiral=a.spiral, helical=a.helical)
    report(d, sid, a.post)
    if a.save:
        d.call("save_project", path=a.save, message="barley-twist rolling pin")
        print(f"  project      {a.save}")


if __name__ == "__main__":
    main()
