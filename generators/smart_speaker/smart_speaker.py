#!/usr/bin/env python3
"""The smart_speaker cabinet, built from its parameters.

    python3 generators/smart_speaker/smart_speaker.py [--port N]
            [--out scratch/smart_speaker.dsgn]
            [--stage carcass|parts|assembly|cam|all] [--no-save]

A two-way floorstander: a Visaton W 170 S in an 854 mm reflex chamber, a
Markaudio Alpair 7 MS in a 120 mm sealed chamber above it, a flared port down
the back of the bay under both, and an ESP32 amplifier beside the port. Six
MDF panels, two braces and the port are made here; the drivers, the amplifier
and the power brick are bought and arrive as STEP (`extract_bought.py`).

This replaces an earlier port of the original model's STEP export that
brought over the six panels and stopped.
What it is for, and what it fixes about that port, is
[README.md](README.md#structure). The short version:
one parameter table instead of six copies of one, driver heights derived from
the floor and the top rather than typed into a sketch, bolt rings that are a
count and a diameter, and the parts that were missing.

The run refuses to save what it has not checked — see `verify.py`.

`--stage cam` adds the baffle's job on the 6090 — see cam.py, whose targets
are picked by geometry rather than by the element strings a ported job
carries.
"""
import argparse
import math
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent.parent / "tools"))

from designer_sdk import connect  # noqa: E402

import bought  # noqa: E402
import braces  # noqa: E402
import hardware  # noqa: E402
import mates  # noqa: E402
import params  # noqa: E402
import port as port_mod  # noqa: E402
import veneer  # noqa: E402
import verify  # noqa: E402
import walls  # noqa: E402

DEFAULT_OUT = HERE.parent.parent / "scratch/smart_speaker.dsgn"
PHASES = []


class phase:
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        self.at = time.perf_counter()
        return self

    def __exit__(self, *exc):
        PHASES.append((self.name, time.perf_counter() - self.at))


def report_phases():
    total = sum(d for _, d in PHASES) or 1.0
    print(f"\ntime: {total:.1f} s")
    for name, seconds in PHASES:
        print(f"  {name:<16} {seconds:6.1f} s {100 * seconds / total:4.0f}%")


def build_parts(app, with_bought=True):
    """Every part, and the parameter table they all read.

    The table is declared on the first part but at PROJECT scope, so there is
    one of each name for the whole document — see params.py on why that
    matters here specifically.
    """
    print("parameters:")
    seed = app.part("side_wall")
    p = params.declare(seed)
    print(f"  {len(p)} at project scope; "
          f"{len(params.DROPPED)} from the port dropped "
          f"({', '.join(params.DROPPED)})")

    made = {}

    def add(fn, *a):
        part = fn(app, p, *a)
        lo, hi = part.bbox()
        made[part.name] = part
        print(f"  {part.name:<22} {hi[0]-lo[0]:7.1f} x {hi[1]-lo[1]:7.1f} x "
              f"{hi[2]-lo[2]:7.1f} mm   {part.volume()/1000:9.1f} cm3")
        return part

    print("panels:")
    add(walls.side_wall)
    add(walls.front_wall)
    add(walls.back_wall)
    add(walls.top_wall)
    add(walls.bottom_wall)
    add(walls.divider)
    print("plywood coverings (derived from the walls):")
    for part in veneer.build_all(app, p, made):
        lo, hi = part.bbox()
        made[part.name] = part
        print(f"  {part.name:<22} {hi[0]-lo[0]:7.1f} x {hi[1]-lo[1]:7.1f} x "
              f"{hi[2]-lo[2]:7.1f} mm   {part.volume()/1000:9.1f} cm3")

    print("braces, port, hardware:")
    add(braces.ring_brace)
    add(braces.ladder)
    add(port_mod.tube)
    add(hardware.t_nut)
    add(hardware.dowel)
    if not with_bought:
        print("bought: skipped")
        _file_tabs(app)
        return p, made
    print("bought:")
    for part in bought.import_all(app, p):
        lo, hi = part.bbox()
        made[part.name] = part
        print(f"  {part.name:<22} {hi[0]-lo[0]:7.1f} x {hi[1]-lo[1]:7.1f} x "
              f"{hi[2]-lo[2]:7.1f} mm")
    _file_tabs(app)
    return p, made


