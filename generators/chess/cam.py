#!/usr/bin/env python3
"""CAM for the chess set: one job per blank, and the jig and board jobs.

    python3 generators/chess/cam.py --port N [--only blank_pawns ...]

Opens scratch/chess/set.dsgn (run_all.sh builds it) and saves <out>.dsgn with
the jobs. Every blank is turned between the chuck and the tailstock, then its
head details are cut from indexed setups; the board, the base jig and the
pieces' undersides are 3-axis jobs on the bed. The machine's rotary mount (`rotary` below) is the
SAMPLER'S TEST VALUES: the 6090's axis letter and direction are not measured,
so no program posted here is for the machine until they are.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What the run makes -- projects, programs, renders, reports -- is
# regenerable, so it goes to the untracked scratch/, not beside the code.
OUT = HERE.parent.parent / "scratch" / "chess"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(HERE.parent.parent / "tools"))
sys.path.insert(0, str(HERE))
from designer_sdk import connect  # noqa: E402

CHUCK_R = 32.0   # the jaws stand this far out from the axis
TAIL_R = 17.0    # a live centre's body, about 34 across

MACHINE = {
    "name": "6090", "post": "mach3", "rapid_feed": 1500.0,
    "has_rotary_axis": True, "a_max_deg_min": 3600.0,
    # NOT MEASURED -- the sampler's test values (docs/plans/chess-set-dogfood.md,
    # step 0). The rotary lies along machine Y with the chuck at -Y.
    "rotary": {"along": "y", "chuck_end": "-", "word": "A",
               "top_moves": "-x"},
    "holding": {"chuck_diameter_mm": 2 * CHUCK_R, "grip_mm": 20.0,
                "tailstock": True, "centre_length_mm": 25.0,
                "centre_diameter_mm": 14.0,
                "tailstock_body_diameter_mm": 2 * TAIL_R,
                "clearance_mm": 3.0},
    "travel": {"min": [0.0, 0.0, 0.0], "max": [600.0, 900.0, 150.0]},
}
TOOLS = [
    {"number": 1, "name": "6 mm flat 2F", "diameter": 6.0, "tool_type": "flat",
     "flutes": 2, "flute_length": 22.0, "feed_cut": 1200.0,
     "feed_plunge": 300.0, "spindle_rpm": 18000.0},
    {"number": 2, "name": "3 mm ball 2F", "diameter": 3.0, "tool_type": "ball",
     "flutes": 2, "flute_length": 12.0, "shank_diameter": 3.175,
     "feed_cut": 900.0, "feed_plunge": 250.0, "spindle_rpm": 18000.0},
    {"number": 3, "name": "6 mm ball 2F", "diameter": 6.0, "tool_type": "ball",
     "flutes": 2, "flute_length": 20.0, "feed_cut": 1200.0,
     "feed_plunge": 300.0, "spindle_rpm": 18000.0},
    {"number": 4, "name": "4 mm flat 2F", "diameter": 4.0, "tool_type": "flat",
     "flutes": 2, "flute_length": 14.0, "shank_diameter": 6.0,
     "feed_cut": 900.0, "feed_plunge": 250.0, "spindle_rpm": 18000.0},
    {"number": 5, "name": "3 mm flat 2F", "diameter": 3.0, "tool_type": "flat",
     "flutes": 2, "flute_length": 12.0, "shank_diameter": 3.175,
     "feed_cut": 700.0, "feed_plunge": 200.0, "spindle_rpm": 18000.0},
    {"number": 6, "name": "2 mm flat 2F", "diameter": 2.0, "tool_type": "flat",
     "flutes": 2, "flute_length": 8.0, "shank_diameter": 3.175,
     "feed_cut": 500.0, "feed_plunge": 150.0, "spindle_rpm": 20000.0},
    {"number": 8, "name": "90 deg V-bit 12 mm", "diameter": 12.0,
     "tool_type": "vbit", "tip_angle": 90.0, "flutes": 2,
     "flute_length": 6.0, "feed_cut": 1200.0, "feed_plunge": 300.0,
     "spindle_rpm": 16000.0},
    {"number": 7, "name": "1.5 mm ball 2F", "diameter": 1.5,
     "tool_type": "ball", "flutes": 2, "flute_length": 6.0,
     "shank_diameter": 3.175, "feed_cut": 500.0, "feed_plunge": 150.0,
     "spindle_rpm": 20000.0},
]
VERIFY_KEYS = ("ok", "rotary", "max_gouge_mm", "gouge_at", "max_leftover_mm",
               "leftover_p95_mm", "leftover_surface_max_mm",
               "leftover_surface_at", "rapids_through_stock",
               "worst_rapid_depth_mm", "over_bites", "worst_bite_mm",
               "shank_collisions", "holder_collisions", "holding_collisions",
               "worst_holding_depth_mm", "worst_holding", "holding_defaults",
               "leftover_other_setups", "other_setups_not_checked")


class Job:
    def __init__(self, app, part_name, label):
        self.app, self.label = app, label
        r = app.machine_part(part=part_name, name=label)
        self.job = r.get("job") or r.get("id") or label
        self.said = {}
        self.setups = []

    def setup(self, name, a=None, turn=False):
        app = self.app
        sid = app.add_cam_setup(job=self.job, name=name, post="mach3")["id"]
        kw = {"machine": MACHINE["name"], "order": "tool",
              "held_in": "rotary", "tailstock": True,
              "stock": {"mode": "relative", "margin": [0, 0, 0],
                        "bottom_margin": 0}}
        if a is not None:
            kw["rotation_a"] = a
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
        keys = ("type", "top_z", "bottom_z", "stepdown", "stepover",
                "rotary_pattern", "rest_from_tool", "derived")
        print(f"  {label}: { {k: r.get(k) for k in keys if k in r} }")
        for w in r.get("warnings", []):
            print(f"     warning: {w}")
        self.said[label] = {"op": r, "warnings": r.get("warnings", [])}
        return r

    def run(self, out_stem):
        app, report = self.app, {"ops": self.said, "setups": {}}
        for name, sid in self.setups:
            print(f"== {self.label} / {name}")
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
            v = app.cam_verify(job=self.job, setup=sid, _timeout=1800)
            print("  verify:", {k: v.get(k) for k in VERIFY_KEYS if k in v})
            for s in v.get("says", []):
                print(f"  says: {s}")
            nc = f"{out_stem} - {name}.nc"
            try:
                post = app.cam_post(job=self.job, setup=sid, path=nc, _timeout=1800)
                print(f"  posted {Path(nc).name}: {post.get('lines')} lines, "
                      f"{post.get('cutting_minutes', 0):.1f} min")
            except Exception as e:
                post = {"refused": str(e)}
                print(f"  !! post refused: {e}")
            report["setups"][name] = {"id": sid, "toolpath": tp, "verify": v,
                                      "post": post, "nc": nc}
        return report


def faces_of(app, part_name):
    return app.part(part_name, create=False).bodies()[0]


def turn(job, sid, rest=False):
    """Rough and finish the whole turned length. The turned profiles need no
    rest pass: every inside radius is at least 1.6 mm (profiles.FILLET_R),
    so the 3 mm finishing ball reaches all of it. The knight's head does --
    its outline turns tighter than that at the mouth and under the chin."""
    job.op(sid, "Rough", type="surface_rotary", tool=1, stepdown=3.0,
           stock_to_leave=0.5, rotary_pattern="parallel")
    job.op(sid, "Finish", type="surface_rotary", tool=2,
           rotary_pattern="parallel")
    if rest:
        job.op(sid, "Rest", type="surface_rotary", tool=7, rest_from_tool=2)


def body(app, part_name):
    return app.part(part_name, create=False).bodies()[0]


def a_for(normal):
    """The rotary index that turns a face with this outward normal to the
    spindle: A = 0 presents +Z, 90 presents -Y (found by the pocket
    derivation refusing every other pairing), so A = theta - 90 with theta
    the normal's angle from +Y toward +Z."""
    import math
    th = math.degrees(math.atan2(normal[2], normal[1]))
    return round((th - 90.0) % 360.0, 3)


