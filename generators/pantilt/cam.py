#!/usr/bin/env python3
"""Stage 6: the aluminium base plate's CAM job on the 6090. Saves
stages/cam.dsgn.

    python3 generators/pantilt/cam.py --port N [--shots]

Opens scratch/pantilt/stages/assembly.dsgn (the assembly stage put the bearing
seat's K7 on the plate), makes a job on the base plate from a saw-cut blank,
cuts it in two vise setups, verifies and posts each, and writes the programs
and their setup sheets to scratch/pantilt/nc/ and a report to
scratch/pantilt/cam_report.json.
`--shots` plays each setup back and screenshots it: that needs an instance
that draws toolpaths (tools/headless-preview.sh --gui PORT).

- **Top** (as modelled; the blank in the vise by its lower 8 mm, on
  parallels): face, clear the waste round the disc down to 2 mm over the
  jaws, the bearing seat (rough with the 10 mm, finish with a fresh 6 mm,
  spring pass, both under cutter compensation in wear mode), the relief,
  the four M4 tap holes, chamfers.
- **Underside** (flipped; the vise on the finished Ø73.6 by its top 10 mm):
  face to thickness, clear the rest of the waste (0.5 mm into what Top
  cleared), the 5/16 tap bore and the 3/8-16 UNC-2B thread, chamfers.

The machine is the same 6090 as generators/chess/cam.py. What is assumed
rather than measured is in UNMEASURED, and in the README.
"""
import json
import math
import sys
import time

import common
from common import OUT

# The 6090 as generators/chess/cam.py has it; this job is 3-axis (nothing in
# the rotary). rapid_feed 1500 is the chess job's number, not measured, and
# every feed below is held under it.
MACHINE = {
    "name": "6090", "post": "mach3", "rapid_feed": 1500.0,
    "has_rotary_axis": True, "a_max_deg_min": 3600.0,
    "rotary": {"along": "y", "chuck_end": "-", "word": "A",
               "top_moves": "-x"},
    "holding": {"chuck_diameter_mm": 64.0, "grip_mm": 20.0,
                "tailstock": True, "centre_length_mm": 25.0,
                "centre_diameter_mm": 14.0,
                "tailstock_body_diameter_mm": 34.0, "clearance_mm": 3.0},
    "travel": {"min": [0.0, 0.0, 0.0], "max": [600.0, 900.0, 150.0]},
}

UNMEASURED = [
    "rapid_feed 1500 mm/min and the 600 x 900 x 150 travel are the chess "
    "job's numbers, not read off the machine's Mach3 motor tuning",
    "the spindle is taken as 2.2 kW, 24000 rpm, ER20, usable from about "
    "8000 rpm on its VFD; its runout, which adds straight to a bore's size, "
    "is not measured",
    "the vise is taken as a 100 mm vise with 32 mm jaws, the fixed jaw "
    "trammed along machine X; jaw height and width are not measured",
    "the parallels under the blank in Top must hold it {proud:g} mm proud "
    "of the jaws (gripped by its lower {grip:g} mm); their height is not "
    "measured",
    "the ER20 nuts are taken as 35 mm across and each tool's stickout as "
    "set in TOOLS; measure both, since the holder check reads them",
    "what a bore comes out at on this machine (backlash, steps/mm, frame "
    "stiffness, spindle runout) is not measured: see the K7 note",
]