# Where each tab is filed. Display grouping only — ids, references and
# storage are untouched — but a project with thirty tabs in one row is not
# one anybody reads.
FOLDERS = {
    "Cabinet": ["side_wall", "front_wall", "back_wall", "top_wall",
                "bottom_wall", "divider"],
    "Coverings": ["veneer_side", "veneer_front", "veneer_back", "veneer_top"],
    "Bracing": ["ring_brace", "brace_ladder"],
    "Port": ["port_tube", "port_flare_top", "port_flare_bottom"],
    "Hardware": ["t_nut", "dowel"],
    "Bought": ["W170S woofer", "Alpair 7MS", "amp module", "PSU brick"],
}


def _file_tabs(app):
    have = {r["name"] for r in app.list_parts()["parts"]}
    for folder, members in FOLDERS.items():
        inside = [m for m in members if m in have]
        if not inside:
            continue
        app.create_folder(folder=folder)
        app.move_to_folder(part=inside, folder=folder)
    # `new_project` opens with an empty "Part 1" and nothing here ever uses
    # it. Left alone it ships as a part of the loudspeaker.
    if "Part 1" in have:
        app.delete_part(part="Part 1")
    print(f"  filed into {len(FOLDERS)} folders; empty 'Part 1' removed")


def build_assembly(app, p, made):
    print("assembly:")
    asm = app.assembly("speaker")
    asm.activate()
    placed = []

    def place(part_name, name, at, orientation=None, fixed=False):
        kw = {"part": part_name, "fixed": fixed, "name": name,
              "transform": {"position": [float(v) for v in at],
                            "orientation": list(orientation or (0, 0, 0, 1))}}
        r = app.add_occurrence(**kw)
        placed.append((name, r))
        return r

    for part_name, name, at, quat in walls.placements(p):
        place(part_name, name, at, quat, fixed=(name == "side_wall L"))
    for row in braces.placements(p):
        place(*row[:3], row[3])
    for row in port_mod.placements(p):
        place(*row[:3], row[3])
    for row in hardware.placements(p):
        place(*row[:3], row[3])
    for row in veneer.placements(p):
        place(*row[:3], row[3])
    for row in bought.placements(p):
        place(*row[:3], row[3])
    print(f"  {len(placed)} occurrences")
    added = mates.build(app, asm, p, made)
    asm.solve()
    print(f"  {added} mates")
    return asm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--stage", default="all",
                    choices=["carcass", "parts", "assembly", "cam", "all"])
    ap.add_argument("--skip-cam", action="store_true")
    ap.add_argument("--no-save", action="store_true")
    ap.add_argument("--deep", action="store_true",
                    help="compare against the reference STEPs by SHAPE "
                         "(iou/dice, three OCC booleans — 22 s of the run) "
                         "rather than by size and census")
    ap.add_argument("--save-anyway", action="store_true",
                    help="save even though the checks failed — for LOOKING at "
                         "what failed. The document is not to be trusted.")
    args = ap.parse_args()

    app = connect(args.port)
    app.new()

    # `carcass` is what CI can run: the panels, coverings, braces, port and
    # hardware, and none of the bought STEP — those live in scratch/, which
    # is not tracked, because the projects here are regenerable and the
    # slices come back from `extract_bought.py`.
    with phase("parts"):
        p, made = build_parts(app, with_bought=(args.stage != "carcass"))
    if args.stage in ("carcass", "parts"):
        return _finish(app, args, None, p, made)

    with phase("assembly"):
        asm = build_assembly(app, p, made)
    if args.stage == "assembly":
        return _finish(app, args, asm, p, made)

    if not args.skip_cam:
        with phase("cam"):
            import cam
            _job, sid = cam.build(app, p)
            ok, _v = cam.verify(app, sid)
            if not ok:
                print("  !! the job does not cut what it should")
            elif not args.no_save:
                cam.post(app, sid,
                         Path(args.out).expanduser().with_suffix("")
                         .with_name(Path(args.out).stem
                                    + " - baffle face 1.nc"))
            # The other side of both uprights: the divider dado and the face
            # dowels, in two setups because they do not fit the machine at
            # once (cam_back.py).
            import cam_back
            stem = Path(args.out).expanduser().with_suffix("")
            cam_back.build_all(
                app, p,
                post_to=None if args.no_save else
                (lambda panel, which:
                 stem.with_name(f"{stem.stem} - {panel} back {which}.nc")))

    return _finish(app, args, asm, p, made)


