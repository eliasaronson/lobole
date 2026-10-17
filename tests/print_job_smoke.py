#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: slicer settings, the printed mass and export_print_job.

docs/tolerances/README.md#slicer-settings, #printed-mass, #print-jobs.
Headless. The settings refused where the slicer would refuse them, and a
refusal changes nothing; a printed mass beside the solid one; a two-plate
job exported and read back, its per-object settings in Bambu's metadata;
what a job refuses before it writes anything (no config for two plates, a
piece in the bed's keep-out corner, two filaments in one job).

Then, when Bambu Studio's CLI is installed ($LOBOLE_BAMBU_STUDIO,
`bambu-studio` on PATH, or ~/.local/state/nix-roots/bambu-studio), the
job is SLICED and each object's walls, shells and infill are read back out
of the G-code. Without it that half is skipped, and the test says so.

Usage: print_job_smoke.py <path-to-designer-binary>
"""
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROFILE = {
    "name": "Smoke P1S", "process": "fdm", "material": "pla",
    "layer_height_mm": 0.2, "bed_mm": [256, 256, 250],
    "bed_exclude_mm": [[0, 0, 18, 28]], "line_width_mm": 0.5}
# A config with every key a project needs, for the read-back half: what
# Bambu does with it is the CLI half's question.
CONFIG = {"printer_settings_id": "Bambu Lab P1S 0.4 nozzle",
          "print_settings_id": "0.20mm Standard @BBL X1C",
          "filament_settings_id": ["Bambu PLA Basic @BBL P1S 0.4 nozzle"],
          "nozzle_diameter": ["0.4"], "printable_height": "250",
          "printable_area": ["0x0", "256x0", "256x256", "0x256"],
          "bed_exclude_area": ["0x0", "18x0", "18x28", "0x28"],
          "filament_type": ["PLA"], "layer_height": "0.2"}


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **kw):
    before = d.call("get_print_settings", **(
        {"part": kw["part"]} if "part" in kw else {}))
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refused, but not naming "
                                 f"'{needle}': {e}")
        after = d.call("get_print_settings", **(
            {"part": kw["part"]} if "part" in kw else {}))
        expect(before == after, f"a refused {method} changed the settings")
        return str(e)
    raise AssertionError(f"{method} was accepted: {kw}")


def plate(d, name, pieces):
    """A plate part: a Derived copy of each (part, x, y) at that place."""
    ids = {p["name"]: p["id"] for p in d.call("list_parts")["parts"]}
    d.call("add_part", name=name)
    d.call("set_print_process", profile=PROFILE["name"])
    for part, x, y in pieces:
        r = d.call("add_feature", feature={
            "type": "Derived", "name": part, "source_part": ids[part],
            "reference": False,
            "placement": {"position": [x, y, 0.0],
                          "orientation": [0, 0, 0, 1]}})
        expect(r.get("regen", {}).get("ok", True), f"{name}: {r}")


def find_bambu():
    env = os.environ.get("LOBOLE_BAMBU_STUDIO")
    if env and os.path.exists(env):
        return env
    if shutil.which("bambu-studio"):
        return shutil.which("bambu-studio")
    root = os.path.expanduser(
        "~/.local/state/nix-roots/bambu-studio/bin/bambu-studio")
    return root if os.path.exists(root) else None


def gcode_objects(path):
    """Per object (its G-code label; 0 when a plate has one) and layer:
    each feature's extrusion runs and their E."""
    layers = collections.defaultdict(
        lambda: collections.defaultdict(lambda: [0, 0.0]))
    labelled = False
    obj, feat, z, run = None, None, 0.0, False
    with open(path) as f:
        text = f.read()
    labelled = "; start printing object, unique label id:" in text
    if not labelled:
        obj = 0
    for line in text.splitlines():
        if line.startswith("; start printing object, unique label id:"):
            obj = int(line.split(":")[1])
            continue
        if line.startswith("; stop printing object"):
            obj = None
            continue
        if line.startswith("; FEATURE:"):
            feat = line.split(":", 1)[1].strip()
            run = False
            continue
        if line.startswith("; Z_HEIGHT:"):
            z = round(float(line.split(":")[1]), 3)
            continue
        if obj is None or not line.startswith(("G1 ", "G2 ", "G3 ")):
            continue
        m = re.search(r" E(-?[\d.]+)", line)
        xy = re.search(r" [XY]", line)
        e = float(m.group(1)) if m else 0.0
        if e > 0 and xy:
            s = layers[(obj, z)][feat]
            if not run:
                s[0] += 1
            s[1] += e
            run = True
        elif xy:
            run = False
    out = collections.defaultdict(dict)
    for (o, zz), feats in layers.items():
        out[o][zz] = {k: tuple(v) for k, v in feats.items()}
    return out


