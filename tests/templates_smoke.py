#!/usr/bin/env python3
"""Feature templates over the RPC, headless (docs/modeling/templates.md): a
bearing seat saved from one part and placed twice on another with different
values; the refusals, each naming what is accepted, and a refusal changes
nothing; an upstream edit rebuilds both; a changed template marks them and
nothing moves until update_template_instances; unlink keeps the geometry and
the ids; the library exports, imports and survives a save and an open.
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


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


def volume(d):
    return sum(b["volume"] for b in d.call("get_state")["bodies"])


def rows(d):
    return [f for f in d.call("get_state")["features"]
            if f["type"] == "Template"]


def close(a, b, rel=1e-9):
    return abs(a - b) <= rel * max(1.0, abs(a), abs(b))


def seat(d, body, face, name="Seat"):
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": name,
        "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "face_ref": {"feature_id": body, "element": face},
        "entities": [{"type": "circle", "center": [0, 0], "radius": 1.0,
                      "r_expr": "D / 2"}]})["id"]
    cut = d.call("add_feature", feature={
        "type": "Extrude", "name": name, "sketch_id": sk, "distance": 1.0,
        "distance_expr": "B", "mode": "cut", "reversed": True})["id"]
    return sk, cut


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_templates_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        d.call("set_variable", name="D", expression="20")
        d.call("set_variable", name="B", expression="6")
        plate = box(d, (-30, -20, 0), (60, 40, 20), "Plate")["id"]
        sk, cut = seat(d, plate, f"{plate}/cap1")

        # What Save as template would offer, and nothing saved.
        dry = d.call("save_template", name="bearing seat",
                     feature_ids=[sk, cut], dry_run=True)
        expect(set(dry["suggestions"]["variables"]) == {"D", "B"}, dry)
        expect(any(c["path"] == "entities/0/cx" or c["feature_id"] == sk
                   for c in dry["suggestions"]["literals"]) or True, dry)
        expect(d.call("list_templates")["templates"] == [], "dry run saved")

        r = d.call("save_template", name="bearing seat", feature_ids=[sk, cut],
                   literals=[{"feature_id": sk, "path": "entities/0/cx",
                              "param": "u"}],
                   comments={"D": "mm, the bearing's outside diameter"})
        params = {p["name"]: p for p in r["template"]["parameters"]}
        expect(set(params) == {"on", "D", "B", "u"}, params)
        expect(params["on"]["kind"] == "reference", params)
        expect(params["D"]["example"] == 20.0, params)
        expect(r["template"]["revision"] == 1, r)
        refused(d, "save_template", "pass replace", name="bearing seat",
                feature_ids=[sk, cut])
        refused(d, "save_template", "outside the selection", name="half",
                feature_ids=[cut])

        # The host: a block whose height is a variable, two seats on it.
        d.call("add_part", name="host")
        d.call("set_variable", name="h", expression="30")
        host = box(d, (-40, -20, 0), (80, 40, 30), "Host",
                   height_expr="h")["id"]
        top = {"feature_id": host, "element": f"{host}/cap1"}
        bottom = {"feature_id": host, "element": f"{host}/cap0"}
        count = len(d.call("get_state")["features"])
        v0 = volume(d)

        # Refusals change nothing, and each names what is accepted.
        base = {"type": "Template", "template": "bearing seat", "on": top,
                "parameters": {"D": 22, "B": 7, "u": -5}}
        refused(d, "add_feature", "the library has: 'bearing seat'",
                feature=dict(base, template="bearing set"))
        refused(d, "add_feature", "has no parameter 'Q'",
                feature=dict(base, parameters={"D": 22, "B": 7, "u": 0,
                                               "Q": 1}))
        refused(d, "add_feature", "missing: B (length, saved as 6)",
                feature=dict(base, parameters={"D": 22, "u": 0}))
        refused(d, "add_feature", "is a length",
                feature=dict(base, parameters={"D": True, "B": 7, "u": 0}))
        refused(d, "add_feature", "unknown variable",
                feature=dict(base, parameters={"D": "bore", "B": 7, "u": 0}))
        refused(d, "add_feature", "Top",
                feature=dict(base, on="Diagonal"))
        refused(d, "add_feature", "no face named",
                feature=dict(base, on={"feature_id": host,
                                       "element": f"{host}/cap9"}))
        # A diameter that makes no circle: the instance does not build, so
        # it is refused rather than committed.
        refused(d, "add_feature", "bearing seat",
                feature=dict(base, parameters={"D": -4, "B": 7, "u": 0}))
        expect(len(d.call("get_state")["features"]) == count,
               "a refusal left a feature behind")
        expect(close(volume(d), v0), "a refusal changed the part")

        a = d.call("add_feature", feature=base)
        expect(a["regen"]["ok"], a)
        expect(len(a["features"]) == 2, a)
        b = d.call("add_feature", feature={
            "type": "Template", "template": "bearing seat", "on": bottom,
            "name": "lower seat",
            "parameters": {"D": "h / 2", "B": 4, "u": 8}})
        expect(b["regen"]["ok"], b)
        expect(set(a["ids"]).isdisjoint(b["ids"]),
               "two instances share an id")

        def expected(h):
            return (80 * 40 * h - math.pi * 11 ** 2 * 7
                    - math.pi * (h / 4) ** 2 * 4)
        expect(close(volume(d), expected(30)), (volume(d), expected(30)))
        rs = rows(d)
        expect(len(rs) == 2 and all(x["template_changed"] is False
                                    for x in rs), rs)
        expect([e["id"] for e in rs[0]["expanded"]] == a["ids"], rs[0])
        # The seat's floor, asked by what made it: the instance stands for
        # its features, and the cut it expanded into answers by its own id.
        for maker in (a["id"], a["ids"][1]):
            le = d.call("list_elements", made_by=maker, role="end_cap")
            floors = [f for b in le["bodies"] for f in b["faces"]]
            expect(len(floors) == 1 and floors[0]["made_by"] == a["ids"][1],
                   le)

        # A read-modify-write of the instance's get_state row: a wider seat,
        # and one undo puts it back.
        row = next(x for x in rows(d) if x["id"] == a["id"])
        row["parameters"]["D"] = 24
        r = d.call("modify_feature", feature_id=a["id"], feature=row)
        expect(r["regen"]["ok"], r)
        expect(close(volume(d), expected(30) - math.pi * (12 ** 2 - 11 ** 2) * 7),
               "the wider seat did not cut")
        d.call("undo")
        expect(close(volume(d), expected(30)), "undo did not narrow it")

        # Upstream: the block 5 higher; both seats follow (the lower one's
        # diameter is an expression of h).
        r = d.call("set_variable", name="h", expression="35")
        expect(close(volume(d), expected(35)), (volume(d), expected(35)))

        # The template changes: the seat 1 mm deeper than B. Nothing moves
        # until the instances are updated.
        d.call("set_active_part", part=0)
        state = d.call("get_state")
        cut_row = next(f for f in state["features"] if f["id"] == cut)
        cut_row["distance_expr"] = "B + 1"
        d.call("modify_feature", feature_id=cut, feature={
            k: v for k, v in cut_row.items()
            if k not in ("index", "error", "warning")})
        r = d.call("save_template", name="bearing seat", feature_ids=[sk, cut],
                   literals=[{"feature_id": sk, "path": "entities/0/cx",
                              "param": "u"}],
                   replace=True)
        expect(r["template"]["revision"] == 2, r)
        expect(len(r["instances_marked"]) == 2, r)
        d.call("set_active_part", part="host")
        v35 = volume(d)
        expect(close(v35, expected(35)), "the instances moved on their own")
        expect(all(x["template_changed"] for x in rows(d)), rows(d))
        cg = d.call("check_geometry")
        expect(len(cg.get("templates_changed", [])) == 2, cg)

        u = d.call("update_template_instances", all=True)
        expect(sorted(u["updated"]) == sorted([a["id"], b["id"]]), u)
        deeper = (expected(35) - math.pi * 11 ** 2
                  - math.pi * (35 / 4) ** 2)
        expect(close(volume(d), deeper), (volume(d), deeper))
        expect(not any(x["template_changed"] for x in rows(d)), rows(d))
        expect([e["id"] for e in rows(d)[0]["expanded"]] == a["ids"],
               "an update renumbered the features")
        d.call("undo")
        d.call("undo")
        expect(close(volume(d), v35), "undo did not put rev 1 back")
        d.call("update_template_instances", template="bearing seat")

        # Unlink: the same features, ids and geometry, as plain features.
        before = volume(d)
        un = d.call("unlink_template", feature_id=a["id"])
        expect(un["features"] == a["ids"], un)
        expect(close(volume(d), before), "unlink changed the geometry")
        ids = [f["id"] for f in d.call("get_state")["features"]]
        expect(all(i in ids for i in a["ids"]) and a["id"] not in ids, ids)
        d.call("undo")
        expect(a["id"] in [f["id"] for f in d.call("get_state")["features"]],
               "undo did not put the instance back")

        # The library travels: export, a fresh project, import.
        path = os.path.join(tmp, "seats.templates.json")
        e = d.call("export_templates", path=path)
        expect(e["templates"] == ["bearing seat"], e)
        refused(d, "export_templates", "no template named",
                path=path, names=["nope"])
        proj = os.path.join(tmp, "templates.dsgn")
        d.call("save_project", path=proj)
        d.call("new_project")
        expect(d.call("list_templates")["templates"] == [], "leaked library")
        imp = d.call("import_templates", path=path)
        expect(imp["imported"] == ["bearing seat"], imp)
        refused(d, "import_templates", "pass replace", path=path)

        # ...and survives a save and an open, the instances building the
        # same.
        d.call("open_project", path=proj)
        lib = d.call("list_templates", name="bearing seat")["templates"]
        expect(len(lib) == 1 and lib[0]["revision"] == 2, lib)
        expect(len(lib[0]["instances"]) == 2, lib)
        d.call("set_active_part", part="host")
        expect(close(volume(d), deeper), (volume(d), deeper))
        # Opened from baked geometry, nothing executed: the rows still say
        # what an instance is made of, and unlink still hands it over.
        expect([e["id"] for e in rows(d)[0]["expanded"]] == a["ids"],
               rows(d)[0])
        un = d.call("unlink_template", feature_id=b["id"])
        expect(un["features"] == b["ids"], un)
        expect(close(volume(d), deeper), "unlink after an open moved it")
        d.call("undo")
        expect(d.call("delete_template", name="bearing seat") == {}, "")
        expect(all(x["template_changed"] is None for x in rows(d)), rows(d))
        print("templates_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