def _chamber_occupants(app, asm, p):
    """Litres of the woofer chamber that are NOT air, measured off the
    assembly rather than assumed.

    Everything whose middle falls inside the chamber counts — the braces,
    the tube, the driver, the electronics, the T-nuts. Found by POSITION so
    a new part cannot be left out of a list somebody has to remember.

    The port tube counts by its OUTER ENVELOPE, not its material: the air in
    its bore is the port, not the box, so the box loses the whole cylinder.
    That one distinction is 1.9 of the 4.1 litres.
    """
    import math
    if asm is None:
        return None   # --stage carcass: nothing is placed to measure
    asm.activate()
    half_w = float(p["cabinet_internal_w"]) / 2
    half_d = float(p["cabinet_internal_d"]) / 2
    top = -float(p["alpair_chamber_h"]) - 3 * float(p["wall_thickness"])
    floor = -float(p["carcass_h"]) + float(p["wall_thickness"])
    total = 0.0
    for o in app.list_occurrences(bounds=True)["occurrences"]:
        lo, hi = o.get("bbox_min"), o.get("bbox_max")
        if lo is None:
            continue
        mid = [(lo[i] + hi[i]) / 2 for i in range(3)]
        if not (abs(mid[0]) < half_w and abs(mid[1]) < half_d
                and floor < mid[2] < top):
            continue
        total += app.mass_properties(part=o["part_id"])["total"]["volume"] / 1e6
        if o["name"] == "port tube":
            bore = float(p["port_internal_dia"]) / 2000.0
            length = (hi[2] - lo[2]) / 1000.0
            total += math.pi * bore * bore * length * 1000.0  # its air
    return total


def _finish(app, args, asm, p, made):
    displaced = _chamber_occupants(app, asm, p)
    with phase("verify"):
        ok = verify.run(app, p, made, asm, deep=args.deep)
        if displaced is not None:
            tuned, _fb = verify.tuning(p, displaced)
            ok = tuned and ok
    # Without an assembly there is nothing placed to measure, and the GROSS
    # figure is said to be gross rather than quietly printed as the answer.
    note = (f"{displaced:.1f} L standing in the chamber" if displaced
            is not None else "GROSS box — nothing placed to measure")
    print(f"\nport tuning: fb = "
          f"{params.helmholtz_fb(p, displaced or 0.0):.1f} Hz "
          f"(port {float(p['port_internal_dia']):.0f} x "
          f"{float(p['port_length_total']):.0f} mm, {note})")
    if not ok and not args.save_anyway:
        print("\nNOT SAVED — the checks above did not pass")
        print("  (--save-anyway writes it regardless, to look at)")
        report_phases()
        sys.exit(1)
    if not ok:
        print("\n*** SAVING A DOCUMENT THAT FAILED ITS CHECKS (--save-anyway)")
    if not args.no_save:
        out = Path(args.out).expanduser()
        out.parent.mkdir(parents=True, exist_ok=True)
        app.save_project(path=str(out), message="generated by smart_speaker.py")
        print(f"\nsaved {out}")
    report_phases()
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
