#!/usr/bin/env python3
"""The waveguide as a CUTTABLE job on the 6090 router.

`waveguide.py` makes the part; this makes the program. It exists because
"can I cut this?" is a question about the machine, not the model, and the
answers are all numbers: does it fit the travel, can the spindle reach a
sane chip load at the feed ceiling, and how long will it run.

What it builds, which is the 3D half and the reason the part exists:

  1. rough the dish            6 mm flat, levels, 0.3 mm left on
  2. finish the flare          6 mm ball, raster -- the shallow majority

NOT YET, and the program is not cuttable without them: bore the throat,
drill the bolt circle, cut the disc free with tabs. All three are 2.5D and
all three need a face PICKED by element name, which is the one thing this
script cannot do blind -- `cam_verify` says so honestly, reporting 25 mm of
material left over 2844 mm2 where the throat slug and the outside waste
still stand.

Run:  python3 generators/waveguide_job.py --port N [--post PATH]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))

from _lib import connect  # noqa: E402
import waveguide  # noqa: E402

MARGIN = 5.0          # mm of blank round the part: a 210 square of 25 mm MDF
ROUGH_LEAVE = 0.3     # mm for the ball to take off
BALL_STEPOVER = 0.15  # fraction of the cutter: 0.9 mm, a 34 um scallop


def tools(d, sid):
    """Two cutters, both TWO-flute.

    Four flutes is what the speaker baffle used and it is why that job could
    not reach a sane chip load: the 6090 tops out at 1500 mm/min, and
    1500 / (4 flutes x 6000 rpm) is 0.06 mm/tooth against a 0.12-0.24 band.
    Two flutes halves the rpm needed, which is the only lever left once the
    feed is at the ceiling.
    """
    for t in ({"number": 1, "name": "6 mm flat 2F", "diameter": 6.0,
               "tool_type": "flat", "flutes": 2, "flute_length": 25.0,
               "feed_cut": 1500.0, "feed_plunge": 400.0,
               "spindle_rpm": 7000.0},
              {"number": 2, "name": "6 mm ball 2F", "diameter": 6.0,
               "tool_type": "ball", "flutes": 2, "flute_length": 25.0,
               "feed_cut": 1500.0, "feed_plunge": 400.0,
               "spindle_rpm": 7000.0}):
        d.call("set_cam_setup", id=sid, tool=t)


def build_job(d):
    waveguide.build(d)
    d.call("machine_part")
    sid = d.call("add_cam_setup", name="Waveguide - one side", post="mach3")["id"]
    # By NAME, from ~/.config/designer/machines/. Headless sessions used to
    # have no machine store at all -- RunHeadless builds its own AppState and
    # main() had loaded the machines into the GUI's -- so this refused, and
    # every job reported `machine_match: missing` against a machine plainly on
    # disk (docs/cam/dogfood-waveguide.md).
    d.call("set_cam_setup", id=sid, machine="6090")
    tools(d, sid)
    # The blank: the part plus a hand's-width of margin, and NO bottom margin
    # -- it is screwed to the spoilboard and the profile cuts 0.5 mm into it.
    d.call("set_cam_setup", id=sid,
           stock={"mode": "relative", "margin": [MARGIN, MARGIN, 0.0],
                  "bottom_margin": 0.0})
    top = d.call("list_cam_setups")["setups"][0]["stock"]["box"]["max"][2]

    d.call("add_cam_op", setup=sid, type="adaptive", tool=1, name="Rough",
           top_z=top, bottom_z=0.0, stepdown=4.0, stock_to_leave=ROUGH_LEAVE)
    d.call("add_cam_op", setup=sid, type="surface_parallel", tool=2,
           name="Finish the flare", top_z=top, bottom_z=0.0,
           stepover=BALL_STEPOVER, cut_angle_deg=0.0)
    return sid


def report(d, sid, path):
    r = d.call("cam_post", setup=sid, path=path)
    v = d.call("cam_verify", setup=sid)
    su = d.call("list_cam_setups")["setups"][0]
    box = su["stock"]["box"]
    travel = su["machine"]["travel"]

    size = [round(box["max"][i] - box["min"][i], 1) for i in range(3)]
    room = [round(travel["max"][i] - travel["min"][i], 1) for i in range(3)]
    print(f"  blank        {size[0]} x {size[1]} x {size[2]} mm")
    print(f"  travel       {room[0]} x {room[1]} x {room[2]} mm  -> "
          f"{'FITS' if all(s <= t for s, t in zip(size, room)) else 'DOES NOT FIT'}")
    print(f"  outside travel moves: {v['outside_travel']}")

    # Time, from the cut length and each op's own feed. Rapids are counted at
    # the machine's rapid feed, which on this router is the same 1500.
    secs = 0.0
    tool_feed = {t["number"]: t["feed_cut"] for t in su["tools"]}
    for o in r["operations"]:
        secs += o["cut_length_mm"] / max(1.0, tool_feed.get(o["tool"], 1000.0)) * 60.0
    print(f"  cutting      {r['cut_length_mm'] / 1000.0:.1f} m, "
          f"{secs / 60.0:.0f} min at feed (rapids and tool changes on top)")
    print(f"  program      {r['lines']} lines, {os.path.getsize(path) / 1024:.0f} kB")
    print(f"  max gouge    {v['max_gouge_mm'] * 1000:.0f} um")
    for w in r.get("warnings", []):
        print(f"  ! {w}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--post", default="/tmp/waveguide.nc")
    a = ap.parse_args()
    d = connect(a.port)
    sid = build_job(d)
    report(d, sid, a.post)


if __name__ == "__main__":
    main()
