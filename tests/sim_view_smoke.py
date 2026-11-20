#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The simulation viewport's pictures: is the mesh drawn, and does the legend's
clamp colour what it says? (docs/simulation/README.md#the-mesh-you-look-at,
#results-display.)

The mesh view and the result overlay are drawn by one renderer whose work no
unit test sees, and a display RPC that silently drew nothing would pass every
headless check there is. So each claim is read off the PIXELS of a screenshot,
as a property that holds whatever the mesher did -- never a golden image,
because Netgen's mesh is not bit-stable across versions:

  - `edges`: the body turns into a plain grey one crossed by dark element
    edges, which the shaded part has none of;
  - `quality` and `size`: the body takes the colour map's colours;
  - a section through the mesh is made of whole elements, so the cut is not
    the flat cap a section through the body is: it differs from the body's
    section, and still shows edges;
  - a range clamp at half the peak paints the over colour, which the
    unclamped field has none of; "above yield" on a beam far under yield
    paints none;

The legend and the markers are overlays, which an offscreen instance does not
build; the GUI suite's `sim/a_result_field_is_clamped_and_marked` has those.

Needs a real renderer: --offscreen has one without a window or a display.
Skips (77) without a Vulkan device, without Netgen, or without pillow.

    tests/sim_view_smoke.py <designer> [--shots DIR]   # also keep the PNGs
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
import smoke_shapes as shapes  # noqa: E402

try:
    from PIL import Image
except ImportError:
    print("sim_view_smoke: pillow not installed — skipping")
    sys.exit(77)

SKIP = 77


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def census(path):
    """How many pixels of each kind the picture has.

    steps    -- places along a row where the brightness drops sharply: the
                element edges cross a lit body hundreds of times a row, a
                shaded part's silhouette twice
    over     -- the legend's over colour, a pink no colour-map colour is
    coloured -- saturated pixels: the colour map
    """
    img = Image.open(path).convert("RGB")
    w, h = img.size
    px = list(img.get_flattened_data()) if hasattr(
        img, "get_flattened_data") else list(img.getdata())
    steps = over = coloured = 0
    for y in range(h):
        prev = None
        for x in range(w):
            r, g, b = px[y * w + x]
            hi, lo = max(r, g, b), min(r, g, b)
            if prev is not None and prev - hi > 45:
                steps += 1
            prev = hi
            if r > 200 and b > 170 and g < 150 and r - g > 80:
                over += 1
            elif hi - lo > 90:
                coloured += 1
    return {"steps": steps, "over": over, "coloured": coloured,
            "pixels": w * h}


def main():
    binary = sys.argv[1]
    keep = None
    if "--shots" in sys.argv:
        keep = sys.argv[sys.argv.index("--shots") + 1]
        os.makedirs(keep, exist_ok=True)
    scratch = tempfile.mkdtemp(prefix="lobole_sim_view_")
    env = dict(os.environ, XDG_STATE_HOME=scratch,
               XDG_CONFIG_HOME=os.path.join(scratch, "config"))
    try:
        app = launch_app(binary, extra_args=["--offscreen"], env=env,
                         headless=False)
    except RuntimeError as e:
        if "no usable Vulkan device" in str(e) or "VkResult" in str(e):
            print(f"sim_view_smoke: no usable Vulkan device ({e}) — skipping")
            return SKIP
        raise
    try:
        d = Client.wait_for(app.port, timeout=120)
        if not d.call("ping").get("gpu", ""):
            print("sim_view_smoke: instance reports no GPU — skipping")
            return SKIP
        d.call("set_settings", converge_auto_s=0.0)

        def shot(name):
            path = os.path.join(keep or scratch, name + ".png")
            d.call("screenshot", path=path)  # the first settles the frame
            d.call("screenshot", path=path)
            return census(path)

        # A steel bracket plate with a bore, held at one end and pulled at
        # the other: flat faces, a curved one, and a stress concentration.
        d.call("new_project")
        plate = shapes.box(d, (0, 0, 0), (100, 40, 8), "Plate")["id"]
        d.call("set_material", feature_id=plate, material="steel")
        hole = shapes.cylinder(d, (50, 20, -1), "z", 6.0, 10.0, "Hole")["id"]
        d.call("add_feature", feature={"type": "Boolean", "name": "Cut",
                                       "op": "subtract", "target_id": plate,
                                       "tool_id": hole})
        body = d.call("list_elements")["bodies"][0]
        face = {tuple(round(c) for c in f["normal"]): f["name"]
                for f in body["faces"] if f.get("kind", "plane") == "plane"}
        created_by = body["created_by"]
        study = d.call("add_study", name="Pull")["id"]
        d.call("add_load", study=study, type="fixed",
               face={"feature_id": created_by, "element": face[(-1, 0, 0)]})
        d.call("add_load", study=study, type="force", magnitude=4000.0,
               direction=[1, 0, 0],
               face={"feature_id": created_by, "element": face[(1, 0, 0)]})
        d.call("enter_simulation", study=study)
        d.call("set_camera", target=[50, 20, 4], direction=[0.5, -0.6, 0.62],
               distance=190.0)
        body_only = shot("01-body")

        try:
            mesh = d.call("mesh_study", id=study)
        except RuntimeError as e:
            if "without FEM support" in str(e):
                print("sim_view_smoke: built without Netgen — skipping")
                return SKIP
            raise
        elements = mesh["bodies"][0]["elements"]
        expect(elements > 100, f"a mesh of {elements} elements")

        # --- The mesh, drawn.
        d.call("set_mesh_display", study=study, view="edges")
        edges = shot("02-mesh-edges")
        expect(edges["steps"] > 4 * body_only["steps"] + 2000,
               f"element edges are drawn over the body ({edges['steps']} "
               f"brightness steps against {body_only['steps']} without)")
        expect(edges["coloured"] < 0.002 * edges["pixels"],
               "and the body under them is plain")
        d.call("set_mesh_display", study=study, view="quality")
        quality = shot("03-mesh-quality")
        expect(quality["coloured"] > 0.03 * quality["pixels"],
               f"coloured by quality ({quality['coloured']} pixels)")
        expect(quality["steps"] > 4 * body_only["steps"] + 2000,
               "with its edges still on")
        d.call("set_mesh_display", study=study, view="size")
        size = shot("04-mesh-size")
        expect(size["coloured"] > 0.03 * size["pixels"], "coloured by size")

        # --- A section through it: whole elements, not a flat cap.
        d.call("set_mesh_display", study=study, view="off")
        d.call("set_section", base_plane="xz", offset=20.0)
        cut_body = shot("05-section-body")
        d.call("set_mesh_display", study=study, view="quality")
        cut_mesh = shot("06-section-mesh")
        expect(cut_mesh["coloured"] > 0.03 * cut_mesh["pixels"]
               and cut_mesh["steps"] > 4 * cut_body["steps"] + 2000,
               "the cut face is made of coloured elements with their edges")
        d.call("set_section", enabled=False)
        d.call("set_mesh_display", study=study, view="off")

        # --- A result, its range clamped.
        res = d.call("run_study", id=study, converge="never")
        peak = res["max_von_mises_mpa"]["value"]
        expect(peak > 0, f"solved: peak {peak:.1f} MPa")
        d.call("set_result_display", study=study, field="von_mises",
               markers=False)
        whole = shot("07-von-mises")
        expect(whole["coloured"] > 0.05 * whole["pixels"]
               and whole["over"] < 30,
               "the whole field, nothing over its own range")
        d.call("set_result_display", study=study, field="von_mises",
               range_max=0.5 * peak, markers=False)
        clamped = shot("08-clamped")
        expect(clamped["over"] > 200,
               f"clamped at half the peak, what is past it takes the over "
               f"colour ({clamped['over']} pixels)")
        d.call("set_result_display", study=study, field="von_mises",
               range_max=250.0, markers=False)
        yielded = shot("09-above-yield")
        expect(yielded["over"] < 30,
               "topped at steel's yield, nothing on this plate is over")
        d.call("set_result_display", study=study, field="principal_max",
               range_max=None)
        shot("10-principal")
        d.call("set_result_display", study=study, field="factor_of_safety")
        shot("11-factor-of-safety")
        d.call("set_result_display", study=study, field="displacement",
               deform_scale=200.0)
        shot("12-displacement-deformed")
    finally:
        app.terminate()
        app.wait(timeout=20)
    print("sim_view_smoke: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
