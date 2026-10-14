#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Dimensioned Journeyman's Workshop clamp; cosmetic threads, mm throughout."""

import argparse
import json
import math
import sys
import time
from pathlib import Path

from lobole.sdk import Plane, ProjectedCurve, SdkError, connect

SOURCE = "https://journeymans-workshop.uk/images/projects/toolmkrs/toolmkrs.pdf"
PAGE = "https://journeymans-workshop.uk/toolclamps.php"
# Source dimensions are kept separate from explicit assembly/detail choices.
D = dict(
    jaw_l=75,
    jaw_w=12,
    jaw_h=12,
    taper_l=30,
    tip_h=2.5,
    heel_axis=8,
    screw_pitch=30,
    retainer_axis=30,
    rod_d=6,
    rod_l=75,
    thread_pitch=1,
    tip_d=5,
    tip_l=6,
    socket_depth=4,
    knob_d=11,
    knob_l=15,
    knob_bore_depth=12,
    groove_d=8.2,
    groove_w=1.2,
    flange_h=1,
    retainer_d=3,
    retainer_depth=5,
)
CHOICES = dict(
    gap=18,
    insertion=11,
    retaining_rim_d=7,
    retaining_rim_h=0.5,
    retaining_boss_d=4.8,
    retaining_boss_h=2,
    retaining_shank_l=5,
    drive_w=0.8,
    drive_depth=1,
)
SPACER = dict(spacer_d=3.5, spacer_bore=3, spacer_h=1.2)


def profile(p, name, offset, points, depth):
    sk = p.sketch(
        p.datum_plane(base="xz", offset=offset, name=name + " plane"), name=name
    )
    sk.polyline(points)
    return p.extrude(sk.add(), depth, name=name)


def cylinder(p, name, z, d, h, mode="new_body", x=0):
    sk = p.sketch(p.datum_plane(offset=z, name=name + " plane"), name=name)
    sk.circle((x, 0), d=d)
    return p.extrude(sk.add(), h, mode=mode, name=name)


def drill_point(p, name, x, z, d, sign=1):
    # 118 degree drill; depth dimensions denote the cylindrical land.
    sk = p.sketch(Plane.xz(), name=name)
    sk.line((x, z), (x, f"({z})+1"), construction=True)
    sk.polyline(
        [(x, z), (f"({x})+({d})/2", z), (x, f"({z})+({sign})*({d})/2*tan(31 deg)")]
    )
    added = sk.add()
    return p.revolve(
        added,
        axis=dict(kind="sketch_line", sketch_id=added.id, entity_id=1),
        mode="cut",
        name=name,
    )


def thread(p, diameter, designation, minimum=0):
    handles = [
        p.thread(face, designation, allow_engagement=True, minimum_engagement=minimum)
        for face in p.faces(kind="cylinder", radius=diameter / 2, radius_tol=1e-5)
    ]
    if not handles:
        raise SdkError("No threaded faces: " + p.name)
    return handles


def finish(p, material="steel"):
    p.frame("origin", origin=(0, 0, 0))
    p.material(material).check(solids=1)
    if not p.app.check_geometry()["ok"]:
        raise SdkError("Invalid geometry: " + p.name)
    return p