def floors(b, x0, x1, r, tol=0.3):
    """Planar faces facing outward at radius r between x0 and x1."""
    out = []
    for f in b.faces(kind="plane"):
        c, n = f.centroid, f.normal
        if not (x0 < c[0] < x1) or n is None or abs(n[0]) > 0.1:
            continue
        rad = abs(c[1] * n[1] + c[2] * n[2])
        if abs(rad - r) < tol:
            out.append(f)
    return out


def by_a(faces):
    groups = {}
    for f in faces:
        groups.setdefault(a_for(f.normal), []).append(f)
    return groups


def turned(job, rest=False):
    """Rough and finish on the rotary; a rest pass, when there is one, is a
    program of its own (split_output): hours, and for the corners only."""
    sid = job.setup("Turn")
    if rest:
        job.app.set_cam_setup(job=job.job, id=sid, split_output=True)
    turn(job, sid, rest)
    return sid


def pawns(app):
    job = Job(app, "blank_pawns", "Pawns")
    turned(job)
    return job


def rook_bishop(app):
    job = Job(app, "blank_rook_bishop", "Rook and bishop")
    turned(job)
    b = body(app, "blank_rook_bishop")
    x_rook = 14.0
    top = x_rook + 0.58 * 85
    # Crenellations: four notch floors at r = 8, one index each.
    for a, fs in sorted(by_a(floors(b, top - 7, top + 1, 8.0)).items()):
        sid = job.setup(f"Crenel A{a:g}", a=a)
        job.op(sid, "Crenel", type="pocket", tool=4, targets=[
            {"feature_id": f.ref["feature_id"], "element": f.ref["element"]}
            for f in fs])
    # The mitre: a slit through the head, cut half from each side along Y.
    x_b = top + 0.07 * 85
    btop = x_b + 0.74 * 85
    mitre = floors(b, btop - 10, btop - 4, 2.0)
    mitre = [f for f in mitre if f.normal[2] > 0.99]
    assert len(mitre) == 1, [f.name for f in mitre]
    sid = job.setup("Mitre", a=a_for(mitre[0].normal))
    # From the turned head's surface, not the square it was: this setup's
    # stock is the whole blank again (stock is not carried between setups).
    job.op(sid, "Mitre", type="pocket", tool=5, target=mitre[0].ref,
           top_z=10.0)
    return job


