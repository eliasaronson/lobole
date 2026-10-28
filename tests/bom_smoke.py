#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: A4 bill of materials, headless over JSON-RPC.

Two parts (one with a density-bearing material, one without), three
occurrences. Checks: grouping by part with quantities, unit/total mass from
material density, the null-mass taint (a material-less part makes its row and
the grand total null), refreshed rows after a material assignment, and the
CSV export. Also re-checks the refactored mass_properties contract (the BOM
shares its math).

Usage: bom_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_bom_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        # --- block: 10x10x10 steel cube; pin: 2x2x2, no material ------------
        d.call("new_project")
        d.call("add_part", name="block")
        blk = box(d, (0, 0, 0), (10, 10, 10), "Blk")
        d.call("set_material", feature_id=blk["id"], material="steel")
        mp = d.call("mass_properties")  # refactored path: same numbers as BOM
        expect(abs(mp["total"]["volume_mm3"] - 1000.0) < 1e-6,
               f"block volume: {mp['total']}")
        steel_g = mp["total"]["mass_g"]
        expect(steel_g and steel_g > 0, f"steel block has a mass: {mp['total']}")

        d.call("add_part", name="pin")
        box(d, (0, 0, 0), (2, 2, 2), "Pin")

        d.call("add_assembly", name="rig")
        d.call("add_occurrence", part="block", fixed=True)
        d.call("add_occurrence", part="pin",
               transform={"position": [20, 0, 0], "orientation": [0, 0, 0, 1]})
        d.call("add_occurrence", part="pin",
               transform={"position": [40, 0, 0], "orientation": [0, 0, 0, 1]})

        # --- grouped rows, quantities, the null-mass taint ------------------
        bom = d.call("bom")
        rows = {r["part"]: r for r in bom["rows"]}
        expect(len(bom["rows"]) == 2, f"two rows: {bom['rows']}")
        expect(rows["block"]["quantity"] == 1 and rows["pin"]["quantity"] == 2,
               f"quantities: {rows}")
        expect(rows["block"]["material"] == "steel", f"material: {rows['block']}")
        expect(abs(rows["block"]["unit_mass_g"] - steel_g) < 1e-9,
               "BOM row mass == mass_properties total (shared math)")
        expect(rows["pin"]["unit_mass_g"] is None,
               f"material-less pin has null mass: {rows['pin']}")
        expect(abs(rows["pin"]["unit_volume"] - 8.0) < 1e-6,
               f"pin volume still reported: {rows['pin']}")
        expect(bom["total"]["mass_g"] is None,
               f"unknown contributor taints the total: {bom['total']}")

        # --- assign the pin a material: total becomes known -----------------
        parts = {p["name"]: p for p in d.call("list_parts")["parts"]}
        d.call("set_active_part", index=parts["pin"]["index"])
        pin_feat = [f for f in d.call("get_state")["features"]
                    if f["type"] == "Extrude"][0]
        d.call("set_material", feature_id=pin_feat["id"], material="aluminum")
        d.call("set_active_part", index=parts["rig"]["index"])
        bom = d.call("bom")
        rows = {r["part"]: r for r in bom["rows"]}
        expect(rows["pin"]["unit_mass_g"] > 0, f"pin mass known: {rows['pin']}")
        want = steel_g + 2 * rows["pin"]["unit_mass_g"]
        expect(abs(bom["total"]["mass_g"] - want) < 1e-9,
               f"total = block + 2*pin: {bom['total']} vs {want}")

        # --- CSV export ------------------------------------------------------
        csv_path = os.path.join(tmp, "rig-bom.csv")
        out = d.call("export_bom", path=csv_path)
        expect(out["rows"] == 2, f"export row count: {out}")
        lines = open(csv_path).read().strip().splitlines()
        expect(lines[0] ==
               "part,configuration,quantity,standard,designation,material,"
               "unit_volume_mm3,"
               "unit_mass_g,total_mass_g,printed_unit_mass_g,"
               "printed_total_mass_g,plates",
               f"csv header: {lines[0]}")
        expect(len(lines) == 4, f"2 rows + header + total: {lines}")
        expect(lines[-1].startswith("total,"), f"total row: {lines[-1]}")
        # Every field is present on the total row too, so a spreadsheet reads
        # the last cell as the total rather than shifting it under a column
        # that was added since.
        expect(lines[-1].count(",") == lines[0].count(","),
               f"total row field count: {lines[-1]}")
        pin_line = [l for l in lines if l.startswith("pin,")][0]
        # A part somebody modelled leaves standard and designation EMPTY —
        # which is what a purchaser filters on to split bought from made.
        expect(",2,,,aluminum," in pin_line, f"pin CSV row: {pin_line}")

        # --- a PRINTED part: weighed as it prints, and its plate -----------
        # (docs/assemblies/README.md#bill-of-materials). A 20 x 20 x 10 PLA
        # clip at 15 % infill weighs its printed volume, not its solid, and
        # a plate of Derived copies of it says where it prints.
        d.call("save_print_profile", profile={
            "name": "Smoke bom", "process": "fdm", "material": "pla",
            "layer_height_mm": 0.2, "line_width_mm": 0.5})
        d.call("add_part", name="clip")
        d.call("set_print_process", profile="Smoke bom")
        box(d, (0, 0, 0), (20, 20, 10), "Clip")
        d.call("set_print_settings", walls=2, top_layers=3, bottom_layers=3,
               infill_pct=15, infill_pattern="gyroid")
        d.call("set_material", feature_id=d.call("get_state")["features"]
               [-1]["id"], material="pla")
        mp = d.call("mass_properties")
        printed_g = mp["bodies"][0]["printed"]["mass_g"]
        solid_g = mp["total"]["mass_g"]
        expect(printed_g < 0.6 * solid_g, f"infill weighs less: {mp}")
        clip_id = next(p["id"] for p in d.call("list_parts")["parts"]
                       if p["name"] == "clip")
        d.call("add_part", name="plate A")
        d.call("set_print_process", profile="Smoke bom")
        for k in range(3):
            d.call("add_feature", feature={
                "type": "Derived", "name": f"clip {k}",
                "source_part": clip_id, "reference": False,
                "placement": {"position": [30.0 * k, 0, 0],
                              "orientation": [0, 0, 0, 1]}})
        d.call("set_active_part", part="rig")
        d.call("add_occurrence", part="clip",
               transform={"position": [0, 40, 0], "orientation": [0, 0, 0, 1]})
        bom = d.call("bom")
        rows = {r["part"]: r for r in bom["rows"]}
        expect(rows["pin"]["printed"] is None and rows["pin"]["plates"] == [],
               f"a part not printed: {rows['pin']}")
        clip = rows["clip"]
        expect(abs(clip["printed"]["unit_mass_g"] - printed_g) < 1e-9 and
               abs(clip["unit_mass_g"] - solid_g) < 1e-9,
               f"printed as mass_properties weighs it, solid beside: {clip}")
        expect(clip["plates"] == [{"plate": "plate A", "pieces": 3}],
               f"its plate: {clip['plates']}")
        made = bom["total"]["mass_g"] - solid_g + printed_g
        expect(abs(bom["total"]["made_mass_g"] - made) < 1e-9,
               f"the whole as made: {bom['total']} vs {made}")
        d.call("export_bom", path=csv_path)
        lines = open(csv_path).read().strip().splitlines()
        clip_line = [l for l in lines if l.startswith("clip,")][0]
        expect(clip_line.endswith(",plate A x3"), f"clip CSV row: {clip_line}")
        expect(lines[-1].count(",") == lines[0].count(","),
               f"total row field count: {lines[-1]}")

        print("bom_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
