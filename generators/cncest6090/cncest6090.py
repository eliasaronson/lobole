#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""CNCest layout and provisional Z axis. Only spindle_d=80 is user confirmed.

All dimensions are project variables. Sketch coordinates, offsets, extrusions
and attachment frames retain expressions so later measurements propagate.
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # _lib
sys.path.insert(0, str(Path(__file__).resolve().parent))
from lobole.sdk import Plane, SdkError, connect
from cncest6090_spec import D, dimensions, manifest


def profile(p, base, offset, name, draw, depth, mode="new_body"):
    parent = {}
    if base == "xz":
        # Native XZ has +Y normal and -Z V; SDK XZ uses -Y / +Z.
        parent["parent_id"] = p._add(
            dict(
                type="DatumPlane",
                name=name + " XZ basis",
                kind="angled",
                base_plane="xy",
                axis={"kind": "x"},
                angle_deg=90,
            )
        )["id"]
    datum = p._add(
        dict(
            type="DatumPlane",
            name=name + " plane",
            kind="offset",
            offset=0,
            offset_expr=str(offset),
            **(parent or {"base_plane": base}),
        )
    )
    sk = p.sketch({"xy": Plane.xy, "xz": Plane.xz, "yz": Plane.yz}[base](), name=name)
    draw(sk)
    raw = sk.json()
    raw["plane_ref"] = datum["id"]
    sketch = p._add(raw)
    return p.extrude(sketch["id"], depth, mode=mode, name=name)


def box(p, x0, x1, y0, y1, z, h, name, mode="new_body"):
    return profile(
        p,
        "xy",
        z,
        name,
        lambda s: s.polyline([(x0, y0), (x1, y0), (x1, y1), (x0, y1)]),
        h,
        mode,
    )


def cyl(p, x, y, z, d, h, name, mode="new_body"):
    return profile(p, "xy", z, name, lambda s: s.circle((x, y), d=d), h, mode)


def finish(p, material="aluminum_6061_t6"):
    p.frame("origin", origin=(0, 0, 0))
    p.material(material).check(solids=1)
    r = p.app.check_geometry()
    if not r["ok"]:
        raise SdkError(f"{p.name}: {r}")
    return p