def target_list(fs):
    return [{"feature_id": f.ref["feature_id"], "element": f.ref["element"]}
            for f in fs]


def queen_king(app):
    job = Job(app, "blank_queen_king", "Queen and king")
    turned(job)
    b = body(app, "blank_queen_king")
    qtop = 14.0 + 0.89 * 85
    # The crown: eight notch floors at r = 8.8, one index each.
    for a, fs in sorted(by_a(floors(b, qtop - 10.5, qtop - 6, 8.8)).items()):
        sid = job.setup(f"Crown A{a:g}", a=a)
        # the notch floor runs only 2.3 mm along the axis before the dome
        # falls away; that end is OPEN, so a 3 mm cutter clears it
        job.op(sid, "Crown notch", type="pocket", tool=5,
               targets=target_list(fs), top_z=12.5)
    ktop = qtop + 0.07 * 85 + 85
    # The cross: its bar's faces at 2 mm from the axis are the floors -- the
    # bar's top and bottom from above and below, its flats from either side.
    cross = [f for f in floors(b, ktop - 11.5, ktop + 0.5, 2.0)]
    for a, fs in sorted(by_a(cross).items()):
        sid = job.setup(f"Cross A{a:g}", a=a)
        job.op(sid, "Cross", type="pocket", tool=5, targets=target_list(fs),
               top_z=8.0)
    return job


