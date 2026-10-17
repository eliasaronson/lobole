#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What a refused call must leave as it found it: the world, as the smokes
that judge refusals read it (refused_calls_smoke, hostile_params_smoke).

`World(d, app, work).cheap()` is a snapshot of everything a call could move
-- dirty, the tab list, undo/redo, the background jobs a caller started, the
machine store, the settings, and the files under the app's cwd (`work`) and
its scratch dirs -- and `changed(a, b)` names what differs between two.
`settle()` saves the project clean first, so `dirty` reports any edit after.
"""
import hashlib
import os

from smoke_shapes import box, cylinder


def tree(root, skip=(), autosaves=True):
    """{relative path: content hash} under root, minus `skip`, and minus
    every `*.autosave.d` when `autosaves` is false."""
    out = {}
    if not os.path.isdir(root):
        return out
    for r, _dirs, files in os.walk(root):
        rel = os.path.relpath(r, root)
        if any(rel == s or rel.startswith(s + os.sep) for s in skip):
            continue
        if not autosaves and any(part.endswith(".autosave.d")
                                 for part in rel.split(os.sep)):
            continue
        for f in files:
            relf = os.path.normpath(os.path.join(rel, f))
            if relf in skip:
                continue
            try:
                with open(os.path.join(r, f), "rb") as fh:
                    out[relf] = hashlib.sha1(fh.read()).hexdigest()
            except OSError:
                out[relf] = "?"
    return out


class World:
    """What a refused call must leave as it found it."""

    def __init__(self, d, app, work):
        self.d, self.app, self.work = d, app, work
        self.fp = os.path.join(work, "fp")
        self.n = 0
        self.cam = d.call("ping").get("features", {}).get("cam", True)

    def settle(self):
        """Save clean, so `dirty` reports any edit after this."""
        self.n += 1
        path = os.path.join(self.fp, f"{self.n}.designer")
        self.d.call("save_project", path=path)
        return tree(path)

    def cheap(self):
        d = self.d
        try:
            st = d.call("get_state")
        except Exception:  # noqa: BLE001 — not a part tab
            st = {}
        return {
            "dirty": d.call("ping").get("dirty"),
            "parts": d.call("list_parts"),
            "undo": st.get("undo"), "redo": st.get("redo"),
            # What a caller started and would have to stop. Not the CAM
            # passes: they are the job's derived toolpaths, recomputed
            # whenever the job is put back, rollback included.
            "jobs": sorted((j["kind"], j["id"]) for j in
                           d.call("background_jobs")["jobs"]
                           if j["kind"] != "cam"),
            "machines": (d.call("list_machines")["machines"]
                         if self.cam else None),
            "settings": d.call("get_settings"),
            # Not an autosave beside a project: it is written on a timer,
            # whatever the call in flight, as the hostile sweep's quick()
            # already knows (docs/versioning/README.md).
            "cwd": tree(self.work, skip=("fp",), autosaves=False),
            "app": tree(self.app.tmp, skip=(
                "app.log",
                # The session's own log (it grows with every call, as
                # app.log does) and its crash-report marker, which names the
                # open project (docs/architecture.md#crashes-and-stalls).
                os.path.join("state", "lobole", "logs"),
                os.path.join("state", "lobole", "sessions"),
                os.path.join("state", "lobole", "instances"),
                os.path.join("state", "lobole", "recent-projects.json"))),
        }


def changed(a, b):
    out = []
    for k in a:
        if a[k] != b[k]:
            if isinstance(a[k], dict) and isinstance(b[k], dict):
                ks = set(a[k]) | set(b[k])
                diff = sorted(x for x in ks if a[k].get(x) != b[k].get(x))
                out.append(f"{k}: {diff[:5]}")
            else:
                out.append(f"{k}: {str(a[k])[:120]} -> {str(b[k])[:120]}")
    return out


def build_fixture(d, cam, rich=False):
    """One tab of each kind: a part with two boxes, an assembly with an
    occurrence of it, a drawing, and a CAM job when the build has CAM (`cam`
    from ping's features). Returns (part, occurrence, setup id or None).

    `rich` also gives the part a representative feature set -- a sketch
    extruded add and cut, a revolve, a cylinder, a fillet, a chamfer, a
    hole, a shell, a linear and a circular pattern, a boolean and a draft --
    for the sweeps that set each feature's fields to hostile values."""
    d.call("new_project")
    box(d, (0, 0, 0), (40, 30, 20), "Plate")
    box(d, (50, 0, 0), (10, 10, 10), "Cube")
    if rich:
        rich_features(d)
    part = d.call("list_parts")["parts"][0]
    d.call("add_assembly", name="Asm")
    d.call("set_active_part", part="Asm")
    occ = d.call("add_occurrence", part=part["id"])
    d.call("set_active_part", part=part["name"])
    d.call("create_drawing", name="Dwg")
    d.call("set_active_part", part=part["name"])
    setup = None
    if cam:  # -DENABLE_CAM=OFF builds have no jobs
        d.call("machine_part", name="Job")
        setup = d.call("add_cam_setup", name="S")["id"]
        d.call("add_cam_op", setup=setup, type="profile", top_z=21.0,
               bottom_z=0.0, stepdown=8.0)
    d.call("set_active_part", part=part["name"])
    return part, occ, setup


def _top(z):
    return {"origin": [0, 0, z], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}


def rich_features(d):
    """The feature set `build_fixture(rich=True)` adds after the two boxes
    (Plate is feature 2, Cube 4). Every one must build: a fixture that
    fails is no baseline."""
    def add(f):
        r = d.call("add_feature", feature=f)
        if not r.get("regen", {}).get("ok", True):
            raise AssertionError(f"fixture feature {f['name']} failed: "
                                 f"{r['regen']}")
        return r

    def elements(fid):
        return next(b for b in d.call("list_elements")["bodies"]
                    if b["created_by"] == fid)

    plate, cube = 2, 4
    sk = add({"type": "Sketch", "name": "Profile", "plane": _top(20),
              "entities": [{"type": "rect", "c1": [5, 5], "c2": [15, 15]}]})
    add({"type": "Extrude", "name": "Boss", "sketch_id": sk["id"],
         "distance": 5.0, "mode": "add"})
    sk = add({"type": "Sketch", "name": "Pocket sketch", "plane": _top(20),
              "entities": [{"type": "circle", "center": [30, 20],
                            "radius": 4}]})
    pocket = add({"type": "Extrude", "name": "Pocket", "sketch_id": sk["id"],
                  "distance": 6.0, "reversed": True, "mode": "cut"})
    sk = add({"type": "Sketch", "name": "Section",
              "plane": {"origin": [0, 0, 0], "normal": [0, 1, 0],
                        "uAxis": [1, 0, 0]},
              "entities": [{"type": "rect", "c1": [80, 0],
                            "c2": [86, -10]}]})
    add({"type": "Revolve", "name": "Turn", "sketch_id": sk["id"],
         "axis": {"kind": "custom", "origin": [75, 0, 0],
                  "direction": [0, 0, 1]},
         "angle_deg": 360.0, "mode": "new_body"})
    peg = cylinder(d, (-20, 0, 0), "z", 5, 12, name="Peg")
    edge = next(e["name"] for e in elements(plate)["edges"]
                if e["kind"] == "line")
    add({"type": "Fillet", "name": "Round", "radius": 1.0,
         "edges": [{"feature_id": plate, "element": edge}]})
    edge = next(e["name"] for e in elements(cube)["edges"]
                if e["kind"] == "line")
    add({"type": "Chamfer", "name": "Bevel", "radius": 1.0,
         "edges": [{"feature_id": cube, "element": edge}]})
    sk = add({"type": "Sketch", "name": "Hole points", "plane": _top(20),
              "entities": [{"type": "point", "p": [30, 8]}]})
    hole = add({"type": "Hole", "name": "Drill", "hole_kind": "simple",
                "sketch_id": sk["id"], "diameter": 3.0, "depth": 8.0})
    cap = [f["name"] for f in elements(peg["id"])["faces"]
           if f["kind"] == "plane"][-1]
    add({"type": "Shell", "name": "Cup", "body_id": peg["id"],
         "thickness": 1.0,
         "open_faces": [{"feature_id": peg["id"], "element": cap}]})
    add({"type": "Pattern", "name": "Row", "kind": "linear",
         "source_id": pocket["id"], "axis": {"kind": "x"}, "count": 2,
         "spacing": -12.0})
    add({"type": "Pattern", "name": "Ring", "kind": "circular",
         "source_id": hole["id"],
         "axis": {"kind": "custom", "origin": [20, 15, 0],
                  "direction": [0, 0, 1]},
         "count": 3, "angle_deg": 120.0})  # between neighbours
    tool = box(d, (52, 2, 5), (4, 4, 10), "Tool")
    add({"type": "Boolean", "name": "Join", "target_id": cube,
         "tool_id": tool["id"], "op": "union"})
    taper = box(d, (0, 50, 0), (10, 10, 10), "Taper")
    side = next(f["name"] for f in elements(taper["id"])["faces"]
                if f["kind"] == "plane" and abs(f["normal"][2]) < 0.5)
    add({"type": "Draft", "name": "Draft", "angle_deg": 5.0,
         "neutral_plane": "xy", "body_id": taper["id"],
         "faces": [{"feature_id": taper["id"], "element": side}]})


def outside_baselines(part, setup, out, cam):
    """The calls that reach outside the document (a file, a tab, the
    settings, the machine store), each as (tab, method, VALID params), with
    every file under `out`. refused_calls_smoke makes each key wrong in
    turn; hostile_params_smoke sets each to hostile values."""
    def path(name):
        return os.path.join(out, name)

    every = [
        (part["name"], "save_project",
         {"path": path("p.designer"), "message": "m"}),
        (part["name"], "export_step", {"path": path("a.step")}),
        (part["name"], "export_stl", {"path": path("a.stl")}),
        (part["name"], "export_brep", {"path": path("a.brep")}),
        (part["name"], "export_visual", {"path": path("a.glb")}),
        (part["name"], "compare_geometry",
         {"a": 0, "b": 1, "missing_path": path("miss.brep"),
          "extra_path": path("extra.brep")}),
        (part["name"], "checkpoint_create", {"name": "cp"}),
        (part["name"], "add_part", {"name": "New part"}),
        (part["name"], "add_assembly", {"name": "New asm"}),
        (part["name"], "create_drawing", {"name": "New dwg"}),
        (part["name"], "machine_part",
         {"part": part["name"], "name": "New job"}),
        (part["name"], "delete_part", {"part": "Dwg"}),
        (part["name"], "set_active_part", {"part": "Asm"}),
        (part["name"], "rename_part",
         {"part": "Dwg", "new_name": "Dwg 2"}),
        (part["name"], "create_folder", {"folder": "F"}),
        (part["name"], "set_keybinding",
         {"command": "part.fillet", "chord": "Shift+B"}),
        (part["name"], "save_machine",
         {"machine": {"name": "Refusal test mill"}}),
        ("Asm", "export_bom", {"path": path("bom.csv")}),
        ("Dwg", "export_drawing", {"path": path("d.svg")}),
        ("Job", "cam_post",
         {"setup": setup, "path": path("j.nc")}),
        ("Job", "export_setup_sheet",
         {"setup": setup, "path": path("j.pdf"),
          "program": "j.nc", "picture": False}),
    ]
    cam_only = ("machine_part", "save_machine", "cam_post",
                "export_setup_sheet")
    return [b for b in every if cam or b[1] not in cam_only]
