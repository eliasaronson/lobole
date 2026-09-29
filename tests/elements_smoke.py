#!/usr/bin/env python3
"""list_elements by what made a face, headless (docs/modeling/toponaming.md):
a pocket's floor and walls, a hole's bores, a pattern's copies and a Derived
copy's faces through `via`, each asked for by feature and role; the refusal
of a role that does not exist, and the why of a filter that matches nothing.
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


def faces(r):
    return [f for b in r["bodies"] for f in b["faces"]]


def plane(z):
    return {"origin": [0, 0, z], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_elements_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        d.call("add_part", name="plate")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Outline", "plane": plane(0),
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [60, 40]}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Plate", "sketch_id": sk["id"],
            "distance": 20.0, "mode": "new_body"})
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Pocket sketch", "plane": plane(20),
            "entities": [{"type": "rect", "c1": [5, 5], "c2": [15, 15]}]})
        pocket = d.call("add_feature", feature={
            "type": "Extrude", "name": "Pocket", "sketch_id": sk["id"],
            "distance": 8.0, "reversed": True, "mode": "cut"})["id"]
        d.call("add_feature", feature={
            "type": "Pattern", "name": "Pockets", "source_id": pocket,
            "kind": "linear", "axis": {"kind": "x"}, "count": 3,
            "spacing": 15.0})
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Hole points", "plane": plane(20),
            "entities": [{"type": "point", "p": [50, 30]},
                         {"type": "point", "p": [50, 10]}]})
        d.call("add_feature", feature={
            "type": "Hole", "name": "Holes", "sketch_id": sk["id"],
            "diameter": 5.0, "depth": 10.0})

        floor = d.call("list_elements", made_by="Pocket", role="floor")
        expect(floor["matched_faces"] == 3 and
               all(f["role"] == "end_cap" for f in faces(floor)),
               f"the pocket and its two copies each have a floor: {floor}")
        expect(sorted(f.get("instance", "") for f in faces(floor)) ==
               ["", "1", "2"], faces(floor))
        # The pattern answers for its copies: they carry its source's names.
        pat = d.call("list_elements", made_by="Pockets", role="floor")
        expect(pat["matched_faces"] == 3, pat)
        walls = d.call("list_elements", made_by="Pocket", role="walls")
        expect(walls["matched_faces"] == 12 and
               {f["entity"] for f in faces(walls)} == {2, 3, 4, 5}, walls)
        bores = d.call("list_elements", made_by="Holes", role="bore")
        expect(bores["matched_faces"] == 2 and
               all(abs(f["radius"] - 2.5) < 1e-6 for f in faces(bores)),
               bores)

        # Nothing matched: said why, not an empty list to puzzle over.
        none = d.call("list_elements", made_by="Holes", role="countersink")
        expect(none["matched_faces"] == 0 and "made bore" in none["why"],
               none)
        refused(d, "list_elements", "is a Sketch", made_by="Outline")
        refused(d, "list_elements", "is not one of", made_by="Pocket",
                role="lid")
        refused(d, "list_elements", "no feature named", made_by="Nope")

        # A Derived copy: the source part's features, through `via`.
        d.call("add_part", name="holder")
        plate = next(p["id"] for p in d.call("list_parts")["parts"]
                     if p["name"] == "plate")
        d.call("add_feature", feature={
            "type": "Derived", "name": "the plate", "source_part": plate,
            "reference": False})
        copy = d.call("list_elements", part="holder", via="the plate",
                      made_by="Pocket", role="floor")
        expect(copy["matched_faces"] == 3 and
               all(f["derived"] == [1] for f in faces(copy)), copy)
        # Without `via` a copy's faces are not this part's features'.
        own = d.call("list_elements", part="holder", made_by="the plate")
        expect(own["matched_faces"] == 0 and "via" in own["why"], own)
        refused(d, "list_elements", "not a Derived", part="plate",
                via="Pocket")
        print("elements_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()


if __name__ == "__main__":
    main()