def collar(app, accessory=False):
    p = app.part(
        "Draft dust-shoe adapter collar" if accessory else "Spindle split clamp"
    )
    h = "accessory_h" if accessory else "clamp_h"
    r = "accessory_r" if accessory else "clamp_r"
    profile(p, "xy", 0, "Collar ring", lambda s: s.circle((0, 0), r=r), h)
    box(p, -20, 20, f"-{r}-11", f"-{r}+11", 0, h, "Pinch lugs", "add")
    if accessory:
        box(p, -20, 20, 35, 70, 0, h, "Accessory mounting tab", "add")
        profile(
            p,
            "xy",
            0,
            "Accessory M5 clearance holes",
            lambda s: (s.circle((-12, 60), d=5.5), s.circle((12, 60), d=5.5)),
            h,
            "cut",
        )
    else:
        for sign in (-1, 1):
            x = f"{sign}*clamp_bolt_pitch/2"
            box(
                p,
                f"{x}-12",
                f"{x}+12",
                -5,
                "bearing_front-carriage_t-spindle_y",
                0,
                h,
                "Carriage mounting ear" + str(sign),
                "add",
            )
        profile(
            p,
            "xz",
            "spindle_y-bearing_front+carriage_t-1",
            "Carriage mounting holes",
            lambda s: (
                s.circle(("-clamp_bolt_pitch/2", f"{h}/2"), d=6.6),
                s.circle(("clamp_bolt_pitch/2", f"{h}/2"), d=6.6),
            ),
            "bearing_front-carriage_t-spindle_y+6",
            "cut",
        )
    cyl(p, 0, 0, 0, "spindle_d+bore_clearance", h, "Spindle seating bore", "cut")
    if not accessory:
        box(
            p,
            -200,
            200,
            "bearing_front-carriage_t-spindle_y",
            200,
            0,
            h,
            "Flush carriage seat",
            "cut",
        )
    box(p, "-clamp_gap/2", "clamp_gap/2", f"-{r}-12", 0, 0, h, "Clamp split", "cut")
    profile(
        p,
        "yz",
        -21,
        "Pinch bolt clearance",
        lambda s: s.circle((f"-{r}-4", f"{h}/2"), d=6.6),
        42,
        "cut",
    )
    p.frame("pinch_bolt", origin=(-20, f"-{r}-4", f"{h}/2"), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("pinch_nut", origin=(20, f"-{r}-4", f"{h}/2"), z=(-1, 0, 0), x=(0, 1, 0))
    if not accessory:
        for i, sign in enumerate((-1, 1)):
            p.frame(
                f"mount_bolt_{i}",
                origin=(f"{sign}*clamp_bolt_pitch/2", -5, f"{h}/2"),
                z=(0, 1, 0),
                x=(1, 0, 0),
            )
    return finish(p)


def z_parts(app):
    back = app.part("Z stationary plate")
    box(back, "-z_w/2", "z_w/2", 0, "back_t", 0, "z_h", "Stationary plate")
    for side in (-1, 1):
        back.frame(f"guide{side}", origin=(f"{side}*guide_pitch/2", "guide_y", "end_t"))
    for i, z in enumerate(("0", "z_h-end_t")):
        back.frame(f"end{i}", origin=(0, 0, z))
    back.frame("motor", origin=(0, "guide_y", "z_h"))
    back.frame("screw", origin=(0, "guide_y", 0))
    finish(back)
    end = app.part("Z guide support")
    box(end, "-z_w/2", "z_w/2", "guide_y-guide_d/2-8", 0, 0, "end_t", "End bridge")
    profile(
        end,
        "xy",
        0,
        "Shaft seats",
        lambda s: (
            s.circle(("-guide_pitch/2", "guide_y"), d="guide_d+bore_clearance"),
            s.circle(("guide_pitch/2", "guide_y"), d="guide_d+bore_clearance"),
            s.circle((0, "guide_y"), d="screw_d+bore_clearance"),
        ),
        "end_t",
        "cut",
    )
    finish(end)
    guide = app.part("Z guide shaft")
    cyl(guide, 0, 0, 0, "guide_d", "z_h-2*end_t", "Guide shaft")
    finish(guide, "steel")
    screw = app.part("Z screw envelope")
    cyl(screw, 0, 0, 0, "screw_d", "z_h", "Unthreaded screw envelope")
    finish(screw, "steel")
    motor = app.part("Z motor envelope")
    box(
        motor,
        "-motor_w/2",
        "motor_w/2",
        "-motor_w/2",
        "motor_w/2",
        0,
        "motor_l",
        "Motor body",
    )
    finish(motor, "steel")
    moving = app.part("Z moving plate")
    box(
        moving,
        "-z_w/2",
        "z_w/2",
        "bearing_front-carriage_t",
        "bearing_front",
        "carriage_z",
        "carriage_h",
        "Moving plate",
    )
    profile(
        moving,
        "xz",
        "-bearing_front",
        "Clamp mounting clearance",
        lambda s: (
            s.circle(("-clamp_bolt_pitch/2", "clamp_z+clamp_h/2"), d=6.6),
            s.circle(("clamp_bolt_pitch/2", "clamp_z+clamp_h/2"), d=6.6),
        ),
        "carriage_t",
        "cut",
    )
    for side in (-1, 1):
        for i, z in enumerate(("carriage_z", "carriage_z+carriage_h-bearing_h")):
            moving.frame(f"bearing{side}_{i}", origin=(f"{side}*guide_pitch/2", 0, z))
    for i, sign in enumerate((-1, 1)):
        moving.frame(
            f"clamp_nut_{i}",
            origin=(
                f"{sign}*clamp_bolt_pitch/2",
                "bearing_front+seat_allowance",
                "clamp_z+clamp_h/2",
            ),
            z=(0, -1, 0),
            x=(1, 0, 0),
        )
    moving.frame("spindle", origin=(0, "spindle_y", "spindle_z"))
    moving.frame("clamp", origin=(0, "spindle_y", "clamp_z"))
    moving.frame("accessory", origin=(0, "spindle_y", "accessory_z"))
    finish(moving)
    bearing = app.part("Z linear bearing envelope")
    box(
        bearing,
        "-bearing_w/2",
        "bearing_w/2",
        "bearing_front",
        "bearing_back",
        0,
        "bearing_h",
        "Bearing housing",
    )
    cyl(
        bearing,
        0,
        "guide_y",
        0,
        "guide_d+bore_clearance",
        "bearing_h",
        "Guide bore",
        "cut",
    )
    finish(bearing)
    spindle = app.part("80 mm spindle")
    spindle.revolve_profile(
        [
            (0, -35),
            (8, -35),
            (8, -20),
            (20, -20),
            (20, 0),
            ("spindle_d/2", 0),
            ("spindle_d/2", "spindle_l"),
            (0, "spindle_l"),
        ],
        name="Spindle body and nose",
    )
    finish(spindle, "steel")
    return dict(
        back=back,
        end=end,
        guide=guide,
        screw=screw,
        motor=motor,
        moving=moving,
        bearing=bearing,
        spindle=spindle,
        clamp=collar(app),
        accessory=collar(app, True),
    )


def fasten(rig, parent, frame, part, name=None):
    o = rig.place(part, name=name or part.name)
    rig.fasten((parent, frame), (o, "origin"), flipped=False)
    return o


def bolt(app, rig, parent, frame, size, length=0, nut=False):
    rig.activate()
    r = app.add_fastener(
        standard="iso4032" if nut else "iso4762", size=size, length=length
    )
    app.rename_occurrence(id=r["occurrence"], name=f"{parent.name} {frame}")
    rig.fasten(
        (parent, frame), dict(occurrence=r["occurrence"], origin=True), flipped=False
    )


def assemble_z(app, rig, parts, anchor=None):
    back = rig.place(parts["back"], fixed=anchor is None, name=parts["back"].name)
    if anchor:
        rig.fasten(anchor, (back, "origin"), flipped=False)
    moving = rig.place(parts["moving"], name=parts["moving"].name)
    slide = rig.slider(
        (back, "origin"), (moving, "origin"), flipped=False, name="Z carriage travel"
    )
    for side in (-1, 1):
        fasten(rig, back, f"guide{side}", parts["guide"], f"Z guide {side}")
        for i in range(2):
            fasten(
                rig,
                moving,
                f"bearing{side}_{i}",
                parts["bearing"],
                f"Z bearing {side} {i}",
            )
    for i in range(2):
        fasten(rig, back, f"end{i}", parts["end"], f"Z end support {i}")
    for key in ("motor", "screw"):
        fasten(rig, back, key, parts[key])
    spindle = fasten(rig, moving, "spindle", parts["spindle"])
    for key in ("clamp", "accessory"):
        o = fasten(rig, moving, key, parts[key])
        bolt(app, rig, o, "pinch_bolt", "M6", 50)
        bolt(app, rig, o, "pinch_nut", "M6", nut=True)
        if key == "clamp":
            for i in range(2):
                bolt(app, rig, o, f"mount_bolt_{i}", "M6", 80)
                bolt(app, rig, moving, f"clamp_nut_{i}", "M6", nut=True)
    solved = rig.solve()
    if solved["errored_mates"]:
        raise SdkError(str(solved))
    return slide, spindle


def layout(app, parts):
    base = app.part("Router base envelope")
    box(base, "-bed_w/2", "bed_w/2", 0, "bed_l", 0, "bed_top-bed_t", "Base envelope")
    base.frame("bed", origin=(0, 0, "bed_top-bed_t"))
    base.frame(
        "z_axis", origin=(0, "gantry_y-back_t", "bed_top+tip_clearance-spindle_z+35")
    )
    base.frame("beam", origin=(0, "gantry_y", "gantry_h-200"))
    for side in (-1, 1):
        base.frame(f"upright{side}", origin=(f"{side}*(bed_w/2+30)", "gantry_y", 0))
    finish(base)
    bed = app.part("Slotted bed envelope")
    box(bed, "-bed_w/2", "bed_w/2", 0, "bed_l", 0, "bed_t", "Bed plate envelope")
    for i in range(1, 16):
        box(
            bed,
            f"-bed_w/2+bed_w*{i}/16-2",
            f"-bed_w/2+bed_w*{i}/16+2",
            0,
            "bed_l",
            "bed_t-5",
            5,
            f"Visual slot {i}",
            "cut",
        )
    finish(bed)
    upright = app.part("Gantry upright envelope")
    box(
        upright,
        "-gantry_t/2",
        "gantry_t/2",
        0,
        "beam_depth",
        0,
        "gantry_h",
        "Upright envelope",
    )
    finish(upright)
    beam = app.part("Gantry beam envelope")
    box(beam, "-bed_w/2-20", "bed_w/2+20", 0, "beam_depth", 0, 200, "Beam envelope")
    finish(beam)
    rig = app.assembly("CNCest 6090 provisional layout")
    root = rig.place(base, fixed=True)
    fasten(rig, root, "bed", bed)
    fasten(rig, root, "beam", beam)
    for side in (-1, 1):
        fasten(rig, root, f"upright{side}", upright, f"Gantry upright {side}")
    assemble_z(app, rig, parts, (root, "z_axis"))
    return rig


def build(app, values):
    app.new()
    for key, row in manifest(values).items():
        app.set_variable(
            name=key,
            expression=str(row["value"]),
            scope="project",
            comment=f"[{row['status']}] Baseline {D[key].value} mm. {row['note']} Later edits require verification; no manufacturing tolerance assigned.",
        )
    parts = z_parts(app)
    rig = app.assembly("Z-axis development fixture")
    slide, spindle = assemble_z(app, rig, parts)
    machine = layout(app, parts)
    app.delete_part(part="Part 1")
    return parts, rig, slide, spindle, machine


def drawing(app, part, out, values):
    app.create_drawing(
        name="Accessory collar - provisional",
        sheet=dict(
            size="a3",
            landscape=True,
            title_block=dict(
                project="CNCest 6090", document="PROVISIONAL - verify on machine"
            ),
        ),
    )
    views = {}
    for orientation, position in [
        ("top", [105, 180]),
        ("front", [105, 85]),
        ("iso", [280, 180]),
    ]:
        views[orientation] = app.add_drawing_view(
            source=part.name, orientation=orientation, position=position, scale=1
        )
    app.add_drawing_note(
        text="DRAFT: 80 mm spindle supplied by owner. Other interfaces unverified.",
        position=[210, 275],
    )
    app.add_drawing_note(
        text="Split collar + accessory tab. Check spindle land, clearances and fastening before manufacture.",
        position=[210, 263],
    )
    app.update_drawing(_timeout=120)
    geometry = app.list_drawing_geometry(view=views["top"]["id"])["geometry"]
    for radius in ((values["spindle_d"] + values["bore_clearance"]) / 2, 2.75):
        edge = next(
            g
            for g in geometry
            if g.get("visible") and abs(g.get("radius", -1) - radius) < 0.001
        )
        dim = app.add_drawing_dim(
            view=views["top"]["id"], kind="diameter", refs=[{"edge": edge["ref"]}]
        )
        if not dim["resolved"]:
            raise SdkError("Draft diameter dimension did not resolve")
    geometry = app.list_drawing_geometry(view=views["front"]["id"])["geometry"]
    lines = [
        g
        for g in geometry
        if g.get("visible")
        and g["type"] == "line"
        and abs(g["p1"][1] - g["p2"][1]) < 0.001
    ]
    low, high = min(lines, key=lambda g: g["p1"][1]), max(
        lines, key=lambda g: g["p1"][1]
    )
    app.add_drawing_dim(
        view=views["front"]["id"],
        kind="distance_y",
        refs=[{"edge": g["ref"]} for g in (low, high)],
    )
    check = app.check_drawing()
    (out / "drawing-check.json").write_text(json.dumps(check, indent=2))
    if not check["ok"]:
        raise SdkError(f"Draft drawing failed: {check}")
    app.export_drawing(path=str(out / "accessory-collar-draft.svg"))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", type=Path, default=Path("/tmp/cncest6090"))
    ap.add_argument(
        "--dimensions", type=Path, help="JSON object of dimension overrides in mm"
    )
    ap.add_argument("--stage", choices=("preview", "local", "full"), default="preview")
    a = ap.parse_args()
    v = dimensions(json.loads(a.dimensions.read_text()) if a.dimensions else None)
    a.out.mkdir(parents=True, exist_ok=True)
    app = connect(a.port)
    start = time.perf_counter()
    parts, rig, slide, spindle, machine = build(app, v)
    result = dict(
        status="provisional",
        manufacturing_ready=False,
        validation_scope="Z-axis fixture only; layout envelopes excluded",
        dimensions=manifest(v),
        build_s=time.perf_counter() - start,
    )
    machine.activate()
    app.save_project(path=str(a.out / "cncest6090-provisional.lob"))
    print(f'Built provisional model in {result["build_s"]:.2f}s', flush=True)
    rig.activate()
    if a.stage != "preview":
        result["standing"] = app.interference(
            mode="exact", max_pairs=5000, deadline_s=120
        )
    if a.stage == "full":
        study = rig.motion(
            slide,
            dof="offset",
            **{"from": 0},
            to=v["z_travel"],
            samples=9,
            clash_mode="exact",
            name="Provisional Z travel",
        )
        result["motion"] = app.run_motion_study(
            study=study["id"], include_poses=True, _timeout=300
        )
    drawing(app, parts["accessory"], a.out, v)
    machine.activate()
    app.save_project(path=str(a.out / "cncest6090-provisional.lob"))
    (a.out / "dimensions.json").write_text(json.dumps(v, indent=2))
    (a.out / "dimension-register.json").write_text(json.dumps(manifest(v), indent=2))
    (a.out / "report.json").write_text(json.dumps(result, indent=2))
    print(
        json.dumps(
            {
                k: val
                for k, val in result.items()
                if k not in ("dimensions", "motion", "standing")
            },
            indent=2,
        )
    )
    if (
        result.get("standing", {}).get("clash")
        or result.get("standing", {}).get("truncated")
        or result.get("motion", {}).get("clean") is False
    ):
        raise SdkError("Provisional Z fixture clearance failed; inspect report.json")


if __name__ == "__main__":
    main()
