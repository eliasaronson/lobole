#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The waveguide as a CUTTABLE job on the 6090 router.

`waveguide.py` makes the part; this makes the program. It exists because
"can I cut this?" is a question about the machine, not the model, and the
answers are all numbers: does it fit the travel, can the spindle reach a
sane chip load at the feed ceiling, and how long will it run.

The whole program, in the order a router wants it, in two setups:

  front, the dish up
  1. rough the dish        6 mm flat, levels, 0.3 mm left on
  2. finish the flare      6 mm ball, raster -- the shallow majority
  3. drill the bolt circle 5 x 3.4 mm, while the blank is still solid
  4. cut the disc free     6 mm flat outside the rim, tabs
  back, the sheet turned over with the disc still on its tabs
  5. free the throat       6 mm flat inside the back face's opening

3 before 4 because a blank that is still one piece cannot move. 4 leaves
tabs because the disc is the part, and they are what keeps it in the sheet
when the sheet is turned over.

The throat has NO cylindrical wall to follow -- the flare runs all the way
down to r=50 -- so there is nothing to profile in the usual sense. Its
opening is the INNER WIRE of the back face, and the back face points down
on the front setup, where the spindle cannot reach it and the app refuses
it as a pick. Turned over, it faces the spindle, and an inside profile on it
cuts exactly its opening. That is the whole trick of this job.