def build(app):
    app.new()
    for group, source in (
        (D, SOURCE),
        (SPACER, PAGE),
        (CHOICES, "Explicit design choice; not in source drawing"),
    ):
        for key, value in group.items():
            if key == "thread_pitch":
                continue  # M6 coarse designation and screw relation fix the pitch.
            app.set_variable(
                name=key, expression=str(value), scope="project", comment=source
            )
    app.set_variable(
        name="front_axis", expression="heel_axis+screw_pitch", scope="project"
    )
    parts = {}
    for moving in (False, True):
        p = app.part("Moving jaw" if moving else "Fixed jaw")
        points = (
            [
                (0, 0),
                ("jaw_l-taper_l", 0),
                ("jaw_l", "jaw_h-tip_h"),
                ("jaw_l", "jaw_h"),
                (0, "jaw_h"),
            ]
            if moving
            else [
                (0, 0),
                ("jaw_l", 0),
                ("jaw_l", "tip_h"),
                ("jaw_l-taper_l", "jaw_h"),
                (0, "jaw_h"),
            ]
        )
        profile(p, "Tapered square stock", "-jaw_w/2", points, "jaw_w")
        cylinder(
            p,
            "M6 tap drill" if moving else "Centre clearance",
            0,
            5 if moving else "rod_d",
            "jaw_h",
            "cut",
            "front_axis",
        )
        cylinder(
            p,
            "Rear M6 tap drill" if moving else "Thrust socket",
            0,
            5 if moving else "tip_d",
            "jaw_h" if moving else "socket_depth",
            "cut",
            "heel_axis",
        )
        if not moving:
            cylinder(
                p,
                "M3 tap drill",
                "jaw_h-retainer_depth",
                2.5,
                "retainer_depth",
                "cut",
                "retainer_axis",
            )
            drill_point(
                p, "M3 drill point", "retainer_axis", "jaw_h-retainer_depth", "2.5", -1
            )
            drill_point(
                p, "Thrust socket drill point", "heel_axis", "socket_depth", "tip_d"
            )
            thread(p, 2.5, "M3", minimum=3)
            p.frame("centre", origin=("front_axis", 0, "jaw_h"))
            p.frame("rear", origin=("heel_axis", 0, "socket_depth-rod_l"))
            p.frame("slide", origin=(0, 0, "-gap-jaw_h"))
            p.frame("retainer", origin=("retainer_axis", 0, "jaw_h"))
        else:
            thread(p, 5, "M6", minimum=12)
            p.frame(
                "centre", origin=("front_axis", 0, "jaw_h+gap+jaw_h+insertion-rod_l")
            )
            p.frame("rear", origin=("heel_axis", 0, "socket_depth-rod_l+gap+jaw_h"))
        parts["moving" if moving else "fixed"] = finish(p)
    for rear in (False, True):
        p = app.part("Rear thrust screw" if rear else "Centre screw")
        cylinder(p, "M6 studding", 0, "rod_d", "rod_l-tip_l" if rear else "rod_l")
        if rear:
            cylinder(p, "Turned thrust tip", "rod_l-tip_l", "tip_d", "tip_l", "add")
        p.thread_handle = thread(p, 6, "M6")[0]
        p.frame(
            "knob", origin=(0, 0, "insertion-knob_l" if rear else "rod_l-insertion")
        )
        parts["rear_rod" if rear else "centre_rod"] = finish(p)
    for rear in (False, True):
        p = app.part("Rear knob" if rear else "Captive knob")
        if rear:
            cylinder(p, "Knurled blank", 0, "knob_d", "knob_l")
        else:
            p.revolve_profile(
                [
                    (0, 0),
                    ("knob_d/2", 0),
                    ("knob_d/2", "flange_h"),
                    ("groove_d/2", "flange_h"),
                    ("groove_d/2", "flange_h+groove_w"),
                    ("knob_d/2", "flange_h+groove_w"),
                    ("knob_d/2", "knob_l"),
                    (0, "knob_l"),
                ],
                name="Captive turned profile",
            )
        cylinder(
            p,
            "Blind M6 tap drill",
            "knob_l-knob_bore_depth" if rear else 0,
            5,
            "knob_bore_depth",
            "cut",
        )
        drill_point(
            p,
            "Blind tap drill point",
            0,
            "knob_l-knob_bore_depth" if rear else "knob_bore_depth",
            5,
            -1 if rear else 1,
        )
        thread(p, 5, "M6", minimum=10)
        parts["rear_knob" if rear else "centre_knob"] = finish(p)
    p = app.part("Brass retaining spacer")
    cylinder(p, "Spacer stock", 0, "spacer_d", "spacer_h")
    cylinder(p, "Spacer bore", 0, "spacer_bore", "spacer_h", "cut")
    parts["spacer"] = finish(p, "brass")
    p = app.part("M3 retaining screw - specified replacement")
    cylinder(
        p,
        "Thread envelope",
        "spacer_h-retaining_shank_l",
        "retainer_d",
        "retaining_shank_l",
    )
    cylinder(
        p, "Thin retaining rim", "spacer_h", "retaining_rim_d", "retaining_rim_h", "add"
    )
    cylinder(
        p,
        "Reduced head",
        "spacer_h+retaining_rim_h",
        "retaining_boss_d",
        "retaining_boss_h",
        "add",
    )
    datum = p._add(
        dict(
            type="DatumPlane",
            name="Drive slot plane",
            kind="offset",
            base_plane="xy",
            offset=0,
            offset_expr="spacer_h+retaining_rim_h+retaining_boss_h-drive_depth",
        )
    )["id"]
    sk = p.sketch(Plane.xy(), name="Drive slot")
    sk.polyline(
        [
            ("-retaining_boss_d", "-drive_w/2"),
            ("retaining_boss_d", "-drive_w/2"),
            ("retaining_boss_d", "drive_w/2"),
            ("-retaining_boss_d", "drive_w/2"),
        ]
    )
    raw = sk.json()
    raw["plane_ref"] = datum
    p.extrude(p._add(raw)["id"], "drive_depth", mode="cut", name="Screwdriver slot")
    thread(p, 3, "M3")
    parts["retainer"] = finish(p)
    rig = app.assembly("Toolmakers clamp")
    fixed = rig.place(parts["fixed"], fixed=True, name="Fixed jaw")
    moving = rig.place(parts["moving"], name="Moving jaw")
    slide = rig.slider(
        (fixed, "slide"), (moving, "origin"), flipped=False, name="Parallel jaw opening"
    )
    drives = []
    for rear in (False, True):
        key = "rear" if rear else "centre"
        rod = rig.place(parts[key + "_rod"], name=parts[key + "_rod"].name)
        drive = rig.cylindrical(
            (moving, key),
            (rod, "origin"),
            flipped=False,
            name=key + " screw engagement",
        )
        rig.screw(drive, parts[key + "_rod"].thread_handle)
        # Loop fixes the screw axially to the upper jaw but permits rotation.
        if not rear:
            parts["centre_rod"].frame("seat", origin=(0, 0, "rod_l-insertion"))
        rig.revolute(
            (fixed, key),
            (rod, "origin" if rear else "seat"),
            flipped=False,
            name=key + " thrust seat",
        )
        knob = rig.place(parts[key + "_knob"], name=parts[key + "_knob"].name)
        rig.fasten((rod, "knob"), (knob, "origin"), flipped=False)
        drives.append(drive)
    for key in ("spacer", "retainer"):
        o = rig.place(parts[key], name=parts[key].name)
        rig.fasten((fixed, "retainer"), (o, "origin"), flipped=False)
    app.delete_part(part="Part 1")
    rig.solve()
    return parts, rig, drives, moving