def walls_at_middle(layers):
    zs = sorted(layers)
    mid = layers[zs[len(zs) // 2]]
    return (mid.get("Outer wall", (0, 0))[0] +
            mid.get("Inner wall", (0, 0))[0])


def sparse_e(layers):
    zs = sorted(layers)
    return layers[zs[len(zs) // 2]].get("Sparse infill", (0, 0.0))[1]


def solid_layers(layers):
    """Layers at the bottom and at the top before sparse infill starts."""
    zs = sorted(layers)
    sparse = [i for i, z in enumerate(zs) if "Sparse infill" in layers[z]]
    if not sparse:
        return None, None
    return sparse[0], len(zs) - 1 - sparse[-1]


def slice_job(bambu, job, outdir):
    os.makedirs(outdir, exist_ok=True)
    r = subprocess.run([bambu, "--slice", "0", "--outputdir", outdir, job],
                       capture_output=True, text=True, timeout=600)
    res = os.path.join(outdir, "result.json")
    expect(os.path.exists(res), f"Bambu made no result ({r.returncode}): "
                                f"{r.stderr[-800:]}")
    with open(res) as f:
        result = json.load(f)
    expect(result.get("return_code") == 0,
           f"Bambu did not slice the job: {result}")
    return result


def cli_half(d, tmp, bambu):
    """Slice real jobs and read every object's settings out of the G-code."""
    cfg = {}
    for fil, preset in (("pla", "Bambu PLA Basic @BBL P1S 0.4 nozzle"),
                        ("tpu", "Bambu TPU 95A HF @BBL P1S")):
        out = os.path.join(tmp, f"{fil}.json")
        r = subprocess.run(
            [sys.executable, os.path.join(ROOT, "tools", "bambu-config.py"),
             "--printer", "Bambu Lab P1S 0.4 nozzle",
             "--process", "0.20mm Standard @BBL X1C", "--filament", preset,
             "-o", out], capture_output=True, text=True, timeout=600,
            env=dict(os.environ, LOBOLE_BAMBU_STUDIO=bambu))
        expect(r.returncode == 0, f"bambu-config: {r.stderr}")
        cfg[fil] = out

    # The PLA job, two plates: a pendulum bob at 100 % rectilinear with
    # three walls beside a block at 15 % gyroid with two, and a plate of
    # four walls.
    pla = os.path.join(tmp, "pla_job.3mf")
    r = d.call("export_print_job", path=pla, slicer_config=cfg["pla"],
               plates=[{"part": "plate 1"}, {"part": "plate 2"}])
    expect(r["form"] == "project", r)
    result = slice_job(bambu, pla, os.path.join(tmp, "pla_out"))
    expect(len(result["sliced_plates"]) == 2,
           f"two plates sliced: {result['sliced_plates']}")
    g1 = gcode_objects(os.path.join(tmp, "pla_out", "plate_1.gcode"))
    g2 = gcode_objects(os.path.join(tmp, "pla_out", "plate_2.gcode"))
    # Objects are labelled 100 + their index in the job.
    bob, block = g1[101], g1[102]
    expect(walls_at_middle(bob) == 3, f"the bob prints 3 walls: {bob}")
    expect(walls_at_middle(block) == 2, f"the block prints 2: {block}")
    expect(walls_at_middle(g2[0]) == 4, f"plate 2's prints 4: {g2[0]}")
    # Same core area per layer, so the infill's E is the density's.
    ratio = sparse_e(bob) / max(sparse_e(block), 1e-9)
    expect(ratio > 4.0, f"100 % against 15 % infill: {ratio:.2f} x the E")
    text = open(os.path.join(tmp, "pla_out", "plate_1.gcode")).read()
    expect("; FEATURE: Brim" not in text, "no brim")
    expect("; FEATURE: Support" not in text, "no supports")
    print(f"  CLI: PLA job sliced, 2 plates; walls 3 / 2 / 4 from the "
          f"G-code, the 100 % bob's infill {ratio:.1f} x the 15 % block's, "
          f"no brim, no supports")

    # The TPU job: the gasket's settings, 1 wall, 2/2 shells, 15 % gyroid,
    # beside a TPU block at the preset's.
    tpu = os.path.join(tmp, "tpu_job.3mf")
    d.call("export_print_job", path=tpu, slicer_config=cfg["tpu"],
           plates=[{"part": "tpu plate"}])
    result = slice_job(bambu, tpu, os.path.join(tmp, "tpu_out"))
    gt = gcode_objects(os.path.join(tmp, "tpu_out", "plate_1.gcode"))
    gasket, other = gt[101], gt[102]
    expect(walls_at_middle(gasket) == 1, f"the gasket prints 1 wall")
    expect(walls_at_middle(other) == 2, f"the preset's 2 walls: {other}")
    bottom, top = solid_layers(gasket)
    expect((bottom, top) == (2, 2),
           f"the gasket's shells are 2 and 2 layers: {bottom}, {top}")
    pb, pt = solid_layers(other)
    expect((pb, pt) == (3, 5), f"the preset's 3/5: {pb}, {pt}")
    head = open(os.path.join(tmp, "tpu_out", "plate_1.gcode")).read(200000)
    expect("; filament_type = TPU" in head, "the TPU job slices as TPU")
    print(f"  CLI: TPU job sliced; the gasket 1 wall and 2/2 shells, its "
          f"neighbour the preset's 2 walls and 3/5")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_print_job_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        d.call("save_print_profile", profile=PROFILE)

        # --- the settings --------------------------------------------------
        d.call("add_part", name="bob")
        d.call("set_print_process", profile=PROFILE["name"])
        box(d, (0, 0, 0), (20, 20, 10), name="Bob")
        refused(d, "set_print_settings", "cannot print 100 %",
                infill_pct=100, infill_pattern="grid")
        msg = refused(d, "set_print_settings", "accepted",
                      infill_pattern="zigzag")
        expect("rectilinear" in msg and "gyroid" in msg,
               f"an unknown pattern names the patterns: {msg}")
        refused(d, "set_print_settings", "pattern stated", infill_pct=100)
        refused(d, "set_print_settings", "at least 1", walls=0)
        r = d.call("set_print_settings", walls=3, top_layers=5,
                   bottom_layers=5, infill_pct=100,
                   infill_pattern="rectilinear")
        expect(r["part"]["infill_pattern"] == "rectilinear", r)
        expect(r["bodies"][0]["described"].startswith("walls 3"), r)

        # --- the printed mass ------------------------------------------------
        m = d.call("mass_properties")
        b = m["bodies"][0]
        expect(b["printed"] and b["printed"].get("mass_g"),
               f"a printed part has a printed mass: {b}")
        # Solid all through, the printed bob is the solid one; with no
        # material of its own it weighs as the filament, the process's PLA.
        pv, sv = b["printed"]["volume_mm3"], b["volume_mm3"]
        expect(abs(pv - sv) < 1e-6 * sv, f"100 % is the solid: {pv} {sv}")
        expect(b["printed"]["density_from"] == "the filament, pla",
               b["printed"])
        # Solid all through it is the solid, exactly, and says so.
        expect(not b["printed"]["estimate"] and
               "exactly" in b["printed"]["method"], b["printed"])

        d.call("add_part", name="block")
        d.call("set_print_process", profile=PROFILE["name"])
        box(d, (0, 0, 0), (20, 20, 10), name="Block")
        d.call("set_print_settings", walls=2, top_layers=3, bottom_layers=3,
               infill_pct=15, infill_pattern="gyroid")
        m = d.call("mass_properties")["bodies"][0]
        # By hand, 0.5 mm walls, layers of 0.2: 20 x 20 x 10 with 1.2 mm of
        # solid top and bottom, a 1 mm ring, the 18 x 18 core at 15 %.
        shell = 400 * 1.2 + (400 - 324) * 8.8
        want = shell + 0.15 * 324 * 8.8
        got = m["printed"]["volume_mm3"]
        expect(abs(got - want) < 1e-6 * want,
               f"the block's printed volume {got} against {want} by hand")
        expect(m["printed"]["estimate"], "an infill is an estimate")
        unstated = None
        d.call("add_part", name="plain")
        d.call("set_print_process", profile=PROFILE["name"])
        box(d, (0, 0, 0), (10, 10, 10), name="Plain")
        unstated = d.call("mass_properties")["bodies"][0]["printed"]
        expect("walls" in unstated.get("why", ""),
               f"an unstated setting is named: {unstated}")
        d.call("set_print_settings", walls=4)

        # --- an assembly's printed mass ---------------------------------------
        # The block printed, beside a steel block that is not: the total
        # weighs each as it is made and says which.
        block_printed = m["printed"]["mass_g"]
        d.call("add_part", name="steel")
        sb = box(d, (0, 0, 0), (10, 10, 10), name="Steel")
        d.call("set_material", feature_id=sb["id"], material="steel")
        steel = d.call("mass_properties")["total"]
        d.call("add_assembly", name="printed rig")
        d.call("add_occurrence", part="block", fixed=True)
        d.call("add_occurrence", part="steel", fixed=True, transform={
            "position": [40.0, 0.0, 0.0], "orientation": [0, 0, 0, 1]})
        am = d.call("mass_properties")
        rows = {r["name"]: r for r in am["occurrences"]}
        expect(rows["block"]["mass_from"] == "printed" and
               abs(rows["block"]["printed"]["mass_g"] - block_printed)
               < 1e-9 * block_printed, f"the block weighs printed: {rows}")
        expect(rows["steel"]["mass_from"] == "solid" and
               "printed" not in rows["steel"], f"the steel solid: {rows}")
        tp = am["total"]["printed"]
        want = block_printed + steel["mass_g"]
        expect(abs(tp["mass_g"] - want) < 1e-9 * want,
               f"the total is the block printed and the steel solid: {tp} "
               f"against {want}")
        expect(tp["printed"] == ["block"] and tp["solid"] == ["steel"], tp)
        # The centre: the printed block's (10, 10, 5) and the steel's
        # (45, 5, 5), by their masses; the solid total stays the solid's.
        cx = (block_printed * 10.0 + steel["mass_g"] * 45.0) / want
        expect(abs(tp["center_of_mass_mm"][0] - cx) < 1e-3,
               f"the centre by the printed masses: {tp} against x {cx}")
        expect(am["total"]["mass_g"] is None or
               am["total"]["mass_g"] != tp["mass_g"],
               f"the solid total is not the printed one: {am['total']}")
        # Inertia about the centre, in g mm^2: the steel's own (m a^2 / 6
        # for a cube), the printed block's, and both carried to the centre.
        I = tp["inertia_g_mm2"]
        expect(len(I) == 3 and I[0][1] == I[1][0], f"a symmetric tensor: {I}")
        own_steel = steel["mass_g"] * 100.0 / 6.0
        expect(I[1][1] > own_steel + steel["mass_g"] * (45.0 - cx) ** 2,
               f"Iyy holds the steel's own and its offset: {I}")

        # --- the job ------------------------------------------------------------
        plate(d, "plate 1", [("bob", 60, 60), ("block", 120, 60)])
        plate(d, "plate 2", [("plain", 120, 120)])
        out = os.path.join(tmp, "job.3mf")
        try:
            d.call("export_print_job", path=out,
                   plates=[{"part": "plate 1"}, {"part": "plate 2"}])
            raise AssertionError("two plates without a config exported")
        except RuntimeError as e:
            expect("slicer_config" in str(e), f"names slicer_config: {e}")
        expect(not os.path.exists(out), "a refused job wrote nothing")
        one = d.call("export_print_job", path=out,
                     plates=[{"part": "plate 1", "name": "the bob's"}])
        expect(one["form"] == "model", one)
        r = d.call("export_print_job", path=out, slicer_config=CONFIG,
                   plates=[{"part": "plate 1"}, {"part": "plate 2"}])
        expect(r["form"] == "project" and r["filament"] == "pla", r)
        with zipfile.ZipFile(out) as z:
            ms = z.read("Metadata/model_settings.config").decode()
            model = z.read("3D/3dmodel.model").decode()
            proj = json.loads(z.read("Metadata/project_settings.config"))
            rec = json.loads(z.read("Metadata/lobole_job.json"))
        objects = re.findall(r'<object id="(\d+)">(.*?)</object>', ms, re.S)
        expect(len(objects) == 3, f"one object per piece: {len(objects)}")
        meta = [dict(re.findall(r'key="([^"]+)" value="([^"]*)"',
                                o[1].split("<part")[0])) for o in objects]
        expect(meta[0]["wall_loops"] == "3" and
               meta[0]["sparse_infill_density"] == "100%" and
               meta[0]["sparse_infill_pattern"] == "zig-zag", meta[0])
        expect(meta[1]["sparse_infill_pattern"] == "gyroid" and
               meta[1]["top_shell_thickness"] == "0", meta[1])
        expect(meta[2]["wall_loops"] == "4" and
               "sparse_infill_density" not in meta[2], meta[2])
        for mm in meta:
            expect(mm["brim_type"] == "no_brim" and
                   mm["enable_support"] == "0" and
                   mm["layer_height"] == "0.2", mm)
        expect(ms.count("<plate>") == 2, "two plates")
        expect('transform="1 0 0 0 1 0 0 0 1 307.2 0 0"' in model,
               "plate 2 sits where Bambu's plate grid has it")
        expect("BambuStudio-" in model, "a Bambu project")
        expect(proj["printer_settings_id"] == CONFIG["printer_settings_id"],
               proj)
        expect(len(rec["plates"]) == 2, rec)
        print(f"  job: {len(objects)} objects on 2 plates, each with its "
              f"settings, read back")

        # --- what a job refuses ---------------------------------------------
        plate(d, "corner", [("plain", 2, 2)])
        cp = d.call("check_print")
        bf = cp["bed_fit"]
        expect(bf["status"] == "fail" and bf["intrusions"] and
               bf["intrusions"][0]["zone"] == 0,
               f"check_print fails the piece in the keep-out zone: {bf}")
        bad = os.path.join(tmp, "corner.3mf")
        try:
            d.call("export_print_job", path=bad, plates=[{"part": "corner"}])
            raise AssertionError("a piece in the keep-out zone exported")
        except RuntimeError as e:
            expect("keep-out zone" in str(e), f"names the zone: {e}")
        expect(not os.path.exists(bad), "nothing written")
        # A TPU piece on a PLA plate: two spools in one job.
        d.call("add_part", name="gasket")
        d.call("set_print_process", profile=PROFILE["name"])
        g = box(d, (0, 0, 0), (20, 20, 10), name="Gasket")
        d.call("set_material", feature_id=g["id"], material="tpu")
        d.call("set_print_settings", walls=1, top_layers=2, bottom_layers=2,
               infill_pct=15, infill_pattern="gyroid")
        d.call("add_part", name="tpu block")
        d.call("set_print_process", profile=PROFILE["name"])
        t = box(d, (0, 0, 0), (20, 20, 10), name="TPU block")
        d.call("set_material", feature_id=t["id"], material="tpu")
        plate(d, "mixed", [("bob", 60, 60), ("gasket", 120, 60)])
        try:
            d.call("export_print_job", path=bad, plates=[{"part": "mixed"}])
            raise AssertionError("two filaments in one job exported")
        except RuntimeError as e:
            expect("one spool" in str(e), f"names the spools: {e}")
        expect(not os.path.exists(bad), "nothing written")
        plate(d, "tpu plate", [("gasket", 60, 60), ("tpu block", 120, 60)])
        print("  refused: two plates without a config, a piece in the "
              "keep-out zone, two filaments in one job; nothing written")

        # --- the slicer --------------------------------------------------------
        bambu = find_bambu()
        if bambu is None:
            print("  CLI: SKIPPED, no Bambu Studio installed (set "
                  "LOBOLE_BAMBU_STUDIO): the job was read back, not sliced")
        else:
            cli_half(d, tmp, bambu)
        print("print_job_smoke: ok")
    finally:
        app.terminate()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