Each setup posts its own program: `--post` is the front, and the back goes
beside it as `<stem>-back<ext>`. The script fails when the verifier finds
either one unsafe.
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

    32 mm of flute, not 25. The part is 25 mm thick and the profiles cut
    through it, so a 25 mm cutter would finish the job with its collet nut
    exactly on the stock surface -- and stock thickness and Z zero are each
    good to a few tenths, which add. The app says so now; it did not when
    this job was first written, because the reach check only fired when the
    cut was DEEPER than the flute.
    """
    for t in ({"number": 1, "name": "6 mm flat 2F", "diameter": 6.0,
               "tool_type": "flat", "flutes": 2, "flute_length": 32.0,
               "feed_cut": 1500.0, "feed_plunge": 400.0,
               "spindle_rpm": 6000.0},
              {"number": 2, "name": "6 mm ball 2F", "diameter": 6.0,
               "tool_type": "ball", "flutes": 2, "flute_length": 32.0,
               "feed_cut": 1500.0, "feed_plunge": 400.0,
               "spindle_rpm": 6000.0},
              {"number": 3, "name": "3.4 mm drill", "diameter": 3.4,
               "tool_type": "drill", "tip_angle": 118.0, "flutes": 2,
               "flute_length": 30.0, "feed_cut": 400.0, "feed_plunge": 200.0,
               "spindle_rpm": 6000.0}):
        d.call("set_cam_setup", id=sid, tool=t)


def face_by(body, want, radius=None, area=None):
    """A face of the part, by shape and size. The names are toponaming ids
    (`2/g5;M4`) which say nothing to a reader, so pick by the geometry and
    let the assertion fail loudly if the model moves under us."""
    hits = [f for f in body["faces"]
            if f.get("kind") == want
            and (radius is None or abs(f.get("radius", -1) - radius) < 0.01)
            and (area is None or abs(f.get("area", -1) - area) / area < 0.05)]
    if not hits:
        raise SystemExit(f"no {want} face with r={radius} area={area}; the "
                         f"model changed and this job's picks are stale")
    return hits


def build_job(d):
    # A part tab first. Run twice against one instance and the second run
    # starts on the JOB this one made, where add_feature refuses -- which is
    # right, and is the script's job to handle rather than the user's.
    parts = [p for p in d.call("list_parts")["parts"]
             if p.get("type") == "part"]
    if not parts:
        d.call("add_part")
        parts = [p for p in d.call("list_parts")["parts"]
                 if p.get("type") == "part"]
    d.call("set_active_part", part=parts[0]["id"])
    # Named, because this project is meant to be OPENED. "Part 1" and
    # "Part 1 Job" tell a reader nothing about what they are looking at.
    d.call("rename_part", part=parts[0]["id"], new_name="Waveguide")
    waveguide.build(d)
    body = d.call("list_elements")["bodies"][0]
    fid = body["created_by"]

    def target(face):
        return {"feature_id": fid, "element": face["name"]}

    # By geometry, not by name: the throat opening is the inner wire of the
    # back face (the big annulus), the rim is the r=100 cylinder, and the
    # bolt holes are the five r=1.7 cylinders.
    back = face_by(body, "plane", area=23516.5)[0]
    flare = face_by(body, "torus", radius=47.78)[0]
    rim = face_by(body, "cylinder", radius=100.0)[0]
    bolts = face_by(body, "cylinder", radius=waveguide.BOLT_D / 2.0)
    if len(bolts) != waveguide.BOLT_N:
        raise SystemExit(f"expected {waveguide.BOLT_N} bolt holes, found "
                         f"{len(bolts)}")

    # MDF, before the job exists. Without it `FeedsFor` reports "nobody
    # looked" and the chip-load half of the check never runs -- which is how
    # this job ran for a whole round with its feeds unexamined.
    d.call("set_material", body=0, material="mdf")
    job = d.call("machine_part")
    if isinstance(job, dict) and "id" in job:
        d.call("rename_part", part=job["id"], new_name="Waveguide job")
    sid = setup(d, "Waveguide - front")
    top = d.call("list_cam_setups")["setups"][0]["stock"]["box"]["max"][2]

    d.call("add_cam_op", setup=sid, type="adaptive", tool=1, name="Rough",
           top_z=top, bottom_z=0.0, stepdown=4.0, stock_to_leave=ROUGH_LEAVE)
    # Targeted at the FLARE, not left to the whole-part silhouette. With no
    # pick the raster covers the blank out to its corners, and the retract at
    # the end of every pass near the rim climbs with the ball's rim overhanging
    # the uncut 25 mm waste outside r=100 -- 342 rubs the verifier is right to
    # flag. The flare's own silhouette keeps the pass inside the dish the
    # roughing already cleared, which is what the pick is FOR.
    d.call("add_cam_op", setup=sid, type="surface_parallel", tool=2,
           name="Finish the flare", targets=[target(flare)],
           stepover=BALL_STEPOVER, cut_angle_deg=0.0)
    # Derived, not typed: every depth comes from the face that was picked.
    # `add_cam_op` with targets does the derivation AND adds the op --
    # `cam_derive_op` is the dry run that answers "what would this pick make"
    # and deliberately adds nothing.
    d.call("add_cam_op", setup=sid, type="drill", tool=3,
           name="Drill the bolt circle",
           targets=[target(f) for f in bolts])
    # Tabs: the disc is the part, and the sheet is turned over with it still
    # hanging in it.
    d.call("add_cam_op", setup=sid, type="profile", tool=1,
           name="Cut the disc free", targets=[target(rim)],
           tabs={"count": 4, "width": 8.0, "height": 2.0})

    # The throat, from the BACK. The throat has no wall to follow -- the flare
    # runs all the way down to r=50 -- so the opening is named by the back
    # face's inner wire, and the back face points DOWN on the front setup:
    # the spindle cannot reach it, and the app refuses it as a pick. This op
    # used to pick it anyway, with no side, and was derived as a cut OUTSIDE
    # the face -- a second pass round the rim, named for the throat. The
    # throat was never cut: the ball cannot follow the flare into the r=50
    # edge, and the verifier finds the lip it leaves (2.8 mm at r=49.9).
    #
    # So the sheet is turned over, half a turn about X with the disc still
    # on its tabs, and the back face is UP. Side `inside` on it cuts its
    # openings -- the throat -- and nothing else; the bolt holes are the
    # drill's and are left alone. No tabs: what drops out of an opening is
    # scrap, and a roughed throat leaves only a skin there.
    back_sid = setup(d, "Waveguide - back", flip=True)
    d.call("add_cam_op", setup=back_sid, type="profile", tool=1,
           name="Free the throat", targets=[target(back)], side="inside",
           drilled_bores=waveguide.BOLT_D)
    return [sid, back_sid]


def setup(d, name, flip=False):
    """One clamping: the 6090, its cutters, and the 210 mm blank."""
    sid = d.call("add_cam_setup", name=name, post="mach3")["id"]
    # By NAME, from ~/.config/lobole/machines/. Headless sessions used to
    # have no machine store at all -- RunHeadless builds its own AppState and
    # main() had loaded the machines into the GUI's -- so this refused, and
    # every job reported `machine_match: missing` against a machine plainly on
    # disk (docs/cam/setup.md#machine-profiles).
    kw = {"machine": "6090"}
    if flip:
        kw["flip"] = True
    d.call("set_cam_setup", id=sid, **kw)
    tools(d, sid)
    # The blank: the part plus a hand's-width of margin, and NO bottom margin
    # -- it is screwed to the spoilboard and the profile cuts 0.5 mm into it.
    d.call("set_cam_setup", id=sid,
           stock={"mode": "relative", "margin": [MARGIN, MARGIN, 0.0],
                  "bottom_margin": 0.0})
    return sid


def report(d, sid, path):
    """Post and verify one setup; False when the verifier finds it wrong."""
    # A long timeout, explicitly. Generating and posting a 3D finishing pass
    # is minutes of work, and the client's 60 s default kills the connection
    # while the server carries on -- which reads as a hang followed by a
    # mystery (designer_remote.py says so for mesh_study; a surfacing job is
    # the same class of call).
    r = d.request("cam_post", {"setup": sid, "path": path}, timeout=900)
    v = d.request("cam_verify", {"setup": sid}, timeout=900)
    su = next(s for s in d.call("list_cam_setups")["setups"]
              if s["id"] == sid)
    box = su["stock"]["box"]
    travel = su["machine"]["travel"]

    print(f"== {su['name']}")
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
    print(f"  program      {path}: {r['lines']} lines, "
          f"{os.path.getsize(path) / 1024:.0f} kB")
    print(f"  max gouge    {v['max_gouge_mm'] * 1000:.0f} um")
    for w in r.get("warnings", []):
        print(f"  ! {w}")
    for w in v.get("says", []):
        print(f"  verify: {w}")
    print(f"  verdict      {'ok' if v['ok'] else 'FAILED'}")
    return bool(v["ok"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--post", default="/tmp/waveguide.nc")
    ap.add_argument("--save", default=None,
                    help="also save the PROJECT here, so the part, the job "
                         "and its two setups can be opened and looked at "
                         "rather than only the program read")
    a = ap.parse_args()
    d = connect(a.port)
    sids = build_job(d)
    # One program per setup: the front as asked, the back beside it.
    stem, ext = os.path.splitext(a.post)
    paths = [a.post] + [f"{stem}-back{ext}"]
    ok = all([report(d, sid, path) for sid, path in zip(sids, paths)])
    if a.save:
        # The program alone is a dead end for a person: it says what the
        # machine will do and nothing about why. Save the project too.
        r = d.call("save_project", path=a.save, message="waveguide dogfood")
        print(f"  project      {a.save}")
        if isinstance(r, dict) and r.get("warnings"):
            for w in r["warnings"]:
                print(f"  ! {w}")
    if not ok:
        raise SystemExit("the verifier found the program wrong -- see above")


if __name__ == "__main__":
    main()