NOTES = {
    "fixed": [
        "{{jaw_l}} x {{jaw_w}} x {{jaw_h}} stock; {{taper_l}} long taper to {{tip_h}} tip.",
        "Axes from heel: thrust {{heel_axis}}; retainer {{retainer_axis}}; centre {{front_axis}}. All on width centreline.",
        "Centre: DIA {{rod_d}} THRU. Thrust: DIA {{tip_d}} depth {{socket_depth}} + 118 deg point.",
        "Retainer: M3 x 0.5 depth {{retainer_depth}}; tap drill DIA 2.5 + 118 deg point.",
    ],
    "moving": [
        "{{jaw_l}} x {{jaw_w}} x {{jaw_h}} stock; {{taper_l}} long taper to {{tip_h}} tip.",
        "2 x M6 x 1 THRU; axes {{heel_axis}} and {{front_axis}} from heel, centred across width.",
        "Tap drill DIA 5; cosmetic M6 thread. Exact checks recognise bounded thread engagement.",
    ],
    "centre_rod": [
        "M6 x 1 studding, cut length {{rod_l}}. Finish and deburr ends.",
        "Insert {{insertion}} into captive knob, from flange end; retain as bonded assembly.",
    ],
    "rear_rod": [
        "M6 x 1 studding, total length {{rod_l}}. Turn last {{tip_l}} to DIA {{tip_d}}.",
        "Flat thrust tip follows plan; shoulder seats at depth {{socket_depth}}.",
        "Insert {{insertion}} into rear knob; retain as bonded assembly.",
    ],
    "centre_knob": [
        "DIA {{knob_d}} x {{knob_l}}. Groove DIA {{groove_d}} x {{groove_w}}; flange {{flange_h}}; body {{knob_l-groove_w-flange_h}}.",
        "M6 x 1 blind depth {{knob_bore_depth}} FROM FLANGE END; DIA 5 tap drill + 118 deg point.",
        "Bore entry corrected to suit assembly; see source audit.",
        "Knurl and chamfer: finish callouts only; pitch/chamfer not specified.",
    ],
    "rear_knob": [
        "DIA {{knob_d}} x {{knob_l}}. M6 x 1 blind depth {{knob_bore_depth}} from rod end.",
        "DIA 5 tap drill + 118 deg point. Knurl and chamfer per source finish note.",
    ],
    "spacer": [
        "Brass tube: DIA {{spacer_d}} OD, DIA {{spacer_bore}} ID, length {{spacer_h}}."
    ],
    "retainer": [
        "DESIGN DETAIL: replacement for source washer-head M3 screw.",
        "M3 x 0.5; shank {{retaining_shank_l}} below rim. Rim DIA {{retaining_rim_d}} x {{retaining_rim_h}}.",
        "Boss DIA {{retaining_boss_d}} x {{retaining_boss_h}} above rim. Drive slot {{drive_w}} wide x {{drive_depth}} deep.",
        "Dimensions chosen for source groove and {{front_axis-retainer_axis}} mm axis offset.",
    ],
}