# Tools for 6061/6082-T6 on a light router. Chip loads are about half of
# what Harvey Tool's and Helical Solutions' 6061 charts give a VMC for these
# diameters (0.04-0.06 mm/tooth at 6 mm, three flutes): a 6090's gantry is
# not a VMC's column. Every feed is held under the machine's 1500 mm/min.
# Holders: ER20 collets, nut 35 mm across; overall_length is the stickout
# from the nut's face to the tip.
ER20_NUT = 35.0
TOOLS = [
    # Face, clear, rough: 10 mm 3-flute carbide for aluminium (polished,
    # 45 deg helix). 15000 rpm (470 m/min) x 3 x 0.03 = 1350 mm/min.
    {"number": 1, "name": "10 mm 3F carbide Al", "diameter": 10.0,
     "tool_type": "flat", "flutes": 3, "flute_length": 22.0,
     "shank_diameter": 10.0, "overall_length": 35.0,
     "collar_diameter": ER20_NUT, "feed_cut": 1350.0, "feed_plunge": 300.0,
     "spindle_rpm": 15000.0, "chipload_min": 0.02, "chipload_max": 0.05},
    # M4 tap holes: 3.3 mm carbide jobber drill, 118 deg -- the point the
    # model's tapped holes are drilled with (a Hole's derived drill point),
    # pecked. 12000 rpm (124 m/min), 0.05 mm/rev = 600 mm/min down.
    {"number": 3, "name": "3.3 mm 118 deg drill", "diameter": 3.3,
     "tool_type": "drill", "tip_angle": 118.0, "flutes": 2,
     "flute_length": 14.0,
     "shank_diameter": 4.0, "overall_length": 25.0,
     "collar_diameter": ER20_NUT, "feed_cut": 600.0, "feed_plunge": 600.0,
     "spindle_rpm": 12000.0, "chipload_min": 0.015, "chipload_max": 0.04},
    # 3/8-16: a 60 deg single-form carbide thread mill whose head goes down
    # the 5/16 (7.94) tap bore: 6 mm head (6 + 2 x 0.86 deep = 7.72 fits),
    # the neck derived (3.6 by 18). 15000 rpm x 3 x 0.02 = 900 at the rim;
    # thread-mill charts run 0.01-0.04 mm/tooth in aluminium.
    {"number": 4, "name": "6 mm 60 deg thread mill", "diameter": 6.0,
     "tool_type": "thread_mill", "tip_angle": 60.0, "flutes": 3,
     "shank_diameter": 6.0, "overall_length": 32.0,
     "collar_diameter": ER20_NUT, "feed_cut": 900.0, "feed_plunge": 300.0,
     "spindle_rpm": 15000.0, "chipload_min": 0.01, "chipload_max": 0.04},
    # Chamfers: 6 mm 90 deg 2-flute carbide chamfer mill. 18000 x 2 x
    # 0.028 = 1000 mm/min; it cuts at about 4 mm, not 6.
    {"number": 5, "name": "6 mm 90 deg chamfer mill", "diameter": 6.0,
     "tool_type": "vbit", "tip_angle": 90.0, "flutes": 2,
     "flute_length": 3.0, "overall_length": 22.0,
     "collar_diameter": ER20_NUT, "feed_cut": 1000.0, "feed_plunge": 300.0,
     "spindle_rpm": 18000.0, "chipload_min": 0.015, "chipload_max": 0.04},
    # The fits: a fresh 6 mm 3-flute that only ever finishes. 20000 rpm
    # x 3 x 0.02 = 1200 mm/min.
    {"number": 6, "name": "6 mm 3F carbide finisher", "diameter": 6.0,
     "tool_type": "flat", "flutes": 3, "flute_length": 18.0,
     "shank_diameter": 6.0, "overall_length": 26.0,
     "collar_diameter": ER20_NUT, "feed_cut": 1200.0, "feed_plunge": 300.0,
     "spindle_rpm": 20000.0, "chipload_min": 0.015, "chipload_max": 0.05},
]

# The blank: saw-cut from standard plate. EN 485 plate thicknesses; the
# thinnest that faces both sides by at least FACE_MIN.
PLATES_MM = [20.0, 25.0, 30.0, 35.0, 40.0]
FACE_MIN = 1.0          # per side, to clean up a rolled face
UNDERSIDE_FACE = 2.0    # what the underside takes; the top takes the rest
SAW_STEP = 5.0          # the blank's side, rounded up to this
GRIP_BLANK = 8.0        # the vise holds the blank by this much (Top)
GRIP_DISC = 10.0        # and the finished disc by this much (Underside)
JAW_CLEAR = 2.0         # Top's clearing stops this far above the jaws
OVERLAP = 0.5           # Underside's clearing runs this far into Top's
JAW = {"w": 15.0, "l": 100.0, "h": 32.0}   # a 100 mm vise's jaw
FACE_STEP = 1.7         # facing depth of cut: two passes off the top
ADAPTIVE_STEP = 0.1     # adaptive engagement: 1 mm of the 10 mm cutter
CLEAR_STEP = 10.0       # and its depth: one cutter diameter a pass
CHAMFER = 0.5           # every edge break (the model's edge_break)
# The check's grid. Its tolerance is a cell and a half, so at the default
# 0.25 a 0.2 mm gouge on the bearing seat's shoulder passed as clean.
VERIFY_RES = 0.1
# The thread the job leaves to a hand tap: the wheel screws' M4.
SCREW_THREAD = "M4"

