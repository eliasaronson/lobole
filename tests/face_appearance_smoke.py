#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: an appearance override on ONE FACE (v164,
docs/rendering/materials.md#a-face-of-its-own) — set_appearance {element},
through the real RPC path: the face resolves by name, survives an upstream
edit, a face a later cut made can be painted on the body's creating feature,
a name that stops resolving is REPORTED in the response that lost it and in
get_state (never dropped), clearing works on a stale name, the override
persists, undoes, and rebuilds no geometry.

Usage: face_appearance_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402

RED = [0.8, 0.02, 0.02]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def close(a, b):
    return all(abs(x - y) < 1e-6 for x, y in zip(a, b))


def face_where(d, fid, normal_z, z):
    """The element name of `fid`'s body face whose normal is +-Z and whose
    centroid sits at height z."""
    for body in d.call("list_elements")["bodies"]:
        if body["created_by"] != fid:
            continue
        for f in body["faces"]:
            n = f.get("normal") or [0, 0, 0]
            c = f.get("centroid") or [0, 0, 0]
            if (f.get("kind") == "plane" and abs(n[2] - normal_z) < 1e-6
                    and abs(c[2] - z) < 1e-6):
                return f["name"]
    raise AssertionError(f"no face of {fid} with normal z {normal_z} at {z}")


def an_edge(d, fid):
    for body in d.call("list_elements")["bodies"]:
        if body["created_by"] == fid:
            for e in body.get("edges", []):
                return e["name"]
    return None


def painted(d, fid):
    """element -> the get_state face_appearance entry, on fid's body."""
    for b in d.call("get_state")["bodies"]:
        if b["created_by"] == fid:
            return {f["element"]: f for f in b.get("face_appearance", [])}
    raise AssertionError(f"no body of {fid}")


def refused(fn, needle):
    try:
        fn()
    except RuntimeError as e:
        expect(needle in str(e), f"refusal names '{needle}': {e}")
        return
    raise AssertionError(f"expected a refusal naming '{needle}'")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_face_appearance_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        fid = box(d, (0, 0, 0), (20, 20, 10), "Wall")["id"]
        top = face_where(d, fid, 1.0, 10.0)

        # Paint the top face; the response names the face it resolved to.
        r = d.call("set_appearance", feature_id=fid, element=top, color=RED)
        expect(not r["cleared"] and r["element"] == top and "face" in r,
               f"face paint reported: {r}")
        expect(r["regen"]["ok"] and not r["regen"]["warnings"],
               f"a clean paint warns nothing: {r['regen']}")
        faces = painted(d, fid)
        expect(top in faces and close(faces[top]["color"], RED)
               and "roughness" not in faces[top],
               f"get_state shows the painted face, set fields only: {faces}")
        body = d.call("get_state")["bodies"][0]
        expect("appearance" not in body,
               f"a face paint is not a body paint: {body}")
        # Rebuilt nothing: the face override is out of the regen hash.
        expect(d.call("get_state")["regen"].get("executed", -1) == 0,
               "a face recolour must not rebuild geometry")

        # An upstream edit renumbers nothing it names: still painted.
        feats = d.call("get_state")["features"]
        wall = next(f for f in feats if f.get("id") == fid)
        sketch_id = wall["sketch_id"]
        sk = next(f for f in feats if f.get("id") == sketch_id)
        r = d.call("modify_feature", feature_id=fid,
                   feature=dict(wall, distance=14.0))
        expect(r["regen"]["ok"] and not r["regen"]["warnings"],
               f"edit keeps the face: {r['regen']}")
        expect(top in painted(d, fid), "still painted after the edit")
        expect(sk is not None, "the wall has a sketch")

        # A pocket cut into the top: its floor is a face of the WALL's body,
        # made by the cut, and is painted on the wall's creating feature.
        cut = box(d, (5, 5, 9), (10, 10, 5), "Pocket", mode="cut")["id"]
        floor = face_where(d, fid, 1.0, 9.0)
        r = d.call("set_appearance", feature_id=fid, element=floor,
                   color=[0.02, 0.6, 0.05], roughness=0.9)
        expect(r["regen"]["ok"] and "face" in r, f"cut face painted: {r}")
        faces = painted(d, fid)
        expect(floor in faces and top in faces,
               f"both faces painted: {list(faces)}")

        # Suppress the cut: the floor is gone. The SAME response says so.
        r = d.call("suppress_feature", feature_id=cut, suppressed=True)
        warn = [w for w in r["regen"]["warnings"]
                if w.get("face_appearance") == floor]
        expect(warn and "names no face" in warn[0]["warning"],
               f"the edit that lost the face reports it: {r['regen']}")
        wall = next(f for f in d.call("get_state")["features"]
                    if f.get("id") == fid)
        expect(wall.get("face_appearance_unresolved") == [floor],
               f"get_state names the stale face: {wall}")
        expect(any(f["element"] == floor
                   for f in wall.get("face_appearance", [])),
               f"kept, not dropped: {wall.get('face_appearance')}")
        expect(floor not in painted(d, fid), "but painted on no face")
        # Back: painted again, and the warning is gone.
        r = d.call("suppress_feature", feature_id=cut, suppressed=False)
        expect(not r["regen"]["warnings"], f"resolves again: {r['regen']}")
        expect(floor in painted(d, fid), "the floor is painted again")

        # Clearing a STALE name works: it is how a stale override goes.
        d.call("suppress_feature", feature_id=cut, suppressed=True)
        r = d.call("set_appearance", feature_id=fid, element=floor)
        expect(r["cleared"] and not r["regen"]["warnings"],
               f"stale override cleared: {r}")
        d.call("suppress_feature", feature_id=cut, suppressed=False)
        expect(floor not in painted(d, fid), "cleared stays cleared")
        # Undo brings it back, painted.
        d.call("undo")  # the unsuppress
        d.call("undo")  # the clear
        d.call("suppress_feature", feature_id=cut, suppressed=False)
        expect(floor in painted(d, fid), f"undo restores the face paint")

        # Refusals, each naming why, and changing nothing.
        refused(lambda: d.call("set_appearance", feature_id=fid,
                               element="no/such", color=RED),
                "names no face")
        edge = an_edge(d, fid)
        if edge is not None:
            refused(lambda: d.call("set_appearance", feature_id=fid,
                                   element=edge, color=RED), "is an edge")
        refused(lambda: d.call("set_appearance", feature_id=fid,
                               element="no/such"), "no face override to clear")
        refused(lambda: d.call("set_appearance", feature_id=sketch_id,
                               element=top, color=RED), "created no body")

        # Persists.
        path = os.path.join(tmp, "faces.json")
        d.call("save_project", path=path)
        d.call("open_project", path=path)
        faces = painted(d, fid)
        expect(top in faces and floor in faces
               and abs(faces[floor]["roughness"] - 0.9) < 1e-6,
               f"face paints survive reopen: {faces}")

        print("face_appearance_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
