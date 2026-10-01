#!/usr/bin/env python3
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

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
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
        d = Designer.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        # --- block: 10x10x10 steel cube; pin: 2x2x2, no material ------------
        d.call("new_project")
        d.call("add_part", name="block")
        blk = box(d, (0, 0, 0), (10, 10, 10), "Blk")
        d.call("set_material", feature_id=blk["id"], material="steel")
        mp = d.call("mass_properties")  # refactored path: same numbers as BOM
        expect(abs(mp["total"]["volume"] - 1000.0) < 1e-6,
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
               "unit_mass_g,total_mass_g",
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

        print("bom_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