def knights(app):
    job = Job(app, "blank_knights", "Knights")
    turned(job, rest=True)
    # No side setups: the turning passes are 4-axis surfacing all the way
    # round, so they carve the head wherever a radial cutter reaches -- and
    # the check says what, if anything, they could not.
    return job


def bed_setup(job, name, margin=(5.0, 5.0, 0.0)):
    """A 3-axis setup on the bed: stock a margin round the part."""
    app = job.app
    sid = app.add_cam_setup(job=job.job, name=name, post="mach3")["id"]
    app.set_cam_setup(job=job.job, id=sid, machine=MACHINE["name"],
                      held_in="bed", order="tool",
                      stock={"mode": "relative", "margin": list(margin),
                             "bottom_margin": 0})
    for t in TOOLS:
        app.set_cam_setup(job=job.job, id=sid, tool=t)
    job.setups.append((name, sid))
    return sid


def board_job(app):
    job = Job(app, "board", "Board")
    b = body(app, "board")
    top = max(f.centroid[2] for f in b.faces(kind="plane"))
    floors_ = [f for f in b.faces(kind="plane")
               if f.normal and f.normal[2] > 0.99
               and abs(f.centroid[2] - (top - 2.0)) < 0.05]
    walls = []
    for f in floors_:
        # one wall per square: the wall whose outward normal is +X, beside
        # this floor
        c = f.centroid
        w = [g for g in b.faces(kind="plane")
             if g.normal and g.normal[0] < -0.99
             and abs(g.centroid[1] - c[1]) < 20 and 0 < g.centroid[0] - c[0]
             < 30 and g.centroid[2] > top - 2.1]
        if w:
            walls.append(min(w, key=lambda g: g.centroid[0] - c[0]))
    sid = bed_setup(job, "Top")
    job.op(sid, "Dark squares", type="pocket", tool=1,
           targets=target_list(floors_))
    job.op(sid, "Square corners", type="profile", tool=6, side="inside",
           targets=target_list(walls))
    sides = [f for f in b.faces(kind="plane") if f.normal
             and abs(f.normal[2]) < 0.01 and f.centroid[2] < top - 3.5
             and (f.normal[0] < -0.99)]
    job.op(sid, "Frame chamfer", type="edge_round", tool=8,
           target=sides[0].ref)
    job.op(sid, "Cut out", type="profile", tool=1,
           tabs={"count": 8, "width": 8.0, "height": 3.0})
    # After the epoxy has cured proud of the squares: face it back flat.
    sid2 = bed_setup(job, "Face after epoxy", margin=(5.0, 5.0, 0.8))
    job.op(sid2, "Face", type="face", tool=1)
    return job


def jig_job(app):
    job = Job(app, "base_jig", "Base jig")
    b = body(app, "base_jig")
    top = max(b.faces(kind="plane"), key=lambda f: f.centroid[2])
    sid = bed_setup(job, "Plate")
    job.op(sid, "Seats", type="profile", tool=1, side="inside",
           target=top.ref)
    job.op(sid, "Cut out", type="profile", tool=1,
           tabs={"count": 6, "width": 8.0, "height": 3.0})
    return job


