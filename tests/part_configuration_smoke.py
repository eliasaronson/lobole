#!/usr/bin/env python3
"""One part placed in two configurations and two materials, headless
(docs/assemblies/README.md#configurations-per-placement): the BOM has a line
for each, mass follows the configuration and the material, editing a
configuration nobody has active reaches the placements that use it, STEP
writes two products, and a job cuts the part in a configuration.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refused, but not naming "
               f"'{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def near(a, b, tol=1e-6):
    return abs(a - b) <= tol * max(1.0, abs(b))


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_partcfg_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        # A 10 mm square `size` tall; `big` makes it 20 tall.
        d.call("add_part", name="cube")
        d.call("set_variable", name="size", expression="10")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Square",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [10, 10]}]})
        ext = d.call("add_feature", feature={
            "type": "Extrude", "name": "Cube", "sketch_id": sk["id"],
            "distance": 10.0, "distance_expr": "size", "mode": "new_body"})
        d.call("set_material", feature_id=ext["id"], material="wood_maple")
        big = d.call("add_configuration", name="big")["id"]
        d.call("set_configuration", id=big, parameter_overrides=[
            {"name": "size", "expression": "20"}])

        d.call("add_assembly", name="rig")
        small_occ = d.call("add_occurrence", part="cube", fixed=True)["id"]
        big_occ = d.call("add_occurrence", part="cube", configuration="big",
                         material="wood_walnut",
                         transform={"position": [50, 0, 0],
                                    "orientation": [0, 0, 0, 1]})["id"]
        occs = {o["id"]: o for o in d.call("list_occurrences")["occurrences"]}
        expect(occs[big_occ]["configuration_id"] == big and
               occs[big_occ]["material"] == "wood_walnut", occs[big_occ])

        # Two lines: what is made, and what it is made of.
        rows = d.call("bom")["rows"]
        expect(len(rows) == 2, f"a line per configuration: {rows}")
        by_cfg = {r["configuration"]: r for r in rows}
        expect(set(by_cfg) == {"", "big"}, by_cfg)
        expect(near(by_cfg[""]["unit_volume"], 1000.0) and
               near(by_cfg["big"]["unit_volume"], 2000.0),
               f"each line's volume is its configuration's: {rows}")
        expect(by_cfg["big"]["material"] == "wood_walnut" and
               by_cfg[""]["material"] == "wood_maple", rows)
        # maple 705, walnut 610 kg/m3
        expect(near(by_cfg[""]["unit_mass_g"], 1000 * 705e-6, 1e-4) and
               near(by_cfg["big"]["unit_mass_g"], 2000 * 610e-6, 1e-4),
               f"each line's mass is its material's: {rows}")
        mass = d.call("mass_properties")
        expect(near(mass["total"]["volume"], 3000.0),
               f"the assembly weighs both as placed: {mass['total']}")

        # Editing `big`, which nobody has ACTIVE, reaches its placement.
        d.call("set_active_part", part="cube")
        d.call("set_configuration", id=big, parameter_overrides=[
            {"name": "size", "expression": "30"}])
        d.call("set_active_part", part="rig")
        by_cfg = {r["configuration"]: r for r in d.call("bom")["rows"]}
        expect(near(by_cfg["big"]["unit_volume"], 3000.0),
               f"a non-active configuration's edit reaches the BOM: {by_cfg}")

        # A part's faces as they are in a configuration nobody has active.
        cap = d.call("list_elements", part="cube", configuration="big",
                     made_by="Cube", role="end_cap")["bodies"][0]["faces"]
        expect(len(cap) == 1 and near(cap[0]["centroid"][2], 30.0),
               f"the big cube's top, listed in its configuration: {cap}")
        refused(d, "list_elements", "Known: big", part="cube",
                configuration="huge")

        # Refusals, each naming what it would take.
        refused(d, "set_occurrence_configuration", "Known: big",
                id=small_occ, configuration="huge")
        refused(d, "set_occurrence_material", "unknown material",
                id=small_occ, material="wood_teak")
        d.call("set_occurrence_material", id=small_occ, material="")

        # STEP: two products, one per configuration and material.
        out = d.call("export_step", path=os.path.join(tmp, "rig.step"))
        expect(out["products"] == 2 and out["instances"] == 2,
               f"two products, two placements: {out}")

        # A job cuts the part in a configuration.
        job = d.call("machine_part", part="cube", configuration="big")
        expect(job["part"]["configuration"] == big, job)
        parts = d.call("list_job_parts")["parts"]
        expect(parts[0]["configuration"] == big and
               parts[0]["configuration_name"] == "big" and
               not parts[0]["missing"], parts)
        faces = [f for b in d.call("list_elements")["bodies"]
                 for f in b["faces"]]
        top = max(f["centroid"][2] for f in faces)
        expect(near(top, 30.0),
               f"the job's part is the configuration it cuts: top at {top}")
        refused(d, "add_job_part", "Known: big", part="cube",
                configuration="huge")
        print("part_configuration_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()


if __name__ == "__main__":
    main()