VERIFY_KEYS = ("ok", "complete", "cuts_nothing", "max_gouge_mm", "gouge_at",
               "max_leftover_mm", "leftover_at", "leftover_area_mm2",
               "rapids_through_stock", "worst_rapid_depth_mm", "over_bites",
               "worst_bite_mm", "shank_collisions", "holder_collisions",
               "fixture_collisions", "worst_fixture_depth_mm",
               "worst_fixture", "fixtures_checked", "below_stock_mm",
               "leftover_other_setups", "failed_ops")


class Job:
    def __init__(self, app, part_name, label):
        self.app, self.label = app, label
        r = app.machine_part(part=part_name, name=label)
        self.job = r.get("job") or r.get("id") or label
        self.said = {}
        self.setups = []

    def setup(self, name, stock, flip=False):
        app = self.app
        sid = app.add_cam_setup(job=self.job, name=name, post="mach3")["id"]
        # By tool: the relief (T1) after the seat's rough would be a
        # fifth manual tool change, each one a Z touch-off.
        kw = {"machine": MACHINE["name"], "order": "tool",
              "held_in": "bed", "stock": stock, "origin_at": "stock_centre"}
        if flip:
            kw["flip"] = True
        app.set_cam_setup(job=self.job, id=sid, **kw)
        for t in TOOLS:
            app.set_cam_setup(job=self.job, id=sid, tool=t)
        self.setups.append((name, sid))
        return sid

    def op(self, sid, label, **kw):
        try:
            r = self.app.add_cam_op(job=self.job, setup=sid, name=label, **kw)
        except Exception as e:  # the refusal IS the finding
            self.said[label] = f"REFUSED: {e}"
            print(f"  !! {label}: refused: {e}")
            return None
        keys = ("type", "tool", "top_z", "bottom_z", "stepdown", "stepover",
                "peck", "finish_tool", "finish_stock", "edge_size", "passes")
        print(f"  {label}: { {k: r.get(k) for k in keys if k in r} }")
        for w in r.get("warnings", []):
            print(f"     warning: {w}")
        self.said[label] = {"op": r, "warnings": r.get("warnings", [])}
        return r

    def run(self, out_stem):
        app, report = self.app, {"ops": self.said, "setups": {}}
        for name, sid in self.setups:
            print(f"== {self.label} / {name}")
            t0 = time.monotonic()
            tp = app.cam_toolpath(job=self.job, setup=sid, _timeout=1800)
            for o in tp.get("operations", []):
                err = o.get("error")
                print(f"  op {o.get('operation')}: moves {o.get('moves')} "
                      f"cut {o.get('cut_length_mm', 0):.0f} mm"
                      f"{' ERROR ' + err if err else ''}"
                      f"{' strategy ' + o['strategy'] if o.get('strategy') else ''}")
            for s in tp.get("skipped", []):
                print(f"  skipped: {s}")
            for w in tp.get("warnings", []):
                print(f"  warning: {w}")
            v = app.cam_verify(job=self.job, setup=sid, resolution=VERIFY_RES,
                               _timeout=1800)
            print("  verify:", {k: v.get(k) for k in VERIFY_KEYS if k in v})
            for th in v.get("threads", []):
                print("  thread:", {k: th.get(k) for k in (
                    "designation", "class", "judged", "why", "in_band",
                    "pitch_diameter_mm", "pitch_diameter_band_mm",
                    "pitch_measured_mm", "hand_measured", "passes",
                    "short_mm", "bore_standing_mm", "neck_clearance_mm")
                    if k in th})
            for f in v.get("fits", []):
                print("  fit:", {k: f.get(k) for k in (
                    "class", "nominal_mm", "band_mm", "aim_mm", "cut_mm",
                    "in_band", "judged", "why") if k in f})
            for u in v.get("uncut", []):
                print("  uncut:", u.get("kind"), u.get("what"),
                      u.get("feature"))
            for reg in v.get("leftover_biggest", [])[:6]:
                print(f"  leftover: {reg}")
            for s in v.get("says", []):
                print(f"  says: {s}")
            nc = f"{out_stem} - {name}.nc"
            try:
                post = app.cam_post(job=self.job, setup=sid, path=nc,
                                    _timeout=1800)
                print(f"  posted {nc.rsplit('/', 1)[-1]}: {post.get('lines')}"
                      f" lines, {post.get('cutting_minutes', 0):.1f} min")
                for w in post.get("warnings", []):
                    print(f"  post warning: {w}")
            except Exception as e:
                post = {"refused": str(e)}
                print(f"  !! post refused: {e}")
            # The page that goes to the machine with the program
            # (docs/cam/setup.md#the-setup-sheet); without a renderer it
            # says it has no picture.
            sheet_path = f"{out_stem} - {name} setup sheet.pdf"
            try:
                sheet = app.call("export_setup_sheet", job=self.job,
                                 setup=sid, path=sheet_path,
                                 program=nc.rsplit("/", 1)[-1],
                                 _timeout=1800)
                print(f"  setup sheet {sheet_path.rsplit('/', 1)[-1]}"
                      f"{'' if sheet.get('picture') else ' (no picture)'}")
            except Exception as e:
                sheet = {"refused": str(e)}
                print(f"  !! setup sheet refused: {e}")
            print(f"  ({time.monotonic() - t0:.1f} s)")
            report["setups"][name] = {"id": sid, "toolpath": tp, "verify": v,
                                      "post": post, "nc": nc,
                                      "sheet": sheet}
        return report