def undersides_jig_job(app):
    """One side's sixteen pieces upside down in the base jig, one job: a
    setup per rank (one flat plate seats each rank at its own height, on its
    cove), the other ranks' placements switched off, each piece clamped
    underside up by its own placement."""
    import build_board
    sizes = build_piece_sizes()
    order = [k for k, n in build_board.SIDE.items() for _ in range(n)]
    first = f"{order[0]}_light"
    job = Job(app, first, "Undersides in the jig")
    jparts = [job_part_of(app, job, first)]
    for kind in order[1:]:
        jparts.append(app.add_job_part(job=job.job, part=f"{kind}_light")["id"])
    holes = jig_holes()
    faces = {}
    for kind in build_board.SIDE:
        bk = body(app, f"{kind}_light")
        base_r = sizes[kind][1] / 2 * 85.0
        faces[kind] = (
            [f for f in bk.faces(kind="plane") if f.normal
             and f.normal[0] < -0.99 and abs(f.centroid[0]) < 0.01][0].ref,
            sorted((f for f in bk.faces(kind="plane") if f.normal
                    and f.normal[0] < -0.99
                    and 0.1 < f.centroid[0] < 0.8 * base_r
                    and abs(f.centroid[1]) < 1 and abs(f.centroid[2]) < 1),
                   key=lambda f: f.centroid[0]))
    for kind in build_board.SIDE:
        h = sizes[kind][0] * 85.0
        base_r = sizes[kind][1] / 2 * 85.0
        sid = bed_setup(job, f"Undersides {kind}", margin=(2.0, 2.0, 0.0))
        mine = []
        for (k, hx, hy, _), jp in zip(holes, jparts):
            app.set_job_placement(job=job.job, setup=sid, part=jp,
                                  pos=[hx, hy], lay_face_up=faces[k][0],
                                  enabled=(k == kind))
            if k == kind:
                mine.append(jp)
        # laid underside up, each piece's underside is at z = 0 and it hangs
        # below; it seats on its cove, half a base radius down, so that is
        # where the plate's top goes (its holes are at the placements' x, y)
        app.add_job_fixture(job=job.job, setup=sid, part="base_jig",
                            pos=[0.0, 0.0, -0.5 * base_r - 18.0])
        for label, f in (("Felt recess", faces[kind][1][0]),
                         ("Weight bore", faces[kind][1][-1])):
            job.op(sid, label, type="pocket", tool=4,
                   targets=[{"feature_id": f.ref["feature_id"],
                             "element": f.ref["element"], "part": jp}
                            for jp in mine])
    return job


def job_part_of(app, job, part_name):
    for p in app.list_job_parts(job=job.job)["parts"]:
        if p["name"] == part_name:
            return p["id"]
    raise RuntimeError(f"{part_name} is not in {job.label}")


def jig_holes():
    import build_board
    out, cols = [], []
    for kind, n in build_board.SIDE.items():
        base_r = build_piece_sizes()[kind][1] / 2 * 85.0
        cols += [(kind, base_r)] * n
    pitch_y = 2 * max(r for _, r in cols) + 10
    for ri, row in enumerate((cols[:8], cols[8:])):
        x = 0.0
        for kind, base_r in row:
            x += base_r + 6
            out.append((kind, x, 20 + pitch_y * (ri + 0.5), base_r))
            x += base_r + 6
    return out


def build_piece_sizes():
    import build_piece
    return build_piece.SIZES


JOBS = {"blank_pawns": pawns, "blank_rook_bishop": rook_bishop,
        "blank_queen_king": queen_king, "blank_knights": knights,
        "board": board_job, "base_jig": jig_job,
        "undersides": undersides_jig_job}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--out", default="chess",
                    help="stem of the saved project and the report")
    args = ap.parse_args()
    app = connect(args.port)
    app.call("open_project", path=str(OUT / "set.dsgn"))
    app.save_machine(machine=MACHINE)
    out = OUT / "nc"
    out.mkdir(exist_ok=True)
    reports = {}
    for name, fn in JOBS.items():
        if args.only and name not in args.only:
            continue
        made = fn(app)
        for job in (made if isinstance(made, list) else [made]):
            reports[f"{name}:{job.label}"] = job.run(str(out / job.label))
    app.save_project(path=str(OUT / f"{args.out}.dsgn"))
    (OUT / f"{args.out}_report.json").write_text(
        json.dumps(reports, indent=1, default=str))


if __name__ == "__main__":
    main()
