#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: the printing process allowance over RPC, headless.

docs/tolerances/README.md#printed-parts. What only the RPC layer shows: a part
declares how it is printed, and check_fit, check_fits, print_allowance and a
Thread's add_feature answer say what to MODEL for it — and the advice they
give, followed, is what they then accept.

Usage: print_allowance_smoke.py <path-to-designer-binary>
"""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402

PETG = {"process": "fdm", "material": "petg"}


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def near(a, b, tol=1e-6):
    return abs(a - b) < tol


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e),
               f"{method} refused, but not naming '{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def round_faces(d):
    return [(b["created_by"], f["name"])
            for b in d.call("list_elements")["bodies"]
            for f in b.get("faces", []) if f.get("kind") == "cylinder"]


def bored_part(d, name, diameter):
    """A block with one bore through it; returns the bore face."""
    d.call("add_part", name=name)
    box(d, (-15, -15, 0), (30, 30, 12), "Body")
    d.call("add_feature", feature={
        "type": "Sketch", "name": "Centre",
        "plane": {"origin": [0, 0, 12], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [0, 0], "radius": 2}]})
    sk = {f["name"]: f["id"] for f in d.call("get_state")["features"]}["Centre"]
    d.call("add_feature", feature={
        "type": "Hole", "name": "Bore", "hole_kind": "simple",
        "sketch_id": sk, "diameter": diameter, "through": True})
    faces = round_faces(d)
    expect(len(faces) == 1, f"one bore: {faces}")
    return faces[0]


def pin(d, name, diameter):
    d.call("add_part", name=name)
    cylinder(d, (0, 0, 0), "z", diameter / 2.0, 30, "Pin")
    return round_faces(d)[0]


def stl_triangles(path):
    """The binary STL's triangles, as three (x, y, z) each."""
    with open(path, "rb") as f:
        data = f.read()
    (n,) = struct.unpack_from("<I", data, 80)
    tris = []
    for i in range(n):
        v = struct.unpack_from("<12f", data, 84 + 50 * i)
        tris.append((v[3:6], v[6:9], v[9:12]))
    return tris


def gap_in_stl(path, r_disc, r_hole):
    """The gap a side between a disc and the hole round it, both on the z
    axis, as the STL carries it: (the narrowest, the mean). A chord sits
    inside its round surface, so the narrowest is between a chord's middle
    on the hole and a vertex on the disc."""
    disc, hole = [], []
    for tri in stl_triangles(path):
        rs = [math.hypot(p[0], p[1]) for p in tri]
        # A wall triangle runs top to bottom; a cap's is flat in z.
        zs = [p[2] for p in tri]
        if max(zs) - min(zs) < 0.5 or max(rs) - min(rs) > 0.02:
            continue
        mids = [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
                for a, b in ((tri[0], tri[1]), (tri[1], tri[2]),
                             (tri[2], tri[0]))]
        pts = rs + [math.hypot(*m) for m in mids]
        if abs(max(rs) - r_disc) < 0.05:
            disc += pts
        elif abs(max(rs) - r_hole) < 0.05:
            hole += pts
    expect(disc and hole, f"found the disc and the hole walls in {path}")
    return min(hole) - max(disc), (sum(hole) / len(hole) -
                                   sum(disc) / len(disc))