def vise_jaw(app):
    """One jaw of a 100 mm machine vise, in its own coordinates as the bed
    reads it: the gripping face on x = 0, the jaw out along +X, its top at
    z = 0 -- so a fixture's pos is the gripping face's top edge."""
    part = app.part("vise jaw")
    part.activate()
    part.box("Top", width=JAW["w"], depth=JAW["l"], height=JAW["h"],
             at=(0.0, -JAW["l"] / 2), centered=False, reversed=True,
             name="Jaw")
    part.material("steel")
    return part


def blank(lo, hi):
    """The saw-cut blank round the part: its side, plate and margins."""
    across = max(hi[0] - lo[0], hi[1] - lo[1])
    clear = next(t["diameter"] for t in TOOLS if t["number"] == 1)
    side = math.ceil((across + 2 * (clear / 2 + JAW_CLEAR)) / SAW_STEP) \
        * SAW_STEP
    t = hi[2] - lo[2]
    plate = next(p for p in PLATES_MM if p >= t + 2 * FACE_MIN)
    return {"side": side, "plate": plate, "xy": (side - across) / 2,
            "under": UNDERSIDE_FACE, "top": plate - t - UNDERSIDE_FACE,
            "t": t, "across": across}


def jaws(app, job, sid, half_gap, top_z, what):
    """The vise's two jaws, gripping faces half_gap either side of the
    centre along setup X, their tops at top_z."""
    for sign, rot, which in ((1, 0.0, "fixed"), (-1, 180.0, "moving")):
        app.add_job_fixture(job=job, setup=sid, part="vise jaw",
                            pos=[sign * half_gap, 0.0, top_z], rot_z=rot,
                            name=f"{which} jaw on the {what}")


