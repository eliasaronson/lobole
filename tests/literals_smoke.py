#!/usr/bin/env python3
"""Integration test: check_literals and extract_variables over RPC, headless.

docs/variables/README.md#literals-that-should-be-expressions. A part built
from literals: the lint names the literal that equals a variable and the
boss sketched on a datum plane where the block's top face lies; the dry run
changes nothing; the apply leaves the geometry where it was, puts an
expression on every field it proposed, is ONE undo step, and each new
variable then moves exactly the fields it drives. In both scopes, plus the
refusals.

Usage: literals_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e),
               f"{method} refused, but not naming '{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def same(a, b):
    """Equal JSON, numbers to 1e-9: a re-solved sketch may put a point at
    1e-35 where it was 0."""
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float) or isinstance(b, float):
        return (isinstance(a, (int, float)) and isinstance(b, (int, float))
                and abs(a - b) <= 1e-9)
    return a == b


def features(d):
    return {f["id"]: f for f in d.call("get_state")["features"]}


def volume(d):
    return sum(b["volume"] for b in d.call("mass_properties")["bodies"])


def variables(d):
    return {v["name"]: v for v in d.call("list_variables")["variables"]}


def field_value(feature, path):
    """The number at `path` in a feature's JSON ('distance',
    'constraints/3' -> its value, 'entities/1/r')."""
    node = feature
    parts = path.split("/")
    for p in parts:
        node = node[int(p)] if p.isdigit() else node[p]
    if isinstance(node, dict):  # a sketch dimension
        return node["value"]
    return node


def has_expr(feature, path):
    parts = path.split("/")
    node = feature
    for p in parts[:-1]:
        node = node[int(p)] if p.isdigit() else node[p]
    last = parts[-1]
    if last.isdigit():  # a constraint, or an array element (origin/2)
        item = node[int(last)]
        if isinstance(item, dict):
            return bool(item.get("expr"))
        key = parts[-2] + "_expr"
        parent = feature
        for p in parts[:-2]:
            parent = parent[int(p)] if p.isdigit() else parent[p]
        return bool(parent.get(key, ["", "", ""])[int(last)])
    return bool(node.get(last + "_expr"))


def build(d):
    """A 40 x 30 block 10 tall on Top, a datum plane 10 up, and a boss
    sketched on the datum: all literals, beside a variable plate_t = 10."""
    d.call("set_variable", name="plate_t", expression="10")
    block = d.call("add_feature", feature={
        "type": "Box", "name": "Block", "on": "Top", "width": 40.0,
        "depth": 30.0, "height": 10.0})
    deck = d.call("add_feature", feature={
        "type": "DatumPlane", "name": "Deck", "kind": "offset",
        "base_plane": "xy", "offset": 10.0})
    boss = d.call("add_feature", feature={
        "type": "Cylinder", "name": "Boss", "on": {"feature_id": deck["id"]},
        "diameter": 8.0, "height": 6.0, "mode": "add"})
    return block, deck, boss


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_literals_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        block, deck, boss = build(d)
        block_id = block["id"]

        # The lint: the block's height equals plate_t, and the boss sits on
        # the datum where the block's top face lies.
        lint = d.call("check_literals")
        hits = [(f["feature_id"], f["field"], f["variables"])
                for f in lint["literals"]]
        expect((block_id, "distance", ["plate_t"]) in hits,
               f"the block's height is plate_t: {lint['literals']}")
        places = lint["placements"]
        expect(len(places) == 1, f"one world placement over a face: {places}")
        expect(places[0]["on"] == "datum plane 'Deck'", places[0])
        expect(places[0]["face"] == {"feature_id": block_id,
                                     "element": f"{block_id}/cap1"},
               f"the block's top face: {places[0]}")
        expect(lint["checked"]["fields"] > 0, lint["checked"])

        # Dry run: nothing moves.
        before = features(d)
        vars_before = variables(d)
        vol = volume(d)
        plan = d.call("extract_variables")
        expect(plan["applied"] is False, plan)
        expect(same(features(d), before), "a dry run changed a feature")
        expect(variables(d) == vars_before, "a dry run changed the variables")
        names = [v["name"] for v in plan["variables"]]
        expect("block_distance" in names, f"named <feature>_<field>: {names}")
        reasons = {l["reason"] for l in plan["left_alone"]}
        expect(any("zero" in r for r in reasons) or
               any("constraint" in r for r in reasons),
               f"left-alone reasons: {reasons}")

        # Apply: one undo step, the geometry where it was, every field driven.
        applied = d.call("extract_variables", apply=True)
        expect(applied["applied"] is True, applied)
        expect(abs(volume(d) - vol) < 1e-6, f"geometry moved: {volume(d)}")
        expect(not applied["regen"].get("errors"), applied["regen"])
        after = features(d)
        for v in applied["variables"]:
            for u in v["uses"]:
                expect(has_expr(after[u["feature_id"]], u["field"]),
                       f"{u} carries no expression")
        now = variables(d)
        for v in applied["variables"]:
            expect(v["name"] in now and now[v["name"]]["scope"] == "project",
                   f"{v['name']} is a project variable")

        # Each variable moves exactly the fields it drives.
        for v in applied["variables"]:
            d.call("set_variable", name=v["name"],
                   expression=f"{v['value']} * 1.25")
            moved = features(d)
            for fid, f in moved.items():
                for u in [u for u in v["uses"] if u["feature_id"] == fid]:
                    expect(abs(field_value(f, u["field"]) -
                               1.25 * field_value(after[fid], u["field"]))
                           < 1e-6, f"{v['name']} did not drive {u}")
            driven = {(u["feature_id"], u["field"]) for u in v["uses"]}
            for x in applied["variables"]:
                for u in x["uses"]:
                    if (u["feature_id"], u["field"]) in driven:
                        continue
                    expect(abs(field_value(moved[u["feature_id"]],
                                           u["field"]) -
                               field_value(after[u["feature_id"]],
                                           u["field"])) < 1e-9,
                           f"{v['name']} moved {u}, which it does not drive")
            d.call("undo")  # the set_variable
        expect(abs(volume(d) - vol) < 1e-6, "the nudges did not undo")

        # One undo restores the literals and removes the variables.
        d.call("undo")
        expect(same(features(d), before), "one undo did not restore the literals")
        expect(variables(d) == vars_before, "one undo left variables behind")
        d.call("redo")
        expect(same(features(d), after), "redo did not put the extraction back")
        d.call("undo")

        # reuse binds the lint's finding to plate_t instead of a new name.
        bound = d.call("extract_variables", feature_ids=[block_id],
                       fields=["distance"], reuse=True, apply=True)
        expect(bound["variables"][0]["name"] == "plate_t" and
               bound["variables"][0]["existing"], bound["variables"])
        expect(features(d)[block_id].get("distance_expr") == "plate_t",
               features(d)[block_id])
        lint = d.call("check_literals")
        expect(not any(f["feature_id"] == block_id and f["field"] ==
                       "distance" for f in lint["literals"]),
               "a bound literal is still reported")
        d.call("undo")

        # Part scope: a part variable, one step on the part's own stack.
        applied = d.call("extract_variables", scope="part",
                         feature_ids=[block_id], apply=True)
        part_vars = [v for v in variables(d).values() if v["scope"] == "part"]
        expect(len(part_vars) == len(applied["variables"]) > 0,
               f"part-scope variables: {part_vars}")
        expect(abs(volume(d) - vol) < 1e-6, "part-scope apply moved geometry")
        d.call("undo")
        expect(same(features(d), before), "part-scope undo did not restore")
        expect(variables(d) == vars_before, "part-scope undo left variables")

        # Refusals name what is accepted.
        refused(d, "extract_variables", "'project' (the default) or 'part'",
                scope="global")
        refused(d, "extract_variables", "distance", feature_ids=[block_id],
                fields=["radius"])
        refused(d, "extract_variables", "no feature with id",
                feature_ids=[987654])
        print("literals_smoke: OK")
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    main()