def drawings(app, parts, out):
    checks = {}
    for key, p in parts.items():
        app.create_drawing(
            name=p.name + " - drawing",
            sheet=dict(
                size="a3",
                landscape=True,
                title_block=dict(project="Toolmakers clamp", document=p.name),
            ),
        )
        views = {}
        for orient, pos in [
            (
                "front",
                [
                    100,
                    95 if key in ("fixed", "moving", "centre_rod", "rear_rod") else 110,
                ],
            ),
            ("top", [100, 75]),
            # Clear of the title block, 48 mm tall since it gained its
            # material, scale and notes rows (v147), and a rod's tall iso
            # clear of the source notes above it.
            (
                "iso",
                [
                    290,
                    {"fixed": 112, "moving": 112, "centre_rod": 88,
                     "rear_rod": 88}.get(key, 95),
                ],
            ),
        ]:
            views[orient] = app.add_drawing_view(
                source=p.name,
                orientation=orient,
                position=pos,
                scale=(
                    1.5 if key in ("fixed", "moving", "centre_rod", "rear_rod") else 4
                ),
            )
        for i, note in enumerate(NOTES[key]):
            app.add_drawing_note(text=note, position=[210, 270 - i * 9])
        app.add_drawing_note(
            text="mm. Tap-drill geometry; cosmetic thread callouts. Engagement checks do not certify thread strength.",
            position=[210, 215],
        )
        app.add_drawing_note(
            text="Source: Journeymans Workshop, 18 Aug 2014. No general tolerances specified in source.",
            position=[210, 205],
        )
        app.update_drawing(_timeout=120)
        # Actual projected B-Rep endpoints supply associative overall dimensions.
        view = views["front"]["id"]
        geo = app.list_drawing_geometry(view=view)["geometry"]
        ends = [
            (g, end, g["p1" if end == 0 else "p2"])
            for g in geo
            if g.get("visible") and g["type"] == "line"
            for end in (0, 1)
        ]
        for axis, kind in enumerate(("distance_x", "distance_y")):
            lo = min(ends, key=lambda e: e[2][axis])
            hi = max(ends, key=lambda e: e[2][axis])
            dim = app.add_drawing_dim(
                view=view,
                kind=kind,
                refs=[
                    ProjectedCurve(e[0]).pick("start" if e[1] == 0 else "end")
                    for e in (lo, hi)
                ],
            )
            if not dim["resolved"]:
                raise SdkError(
                    "Unresolved overall dimension: " + p.name + str((lo, hi, dim))
                )
        check = app.check_drawing()
        if not check["ok"]:
            raise SdkError(p.name + ": " + str(check))
        checks[key] = check
        app.export_drawing(path=str(out / (key + ".svg")))
    return checks