def shots(app, job, setups):
    """Each setup played to its end and screenshotted, then half way."""
    out = []
    for name, sid in setups:
        for at, tag in (("end", "end"), (0.5, "half")):
            app.cam_simulate(job=job, setup=sid, at=at, _timeout=1800)
            path = OUT / f"cam_{name.lower()}_{tag}.png"
            app.screenshot(path=str(path))
            out.append(str(path))
        app.cam_simulate(job=job, setup=sid, close=True)
    return out


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--shots", action="store_true")
    a = ap.parse_args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(common.stage("assembly")))
    plate = app.part("base_plate", create=False)
    tol = [t for t in app.call("list_tolerances")["tolerances"]
           if t.get("class")]
    print("tolerances on the plate:", tol)
    vise_jaw(app)
    app.save_machine(machine=MACHINE)
    clock("open, vise jaw, machine")

    lo, hi = plate.bbox()
    B = blank(lo, hi)
    print("blank:", B)

    job = Job(app, "base_plate", "Base plate")
    J = job.job
    feats = plate.state()["features"]

    def made(name, type_, role):
        """The faces the plate's feature `name` (of `type_`) made in
        `role`, asked by what made them rather than by a name's prefix. Not
        a chamfer's face generated from one of their edges (";G<chamfer>"),
        which the same feature is the origin of."""
        # A template instance of that name (the bearing pocket is one,
        # common.bearing_seat) answers with the feature it expanded into.
        fid = common.template_part(feats, name, type_)
        if len(fid) != 1:
            raise SystemExit(f"base_plate: {len(fid)} {type_} '{name}'")
        out = [f for f in plate.faces(made_by=fid[0], role=role)
               if ";G" not in f.name]
        if not out:
            raise SystemExit(f"base_plate: '{name}' made no {role} face")
        return [{"feature_id": f.ref["feature_id"],
                 "element": f.ref["element"]} for f in out]

    def one(name, type_, role):
        faces = made(name, type_, role)
        if len(faces) != 1:
            raise SystemExit(f"base_plate: '{name}' made {len(faces)} "
                             f"{role} faces, not one")
        return faces[0]

    rim = one("Plate", "Extrude", "side")
    seat = one("Bearing pocket", "Extrude", "side")
    relief = one("Relief", "Extrude", "side")
    tripod = one("Tripod hole", "Hole", "bore")
    # In the hole's own point order (p1..p4), which is the order drilled.
    screws = sorted(made("Wheel screws", "Hole", "bore"),
                    key=lambda f: f["element"])

    # -- Top: the model's own frame, the blank's underside at -B["under"] --
    top = job.setup("Top", {"mode": "relative",
                            "margin": [B["xy"], B["xy"], B["top"]],
                            "bottom_margin": B["under"]})
    jaw_top = lo[2] - B["under"] + GRIP_BLANK
    jaws(app, J, top, B["side"] / 2, jaw_top, "blank")
    job.op(top, "Face", type="face", tool=1, stepdown=FACE_STEP)
    # The waste round the disc, down to just above the jaws -- or as deep as
    # the cutter's flutes reach from the blank's top, a millimetre short, if
    # that is less (the Underside clears the rest): cleared, not profiled,
    # so no frame is left to saw off or to fall into the cutter.
    reach = next(t["flute_length"] for t in TOOLS if t["number"] == 1) - 1.0
    clear_z = max(jaw_top + JAW_CLEAR, hi[2] + B["top"] - reach)
    job.op(top, "Clear round", type="adaptive", tool=1, target=rim,
           bottom_z=clear_z, stepover=ADAPTIVE_STEP, stepdown=CLEAR_STEP)
    # The pan bearing's seat (K7): bored with the 10 mm leaving 0.2 mm,
    # finished with the fresh 6 mm in one full-depth pass, then a spring
    # pass -- the same lap again, to take what the cutter's deflection left.
    # Both finishing walls under cutter compensation, wear mode: the bore
    # gauged before the vise opens, its error typed into T6's diameter in
    # Mach3's tool table, and the finish and spring pass run again
    # (docs/cam/post.md#cutter-compensation).
    job.op(top, "Bearing seat", type="drill", tool=1, target=seat,
           finish_tool=6, finish_stock=0.2, cutter_comp="wear")
    job.op(top, "Bearing seat spring pass", type="profile", tool=6,
           side="inside", target=seat, stepdown=0, cutter_comp="wear")
    job.op(top, "Relief", type="drill", tool=1, target=relief)
    job.op(top, "M4 tap holes", type="drill", tool=3,
           targets=screws)
    job.op(top, "Chamfer seat", type="edge_round", tool=5, target=seat,
           edge_size=CHAMFER)
    job.op(top, "Chamfer top edge", type="edge_round", tool=5,
           target=rim, edge_size=CHAMFER)
    clock("Top setup")

    # -- Underside: flipped about X, so setup z = -(model z) --
    under = job.setup("Underside", {"mode": "relative",
                                    "margin": [B["xy"], B["xy"], B["under"]],
                                    "bottom_margin": B["top"]}, flip=True)
    jaws(app, J, under, B["across"] / 2, -(hi[2] - GRIP_DISC), "disc")
    job.op(under, "Face", type="face", tool=1, stepdown=FACE_STEP)
    job.op(under, "Clear round", type="adaptive", tool=1,
           target=rim, bottom_z=-(clear_z + OVERLAP),
           stepover=ADAPTIVE_STEP, stepdown=CLEAR_STEP)
    job.op(under, "Tripod tap bore", type="drill", tool=6,
           target=tripod)
    job.op(under, "Tripod thread", type="thread_mill", tool=4,
           target=tripod)
    job.op(under, "Chamfer tripod", type="edge_round", tool=5,
           target=tripod, edge_size=CHAMFER)
    job.op(under, "Chamfer bottom edge", type="edge_round", tool=5,
           target=rim, edge_size=CHAMFER)
    clock("Underside setup")

    out = OUT / "nc"
    out.mkdir(exist_ok=True)
    report = job.run(str(out / "base_plate"))
    clock("toolpaths, verify, post")
    report["blank"] = B
    report["tolerances"] = tol
    report["unmeasured"] = [u.format(proud=B["plate"] - GRIP_BLANK,
                                     grip=GRIP_BLANK) for u in UNMEASURED]
    if a.shots:
        report["shots"] = shots(app, J, job.setups)
        clock("playback shots")
    # What the program must do, held by the project as requirements
    # (docs/requirements/README.md): each setup's posted file verified ok
    # and complete at VERIFY_RES, every thread judged in its band, the K7
    # wall in its band, and nothing left uncut but the hand-tapped M4s.
    jid = next(p["id"] for p in app.call("list_parts")["parts"]
               if p["type"] == "job" and p["name"] == job.label)
    for name, sid in job.setups:
        app.require(f"plate CAM: {name} verifies", {
            "kind": "cam_verify", "document": jid, "setup": sid,
            "resolution": VERIFY_RES, "allow_uncut": [SCREW_THREAD]},
            source="the posted program, simulated, cuts the part and only "
                   "the part; the M4s are tapped by hand")
    report["requirements"], bad = common.requirements_met(app)
    clock("requirements")
    common.save(app, "cam")
    (OUT / "cam_report.json").write_text(json.dumps(report, indent=1,
                                                    default=str))
    problems = []
    for label, said in report["ops"].items():
        if isinstance(said, str) and said.startswith("REFUSED"):
            problems.append(f"{label}: {said}")
    problems += [f"requirement {x['name']}: {x['state']}" for x in bad]
    total = 0.0
    for sname, s in report["setups"].items():
        # Safe, complete, the threads and the K7 wall in their bands and
        # nothing uncut but the M4s are the `plate CAM` requirements above.
        # A toolpath warning is a feeds finding (a cutter cutting with its
        # shank): the job is not one to run as it stands.
        for w in s.get("toolpath", {}).get("warnings") or []:
            problems.append(f"{sname}: {w}")
        if "refused" in s.get("post", {}):
            problems.append(f"{sname}: post refused")
        if "refused" in s.get("sheet", {}):
            problems.append(f"{sname}: setup sheet refused")
        total += s.get("post", {}).get("cutting_minutes", 0.0)
    print(f"total cycle time {total:.1f} min")
    if problems:
        print("\nNOT DONE:")
        for p in problems:
            print("  " + p)
        sys.exit(1)
    print("cam: every setup ok and complete")


if __name__ == "__main__":
    main()