def profiles(d, port):
    """Print profiles (docs/tolerances/README.md#print-profiles): the store,
    a part that embeds one, the match, the export that keeps the gaps, and
    the coupon's --record."""
    have = d.call("list_print_profiles")
    expect(have["profiles"] == [], f"the scratch store starts empty: {have}")
    expect(have["dir"].endswith("print_profiles") and
           "config" in have["dir"], f"in the isolated config: {have}")
    expect(any(t["material"] == "pla" for t in have["defaults"]),
           f"the built-in tables are the defaults: {have}")

    # No rows: the built-in table, to correct.
    p = d.call("save_print_profile", profile={
        "name": "Test P1S", "process": "fdm", "material": "pla"})
    expect(any(r["kind"] == "gap" and near(r["allowance_mm"], 0.5)
               for r in p["rows"]), f"starts as the default: {p}")
    rows = [r for r in p["rows"] if r["kind"] != "gap"] + [
        {"kind": "gap", "from_mm": 0, "allowance_mm": 0.2,
         "source": "coupon"}]
    p = d.call("save_print_profile", profile={
        "name": "Test P1S", "process": "fdm", "material": "pla",
        "stl_deflection_mm": 0.01, "rows": rows})
    expect(near(p["stl_deflection_mm"], 0.01) and p["stl_deflection_stated"],
           f"saved: {p}")

    # Refused, naming what is wrong, and nothing written.
    refused(d, "save_print_profile", "needs a name",
            profile={"name": "", "process": "fdm", "material": "pla"})
    refused(d, "save_print_profile", "Accepted for fdm: petg, pla",
            profile={"name": "X", "process": "fdm", "material": "abs"})
    refused(d, "save_print_profile", "gap's allowance must be positive",
            profile={"name": "X", "process": "fdm", "material": "pla",
                     "rows": [{"kind": "gap", "allowance_mm": -0.1}]})
    refused(d, "save_print_profile", "overlaps",
            profile={"name": "X", "process": "fdm", "material": "pla",
                     "rows": [{"kind": "hole", "to_mm": 10,
                               "allowance_mm": 0.2},
                              {"kind": "hole", "from_mm": 5,
                               "allowance_mm": 0.3}]})
    refused(d, "save_print_profile", "would share a file",
            profile={"name": "test p1s", "process": "fdm",
                     "material": "pla"})
    # A Z gap is whole layers of a stated layer height (v175).
    refused(d, "save_print_profile", "needs the layer_height_mm",
            profile={"name": "X", "process": "fdm", "material": "pla",
                     "z_gap_mm": 0.2})
    refused(d, "save_print_profile", "accepted: 0.2 (1 layer) or 0.4",
            profile={"name": "X", "process": "fdm", "material": "pla",
                     "layer_height_mm": 0.2, "z_gap_mm": 0.3})
    refused(d, "set_print_process", "no print profile called 'Nope'",
            profile="Nope")
    expect([x["name"] for x in d.call("list_print_profiles")["profiles"]]
           == ["Test P1S"], "the refusals wrote nothing")

    r = d.call("print_allowance", kind="gap", size_mm=10, profile="Test P1S")
    expect(near(r["model_mm"], 10.2) and r["profile"] == "Test P1S",
           f"answered from the profile: {r}")
    # The row that answered is the profile's own, measured: its confirm
    # note names that source, not the built-in tables' "community
    # practice ... confirm with a coupon" -- which is still what a row the
    # profile copied from the defaults says.
    expect(r["source"] == "coupon" and "measured: coupon" in r["confirm"]
           and "community practice" not in r["confirm"],
           f"a measured row's confirm is its source: {r}")
    r = d.call("print_allowance", kind="hole", size_mm=10, profile="Test P1S")
    expect("community practice" in r["confirm"],
           f"a row copied from the defaults still asks for a coupon: {r}")

    # A part printed with it: a disc in a hole, at the profile's gap.
    d.call("add_part", name="Gap")
    box(d, (-15, -15, 0), (30, 30, 6), "Plate")
    cylinder(d, (0, 0, 0), "z", 5.1, 6, "Hole", mode="cut")
    cylinder(d, (0, 0, 0), "z", 5.0, 6, "Disc", mode="new_body")
    r = d.call("set_print_process", profile="Test P1S")
    expect(r["print"]["profile"] == "Test P1S" and
           r["print"]["match"] == "same", f"embedded: {r}")

    # Exported with no deflection: the profile's 0.01, and the gap survives.
    tmp = tempfile.mkdtemp(prefix="designer_print_stl_")
    stl = os.path.join(tmp, "gap.stl")
    e = d.call("export_stl", path=stl)
    expect(near(e["deflection_mm"], 0.01) and
           e["deflection_from"] == "profile" and "warning" not in e,
           f"meshed at the profile's chords: {e}")
    narrow, mean = gap_in_stl(stl, 5.0, 5.1)
    expect(abs(mean - 0.1) < 0.005,
           f"the STL's gap is the model's within 0.005: {mean:.4f}")
    expect(narrow > 0.1 - 0.01 - 1e-4,
           f"no chord closes more than 0.01 of it: {narrow:.4f}")
    # A coarse one asked for is exported as asked, and warned.
    e = d.call("export_stl", path=stl, stl_deflection_mm=0.1)
    expect(near(e["deflection_mm"], 0.1) and e["deflection_from"] == "asked",
           f"exported as asked: {e}")
    expect("100% of the smallest gap" in e.get("warning", ""),
           f"and warned: {e}")
    # Printed in TPU on the built-in table: its own chords, and said so.
    d.call("set_print_process", process="fdm", material="tpu")
    e = d.call("export_stl", path=stl)
    expect(near(e["deflection_mm"], 0.02) and
           e["deflection_from"] == "material",
           f"TPU meshes at its own chords: {e}")
    d.call("set_print_process", profile="Test P1S")

    # The store changes under the part: it keeps its copy, and says so.
    rows2 = [dict(x) for x in rows]
    rows2[-1]["allowance_mm"] = 0.3
    d.call("save_print_profile", profile={
        "name": "Test P1S", "process": "fdm", "material": "pla",
        "stl_deflection_mm": 0.01, "rows": rows2})
    g = d.call("get_print_process")["print"]
    expect(g["match"] == "differs" and
           any(x["kind"] == "gap" and near(x["allowance_mm"], 0.2)
               for x in g["rows"]), f"differs, reads its copy: {g}")
    # "Use mine": the same name again takes the store's copy.
    r = d.call("set_print_process", profile="Test P1S")
    expect(r["print"]["match"] == "same", f"use mine: {r}")
    d.call("delete_print_profile", name="Test P1S")
    g = d.call("get_print_process")["print"]
    expect(g["match"] == "missing", f"missing, still printed: {g}")
    d.call("save_print_profile", from_part=True)
    expect(d.call("get_print_process")["print"]["match"] == "same",
           "save it: the part's copy is now the store's")

    # The coupon records its answer into a profile.
    coupon = os.path.join(os.path.dirname(__file__), "..", "generators",
                          "print_coupon.py")
    out = subprocess.run(
        [sys.executable, coupon, "--port", str(port), "--record",
         "--profile", "Coupon P1S", "--process", "fdm", "--material", "pla",
         "--size", "8", "--gap-side", "0.15", "--bore", "8.25",
         "--thread-went", "yes"],
        capture_output=True, text=True, timeout=60)
    expect(out.returncode == 0, f"print_coupon --record: {out.stderr}")
    r = d.call("print_allowance", kind="gap", size_mm=8, profile="Coupon P1S")
    expect(near(r["allowance_mm"], 0.3) and "print_coupon" in r["source"],
           f"the recorded gap answers: {r}")
    r = d.call("print_allowance", kind="hole", size_mm=8,
               profile="Coupon P1S")
    expect(near(r["model_mm"], 8.25), f"the recorded bore: {r}")
    # The air row's answer: whole layers, kept as the profile's z_gap_mm.
    out = subprocess.run(
        [sys.executable, coupon, "--port", str(port), "--record",
         "--profile", "Coupon P1S", "--layer", "0.2", "--z-gap-layers", "2"],
        capture_output=True, text=True, timeout=60)
    expect(out.returncode == 0, f"print_coupon --record Z: {out.stderr}")
    mine = {x["name"]: x for x in
            d.call("list_print_profiles")["profiles"]}["Coupon P1S"]
    expect(near(mine.get("z_gap_mm", 0), 0.4) and
           near(mine.get("layer_height_mm", 0), 0.2),
           f"the recorded Z gap: {mine}")
    expect(near(d.call("print_allowance", kind="hole", size_mm=8,
                       profile="Coupon P1S")["model_mm"], 8.25),
           "recording the Z gap kept the rows")
    # The press (docs/tolerances/README.md#press-allowance): the built-in
    # default, to confirm; a profile with no press row reads it and says
    # so; the coupon's answer becomes the profile's own row.
    r = d.call("print_allowance", kind="press", size_mm=3, process="fdm",
               material="pla")
    expect(near(r["allowance_mm"], 0.0) and r["from"] == "builtin"
           and "DEFAULT to confirm" in r["source"]
           and near(r["interference_side_mm"], 0.0),
           f"the built-in press: {r}")
    d.call("save_print_profile", profile={
        "name": "No press", "process": "fdm", "material": "pla",
        # The printer's bed: the coupon's plate is checked against it.
        "bed_mm": [256, 256, 256],
        "rows": [{"kind": "gap", "allowance_mm": 0.2}]})
    r = d.call("print_allowance", kind="press", size_mm=3, profile="No press")
    expect(r["from"] == "builtin" and "has no press row" in r["note"]
           and near(r["model_mm"], 3.0), f"no press row of its own: {r}")
    refused(d, "print_allowance", "Accepted: hole, shaft, internal_thread, "
            "external_thread, gap, press", kind="pressfit", size_mm=3,
            profile="No press")
    out = subprocess.run(
        [sys.executable, coupon, "--port", str(port), "--record",
         "--profile", "No press", "--press-side", "0.025"],
        capture_output=True, text=True, timeout=60)
    expect(out.returncode == 0, f"print_coupon --record press: {out.stderr}")
    r = d.call("print_allowance", kind="press", size_mm=3, profile="No press")
    expect(r["from"] == "profile" and near(r["allowance_mm"], -0.05)
           and near(r["model_mm"], 2.95) and "note" not in r
           and "print_coupon" in r["source"], f"the recorded press: {r}")
    # The coupon's press row: every piece on the bed, checked, a cap a pin.
    tmp = tempfile.mkdtemp(prefix="designer_press_coupon_")
    out = subprocess.run(
        [sys.executable, coupon, "--port", str(port), "--press-only",
         "--profile", "No press", "--size", "3", "--out", tmp],
        capture_output=True, text=True, timeout=120)
    expect(out.returncode == 0, f"print_coupon --press-only: {out.stderr}"
                                f"{out.stdout}")
    row = json.load(open(os.path.join(tmp, "coupon_no_press_3_press.json")))
    bores = [c["bore_mm"] for c in row["caps"]]
    # Five steps of 0.1, two caps each: a step past a print's scatter, and
    # the scatter shown by the pair.
    expect(len(bores) == 10 and near(bores[4], 2.95) and near(bores[5], 2.95)
           and all(near(bores[2 * i], bores[2 * i + 1]) for i in range(5))
           and all(near(bores[2 * i + 2] - bores[2 * i], 0.1)
                   for i in range(4)),
           f"five steps of two caps round the profile's press: {bores}")
    out = subprocess.run(
        [sys.executable, coupon, "--port", str(port), "--press-only",
         "--profile", "No press", "--size", "3", "--press-step", "0.05",
         "--out", tmp], capture_output=True, text=True, timeout=120)
    expect(out.returncode != 0 and "0.1 mm at least" in
           out.stdout + out.stderr,
           f"a press step under the scatter is refused: {out.stderr}")
    expect(row["print"]["ok"] and row["print"]["stacking"]["status"]
           == "pass", f"the press row prints: {row['print']}")
    # --seed installs the two measured P1S profiles (and the TPU one).
    out = subprocess.run([sys.executable, coupon, "--port", str(port),
                          "--seed"], capture_output=True, text=True,
                         timeout=60)
    expect(out.returncode == 0, f"print_coupon --seed: {out.stderr}")
    names = {x["name"]: x for x in
             d.call("list_print_profiles")["profiles"]}
    for name, side in (("K5 PLA on P1S", 0.1), ("PLA on P1S", 0.15)):
        expect(name in names, f"seeded {name}: {names.keys()}")
        r = d.call("print_allowance", kind="gap", size_mm=10, profile=name)
        expect(near(r["allowance_mm"], 2 * side), f"{name}: {r}")
        expect(near(names[name].get("layer_height_mm", 0), 0.2) and
               "z_gap_mm" not in names[name],
               f"{name}: the preset's layer, no Z gap measured yet")
    # check_print's stacking takes its air from the profile: its z_gap_mm,
    # else three of its layers (docs/tolerances/README.md#print-profiles).
    d.call("add_part", name="Stacked")
    box(d, (0, 0, 0), (10, 10, 2), "Lower")
    box(d, (12, 0, 0), (10, 10, 2), "Beside")
    for name, air, src in (("Coupon P1S", 0.4, "profile"),
                           ("K5 PLA on P1S", 0.6, "layers")):
        d.call("set_print_process", profile=name)
        st = d.call("check_print")["stacking"]
        expect(near(st["limit_mm"], air) and st["limit_from"] == src
               and st["status"] == "pass" and "source" in st,
               f"{name}: the air is {air} from the {src}: {st}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_print_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")

        # --- Asking before there is a model --------------------------------
        r = d.call("check_fit", size=8.0,
                   hole={"class": "H7", "print": PETG})
        expect(near(r["print"]["hole"]["model"], 8.2), f"model Ø8.2: {r}")
        expect(r["print_notes"][0] ==
               "H7 on a printed Ø8 bore (fdm petg): model Ø8.2",
               f"the sentence: {r}")
        expect("finer than fdm holds" in r["print"]["hole"]["band_note"],
               f"H7 is finer than a print holds: {r}")
        expect("coupon" in r["print"]["hole"]["confirm"], f"confirm: {r}")

        r = d.call("print_allowance", kind="internal_thread",
                   designation="M8", **PETG)
        expect(near(r["model_mm"], 8.4) and near(r["pitch"], 1.25),
               f"a printed M8 nut thread: {r}")
        r = d.call("print_allowance", kind="gap", size_mm=10,
                   process="fdm", material="pla")
        expect(near(r["model_mm"], 10.5) and near(r["allowance_mm"], 0.5),
               f"a print-in-place gap round a Ø10 PLA pin, the default: {r}")
        r = d.call("print_allowance", kind="hole", size_mm=12, **PETG)
        expect(near(r["model_mm"], 12.3), f"a Ø12 hole: {r}")
        # Asked of a print, the chords are the ones export_stl meshes it at:
        # a tenth of PETG's 0.3 gap a side.
        expect(near(r["stl_deflection_mm"], 0.03),
               f"export_stl meshes a PETG print at 0.03: {r}")
        r = d.call("print_allowance", kind="hole", size_mm=12,
                   stl_deflection_mm=0.1, **PETG)
        expect(near(r["stl_chord_loss_mm"], 0.2),
               f"chords at 0.1 take up to 0.2: {r}")

        # --- Refused, naming what is accepted ------------------------------
        refused(d, "print_allowance", "Accepted: hole, shaft", kind="bore",
                size_mm=8, **PETG)
        refused(d, "print_allowance", "Accepted: fdm", kind="hole",
                size_mm=8, process="sla", material="petg")
        refused(d, "print_allowance", "Accepted for fdm: petg, pla",
                kind="hole", size_mm=8, process="fdm", material="abs")
        refused(d, "check_fit", "Accepted for fdm: petg, pla", size=8.0,
                hole={"class": "H7",
                      "print": {"process": "fdm", "material": "abs"}})

        # --- A printed bracket and a steel pin ------------------------------
        pf, pname = pin(d, "Pin", 8.0)
        d.call("set_tolerance", feature_id=pf, element=pname,
               **{"class": "g6"})
        # Modelled Ø8.2 because it is printed: that prints Ø8.
        bf, bname = bored_part(d, "Bracket", 8.2)
        refused(d, "set_print_process", "needs both", process="fdm")
        refused(d, "set_print_process", "Accepted: fdm", process="sls",
                material="petg")
        refused(d, "set_print_process", "Accepted for fdm: petg, pla",
                process="fdm", material="abs")
        state0 = d.call("get_state")
        r = d.call("set_print_process", **PETG)
        pair = {k: r["print"][k] for k in ("process", "material")}
        expect(pair == PETG and "profile" not in r["print"],
               f"declared, the built-in table: {r}")
        g = d.call("get_print_process")["print"]
        expect({k: g[k] for k in ("process", "material")} == PETG,
               "reads back")
        expect(len(d.call("get_state")["features"]) ==
               len(state0["features"]), "rebuilds nothing, adds nothing")
        d.call("set_tolerance", feature_id=bf, element=bname,
               **{"class": "H7"})

        r = d.call("check_fit", hole={"class": "H7", "feature_id": bf,
                                      "element": bname})
        expect(near(r["size"], 8.0) and r["size_from"] == "printed",
               f"the bore is at what it prints: {r}")
        expect(r["print"]["hole"]["compensated"], f"compensated: {r}")

        # One measured face in each part: the fit is at Ø8, not two sizes.
        r = d.call("check_fit",
                   hole={"class": "H7", "feature_id": bf, "element": bname,
                         "part": "Bracket"},
                   shaft={"class": "g6", "feature_id": pf, "element": pname,
                          "part": "Pin"})
        expect(near(r["size"], 8.0) and r["kind"] == "clearance",
               f"H7/g6 at the printed Ø8: {r}")
        expect("shaft" not in r["print"], f"the steel pin is not printed: {r}")

        # --- check_fits off the mate --------------------------------------
        d.call("add_assembly", name="Joint")
        ob = d.call("add_occurrence", part="Bracket")["id"]
        op = d.call("add_occurrence", part="Pin")["id"]
        d.call("add_mate", type="revolute", name="Pivot",
               a={"occurrence": ob, "face": {"feature_id": bf,
                                             "element": bname}},
               b={"occurrence": op, "face": {"feature_id": pf,
                                             "element": pname}})
        fit = d.call("check_fits")["fits"][0]
        expect(fit["status"] == "pass", f"H7/g6 turns: {fit}")
        expect(near(fit["size"], 8.0), f"at the printed size: {fit}")
        expect(near(fit["hole"]["print"]["model"], 8.2),
               f"and says what to model: {fit}")

        # A printed bore modelled at the BARE size is told what to model.
        bf2, bname2 = bored_part(d, "Bare", 8.0)
        d.call("set_print_process", **PETG)
        d.call("set_tolerance", feature_id=bf2, element=bname2,
               **{"class": "H7"})
        refused(d, "check_fit", "model the hole 8.2",
                hole={"class": "H7", "feature_id": bf2, "element": bname2},
                shaft={"class": "g6", "feature_id": pf, "element": pname,
                       "part": "Pin"})
        d.call("set_active_part", name="Joint")
        ob2 = d.call("add_occurrence", part="Bare")["id"]
        d.call("add_mate", type="revolute", name="Bare pivot",
               a={"occurrence": ob2, "face": {"feature_id": bf2,
                                              "element": bname2}},
               b={"occurrence": op, "face": {"feature_id": pf,
                                             "element": pname}})
        bare = [f for f in d.call("check_fits")["fits"]
                if f["mate_name"] == "Bare pivot"][0]
        expect(bare["status"] == "unverified", f"two basic sizes: {bare}")
        expect("model the bore Ø8.2" in bare["reason"],
               f"and the reason says what to model: {bare}")

        # --- A printed thread ---------------------------------------------
        tf, tname = bored_part(d, "Nut", 6.75)  # M8's tap drill
        d.call("set_print_process", **PETG)
        r = d.call("add_feature", feature={
            "type": "Thread", "name": "M8", "designation": "M8",
            "face": {"feature_id": tf, "element": tname}})
        pr = r["print"]
        expect(pr["side"] == "internal" and pr["known"], f"internal: {pr}")
        expect(near(pr["model_major"], 8.4), f"model the major Ø8.4: {pr}")
        expect(near(pr["prints_major"], 7.6), f"as modelled: {pr}")
        expect(near(pr["model_face"], 6.647 + 0.4, 1e-3),
               f"as a custom thread's bore: {pr}")
        expect(any("cosmetic" in n for n in pr["notes"]),
               f"a cosmetic thread prints nothing: {pr}")
        threads = d.call("get_print_process")["threads"]
        expect(len(threads) == 1, f"listed with the part: {threads}")

        # Follow the advice: a custom thread on the bore it names prints the
        # M8's major.
        nut2 = bored_part(d, "Nut2", round(pr["model_face"], 3))
        d.call("set_print_process", **PETG)
        r = d.call("add_feature", feature={
            "type": "Thread", "name": "Printed M8",
            "designation": "printed nut thread", "pitch": 1.25,
            "face": {"feature_id": nut2[0], "element": nut2[1]}})
        pr = r["print"]
        expect(not pr["known"], f"a custom thread: {pr}")
        expect(near(pr["prints_major"], 8.0, 1e-3),
               f"the advised custom thread prints the M8 major: {pr}")

        # Undo takes the process back off.
        d.call("undo")
        d.call("undo")
        expect(d.call("get_print_process")["print"] is None,
               "undo clears the process")

        profiles(d, app.port)

        print("print_allowance_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