def verify(app, parts, rig, drives, moving):
    rig.activate()
    solved = rig.solve()
    # The clamp's screws close a loop; without one the loop keys are absent
    # and this should fail as an assertion, not a KeyError.
    assert (
        solved.get("loops_closed")
        and solved.get("dof") == 1
        and not solved["errored_mates"]
    ), solved
    volumes = {}
    for key, p in parts.items():
        p.check(solids=1)
        volumes[key] = app.measure(a={"part": p.id})["volume"]
    # Independent stock-minus-tools calculation catches missing cuts/plane mistakes.
    blank = 75 * 12 * 12 - 30 * (12 - 2.5) * 12 / 2
    cone = lambda d: math.pi * (d / 2) ** 3 * math.tan(math.radians(31)) / 3
    expected = {
        "moving": blank - 2 * math.pi * 2.5**2 * 12,
        "fixed": blank
        - math.pi * 3**2 * 12
        - math.pi * 2.5**2 * 4
        - math.pi * 1.25**2 * 5
        - cone(5)
        - cone(2.5),
    }
    for key, value in expected.items():
        assert abs(volumes[key] - value) < 0.001, (key, volumes[key], value)
    rig.activate()
    standing = app.interference(mode="exact", max_pairs=1000, deadline_s=120)
    assert not standing["clash"] and not standing.get("truncated"), standing
    engagements = [
        rig.thread_engagement(rig.occurrence(a), rig.occurrence(b))
        for a, b in (
            ("Moving jaw", "Centre screw"),
            ("Moving jaw", "Rear thrust screw"),
            ("Captive knob", "Centre screw"),
            ("Rear knob", "Rear thrust screw"),
            ("Fixed jaw", "M3 retaining screw - specified replacement"),
        )
    ]
    study = rig.motion(
        drives[0],
        dof="rotation",
        **{"from": 0},
        to=3600,
        samples=41,
        clash_mode="exact",
        name="Ten turns - parallel adjustment",
    )
    motion = app.run_motion_study(study=study["id"], include_poses=True, _timeout=180)
    assert motion["clean"] and not motion["pairs_unknown"], motion["table"]
    gaps = []
    for sample in motion["samples"]:
        pose = next(p for p in sample["poses"] if p["occurrence"] == moving.id)
        gap = -pose["position"][2] - D["jaw_h"]
        assert abs(gap - (CHOICES["gap"] + sample["value"] / 360)) < 0.001, (
            sample,
            gap,
        )
        assert (
            abs(pose["orientation"][0])
            + abs(pose["orientation"][1])
            + abs(pose["orientation"][2])
            < 1e-5
        )
        gaps.append(gap)
    # Both threads engage the entire 12 mm jaw through the tested opening range.
    centre_bottom = D["jaw_h"] + CHOICES["insertion"] - D["rod_l"]
    min_jaw_bottom = -max(gaps) - D["jaw_h"]
    assert centre_bottom < min_jaw_bottom
    # Retaining rim clears groove root and captures flange, with axial clearance.
    offset = D["heel_axis"] + D["screw_pitch"] - D["retainer_axis"]
    radial = offset - CHOICES["retaining_rim_d"] / 2 - D["groove_d"] / 2
    overlap = D["knob_d"] / 2 - (offset - CHOICES["retaining_rim_d"] / 2)
    lower = SPACER["spacer_h"] - D["flange_h"]
    upper = (
        D["flange_h"] + D["groove_w"] - SPACER["spacer_h"] - CHOICES["retaining_rim_h"]
    )
    assert min(radial, overlap, lower, upper) > 0
    return dict(
        solve=solved,
        engagements=engagements,
        volumes_mm3=volumes,
        standing=standing,
        motion=motion,
        opening_mm=[min(gaps), max(gaps)],
        retention_mm=dict(
            root_clearance=radial,
            flange_overlap=overlap,
            lower_clearance=lower,
            upper_clearance=upper,
        ),
    )


def run(app, out, full=True):
    out.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    parts, rig, drives, moving = build(app)
    result = dict(
        build_s=time.perf_counter() - start,
        thread_representation="cosmetic threads on manufacturing tap-drill geometry",
    )
    if full:
        result.update(verify(app, parts, rig, drives, moving))
        result["drawings"] = drawings(app, parts, out)
    rig.activate()
    result["bom"] = app.bom()
    app.export_bom(path=str(out / "bom.csv"))
    app.export_step(path=str(out / "toolmakers-clamp.step"))
    app.save_project(path=str(out / "toolmakers-clamp.dsgn"))
    register = {
        k: dict(
            value=v, source=source, tolerance=None, project_variable=k != "thread_pitch"
        )
        for group, source in ((D, SOURCE), (SPACER, PAGE), (CHOICES, "design choice"))
        for k, v in group.items()
    }
    (out / "dimension-register.json").write_text(json.dumps(register, indent=2))
    result["total_s"] = time.perf_counter() - start
    (out / "report.json").write_text(json.dumps(result, indent=2))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", type=Path, default=Path("/tmp/toolmakers-clamp"))
    ap.add_argument(
        "--preview",
        action="store_true",
        help="Skip drawings and verification for quick iteration",
    )
    a = ap.parse_args()
    result = run(connect(a.port), a.out, not a.preview)
    print(json.dumps({k: result[k] for k in ("build_s", "total_s")}, indent=2))


if __name__ == "__main__":
    main()
