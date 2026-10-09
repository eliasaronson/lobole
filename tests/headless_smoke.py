#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: drive `lobole --headless --remote` — no window, no
GPU, no display server (DISPLAY is stripped to prove it).

Covers the document/geometry RPC surface headless: the S1 bracket with
analytic volume + mass_properties asserts, element names driving a 3D
chamfer, project round-trip, undo/redo, the specific errors of every
GUI-dependent method, clean SIGTERM shutdown, and fail-fast on a taken port.

Usage: headless_smoke.py <path-to-designer-binary>
"""
import glob
import math
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer, RemoteError  # noqa: E402
from smoke_app import launch_app  # noqa: E402
import smoke_shapes as shapes  # noqa: E402


def pose_of(d, occ_id):
    """An occurrence's placed position — enough to tell two poses apart."""
    o = next(x for x in d.call("list_occurrences")["occurrences"]
             if x["id"] == occ_id)
    return tuple(o["transform"]["position"]) + tuple(
        o["transform"]["orientation"])





# Phase logging: this suite runs under `ctest -j`, where a rare failure used
# to say only "headless_smoke ... Failed" with no clue which of its ~40 steps
# gave way or how long it had waited. Every phase prints with its elapsed
# time, so the next flake explains itself instead of costing an investigation.
_T0 = time.time()
_PHASE = "startup"


def phase(name):
    global _PHASE
    print(f"  [{time.time() - _T0:6.1f}s] {name}", flush=True)
    _PHASE = name


def expect(condition, message):
    if not condition:
        raise AssertionError(f"[phase: {_PHASE}] {message}")


def wait_for_file(path, timeout, what):
    """Wait for a file, reporting how long it actually took.

    Fixed short deadlines are the classic source of load flakes: this suite
    shares a machine with a GPU topology solve and fifteen other tests. The
    budget is generous and the elapsed time is printed, so a slow machine
    passes and a genuinely broken one still fails.
    """
    deadline = time.time() + timeout
    started = time.time()
    while time.time() < deadline:
        if os.path.exists(path):
            print(f"      ({what} after {time.time() - started:.1f}s)",
                  flush=True)
            return True
        time.sleep(0.1)
    return False


def wait_exit(proc, timeout, what):
    """proc.wait with the elapsed time reported (see wait_for_file)."""
    started = time.time()
    try:
        code = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise AssertionError(
            f"[phase: {_PHASE}] {what} did not exit within {timeout}s")
    print(f"      ({what} exited {code} after {time.time() - started:.1f}s)",
          flush=True)
    return code


def expect_error(d, method, fragment, **params):
    try:
        d.call(method, **params)
        raise AssertionError(f"{method} must error headless")
    except RuntimeError as e:
        expect(fragment in str(e), f"{method} error mentions '{fragment}': {e}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)  # a display server must NOT be needed
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_headless_")
    env["XDG_STATE_HOME"] = tmp  # keep unsaved-slot autosaves out of $HOME
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    # launch_app keeps the app's own log and prints it on failure — this
    # test grew its own copy of that for the autosave phase, and it is now
    # what every smoke script gets.
    app = launch_app(binary, extra_args=["--autosave", "1"], env=env)
    try:
        d = Designer.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        # What this BUILD contains: optional subsystems are compile-time, so a
        # missing method can mean "not in this build" rather than "broken".
        has_cam = d.call("ping").get("features", {}).get("cam", True)

        phase("Strict params")
        # Strict params: a key no handler reads is a typo, rejected loudly
        # instead of silently ignored (the `named` vs `name` trap). A near
        # miss suggests the intended key; the corrected call still works.
        expect_error(d, "get_state", "unknown parameter", bogus=1)
        expect_error(d, "set_variable", "did you mean 'comment'",
                     name="tw", expression="1", coment="typo")
        d.call("set_variable", name="tw", expression="1", comment="ok")

        phase("The S1 bracket")
        # The S1 bracket: variables -> constrained sketch -> extrude -> fillet
        d.call("new_project")
        d.call("set_variable", name="width", expression="40")
        d.call("set_variable", name="height", expression="width / 2")
        d.call("set_variable", name="thickness", expression="5")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Base",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "line", "a": [1, 1], "b": [30, 2]},
                {"type": "line", "a": [30, 2], "b": [31, 15]},
                {"type": "line", "a": [31, 15], "b": [2, 16]},
                {"type": "line", "a": [2, 16], "b": [1, 1]}]})
        bot, right, top, left = sk["entity_ids"]
        for kind, refs, kw in [
                ("coincident", [[bot, 1], [right, 0]], {}),
                ("coincident", [[right, 1], [top, 0]], {}),
                ("coincident", [[top, 1], [left, 0]], {}),
                ("coincident", [[left, 1], [bot, 0]], {}),
                ("horizontal", [[bot, 0]], {}),
                ("horizontal", [[top, 0]], {}),
                ("vertical", [[right, 0]], {}),
                ("vertical", [[left, 0]], {}),
                ("coincident", [[bot, 0], [0xFFFFFF00, 0]], {}),
                ("distance", [[bot, 0], [bot, 1]], {"expr": "width"}),
                ("distance", [[right, 0], [right, 1]], {"expr": "height"})]:
            d.call("add_constraint", sketch_id=sk["id"], kind=kind, refs=refs,
                   **kw)
        d.call("fillet", sketch_id=sk["id"], line1=top, line2=left, radius=4.0)
        pad = d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": sk["id"],
            "distance": 1.0, "distance_expr": "thickness",
            "mode": "new_body"})
        expected_volume = 40 * 20 * 5 - (16 - math.pi * 4) * 5
        state = d.call("get_state")
        expect(not any("error" in f for f in state["features"]),
               "no feature errors")
        expect(abs(state["bodies"][0]["volume"] - expected_volume) < 1e-2,
               "bracket volume")

        phase("Mass properties")
        # Mass properties: aluminum bracket, analytic mass/CoM (I2)
        d.call("set_material", feature_id=pad["id"], material="aluminum")
        mp = d.call("mass_properties")
        b = mp["bodies"][0]
        expect(abs(b["volume"] - expected_volume) < 1e-2, "massprops volume")
        expect(abs(b["mass_g"] - 2700 * expected_volume * 1e-6) < 1e-3,
               "aluminum mass in grams")
        expect(abs(b["center_of_mass"][2] - 2.5) < 1e-6, "CoM mid-thickness")
        expect(abs(mp["total"]["mass_g"] - b["mass_g"]) < 1e-9, "total mass")
        surface = b["surface_area"]
        expect(surface > 0, "surface area computed")
        inertia = b["inertia"]
        expect(len(inertia) == 3 and len(inertia[0]) == 3, "inertia is 3x3")

        phase("Unassigned material -> mass is null, never guessed")
        # Unassigned material -> mass is null, never guessed
        d.call("set_material", feature_id=pad["id"], material="")
        mp = d.call("mass_properties")
        expect(mp["bodies"][0]["mass_g"] is None, "unknown density: no mass")
        expect(mp["total"]["mass_g"] is None, "unknown density: no total")

        phase("Measure over element names")
        # Measure over element names (I1): the pad's cap face area, and a
        # top-face edge length.
        pels = d.call("list_elements")["bodies"][0]
        face = next(f["name"] for f in pels["faces"])
        marea = d.call("measure", a={"feature_id": pels["created_by"],
                                     "element": face})
        expect(marea["kind"] == "face" and marea["area"] > 0,
               "measure face area")
        edge = pels["edges"][0]["name"]
        mlen = d.call("measure", a={"feature_id": pels["created_by"],
                                    "element": edge})
        expect(mlen["kind"] == "edge" and mlen["length"] > 0,
               "measure edge length")
        try:
            d.call("measure", a={"feature_id": pels["created_by"],
                                 "element": "nonsense"})
            expect(False, "measuring a dangling ref must error")
        except RuntimeError:
            pass

        phase("Element names drive a 3D chamfer headless")
        # Element names drive a 3D chamfer headless (T5 consumer over RPC)
        els = d.call("list_elements")["bodies"][0]
        expect(els["created_by"] == pad["id"], "created_by is the pad")
        expect(len(els["edges"]) > 0, "edges are named")

        # list_elements enrichment: every face carries kind+area, every edge
        # kind+length; the bracket's sketch fillet extrudes to a cylindrical
        # face and radius-4 arc edges.
        expect(all("kind" in f and f["area"] > 0 for f in els["faces"]),
               "faces carry kind and area")
        expect(all("kind" in e and e["length"] > 0 for e in els["edges"]),
               "edges carry kind and length")
        fkinds = {f["kind"] for f in els["faces"]}
        expect("plane" in fkinds and "cylinder" in fkinds,
               f"bracket faces classify plane + cylinder: {fkinds}")
        arcs = [e for e in els["edges"] if e["kind"] == "arc"]
        expect(arcs and all(abs(e["radius"] - 4.0) < 1e-6 for e in arcs),
               "fillet arc edges report radius 4")
        expect(any(e["kind"] == "line" for e in els["edges"]),
               "straight edges classify as line")
        before = d.call("get_state")["bodies"][0]["volume"]
        d.call("add_feature", feature={
            "type": "Chamfer", "name": "Break", "radius": 0.5,
            "edges": [{"feature_id": els["created_by"],
                       "element": els["edges"][0]["name"]}]})
        state = d.call("get_state")
        expect(not any("error" in f for f in state["features"]),
               "chamfer regenerated clean")
        expect(state["bodies"][0]["volume"] < before, "chamfer removed volume")
        d.call("undo")

        phase("Project round-trip + undo/redo")
        # Project round-trip + undo/redo
        project = os.path.join(tmp, "bracket.json")
        d.call("save_project", path=project)
        d.call("new_project")
        expect(len(d.call("get_state")["features"]) == 0, "cleared")
        d.call("open_project", path=project)
        state = d.call("get_state")
        expect(abs(state["bodies"][0]["volume"] - expected_volume) < 1e-2,
               "reloaded volume")
        d.call("undo")  # camera/view state functions without a GUI
        v = d.call("view", name="front")
        offset = [p - t for p, t in zip(v["position"], v["target"])]
        # Z-up world: the front-view camera sits toward -Y (docs/conventions.md#world-axes).
        expect(abs(offset[0]) < 1e-4 and offset[1] < 0, "front view headless")
        # Direct camera control (set_camera) is pure state — works headless.
        v = d.call("set_camera", target=[1.0, 2.0, 3.0], distance=250.0)
        expect(abs(v["distance"] - 250.0) < 1e-3, "set_camera distance")
        expect(abs(v["target"][1] - 2.0) < 1e-6, "set_camera target")
        v = d.call("set_camera", direction=[0, 0, 1])
        expect(v["position"][2] > v["target"][2], "set_camera direction")
        # An `up` along the line of sight sets no roll; within 1 degree of
        # it is refused, naming the angle, and the camera does not move.
        before = d.call("set_camera")["position"]
        for kw, angle in (
                (dict(direction=[0, 0, 1], up=[0, 0, 1]), "0 degrees"),
                (dict(direction=[0, 0, 1], up=[0.01, 0, 1]), "0.573 degrees"),
                (dict(target=[0, 0, 0], eye=[0, 0, 80], up=[0, 0, -1]),
                 "0 degrees"),
                (dict(up=[0, 0, 1]), "degrees from the view")):  # current
            try:
                d.call("set_camera", **kw)
                raise AssertionError(f"set_camera accepted {kw}")
            except RuntimeError as e:
                expect(angle in str(e) and "at least 1 degree" in str(e),
                       f"set_camera {kw} names the angle and the need: {e}")
        expect(d.call("set_camera")["position"] == before,
               "a refused set_camera leaves the camera where it was")
        v = d.call("set_camera", direction=[0, 0, 1], up=[0.03, 0, 1])
        expect(v["position"][2] > v["target"][2], "1.7 degrees is enough")
        # ...and on an ASSEMBLY tab (viewport state, not part state).
        d.call("add_assembly", name="CamAsm")
        d.call("set_active_part", name="CamAsm")
        v = d.call("set_camera", target=[5.0, 0.0, 0.0], distance=100.0)
        expect(abs(v["distance"] - 100.0) < 1e-3, "set_camera on assembly")
        idx = [pp["index"] for pp in d.call("list_parts")["parts"]
               if pp["name"] == "CamAsm"][0]
        d.call("set_active_part", index=0)
        d.call("delete_part", index=idx)
        d.call("reset_layout")  # harmless no-op headless (flag only)

        # --- A fit frames the PICTURE, not just the viewport --------------
        # `fit_view` solved for the viewport's aspect while `render_still`
        # projects at the size it was asked for, so fit-then-render cropped
        # whenever the two differed: a fifth of a 210 mm panel in frame at
        # 400x1100, correctly projected and framed for a different picture
        # (docs/rendering/still-render.md#framing). Checked by projecting the
        # part's own corners through the reported camera, which is why the
        # camera block reports fov_deg and aspect at all — and it needs no
        # GPU, unlike the picture it is about.
        d.call("set_active_part", index=0)
        st = d.call("get_state")
        cam = st["camera"]
        expect("fov_deg" in cam and "aspect" in cam,
               f"the camera says what it SEES, not only where it is: {cam}")
        lo = st["bodies"][0]["bbox_min"]
        hi = st["bodies"][0]["bbox_max"]
        pts = [[lo[0] if c & 1 else hi[0], lo[1] if c & 2 else hi[1],
                lo[2] if c & 4 else hi[2]] for c in range(8)]

        def worst_ndc(camera, aspect):
            """How far the furthest corner lands from the frame's centre,
            in NDC: over 1 is outside the picture."""
            eye, tgt = camera["position"], camera["target"]
            f = [t - e for t, e in zip(tgt, eye)]
            n = math.sqrt(sum(v * v for v in f))
            f = [v / n for v in f]
            up = [0, 0, 1] if abs(f[2]) < 0.99 else [0, 1, 0]
            r = [f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                 f[0] * up[1] - f[1] * up[0]]
            n = math.sqrt(sum(v * v for v in r))
            r = [v / n for v in r]
            u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2],
                 r[0] * f[1] - r[1] * f[0]]
            tv = math.tan(math.radians(camera["fov_deg"]) / 2.0)
            th = tv * aspect
            out = 0.0
            for pt in pts:
                v = [a - b for a, b in zip(pt, eye)]
                depth = max(sum(v[i] * f[i] for i in range(3)), 1e-3)
                out = max(out,
                          abs(sum(v[i] * r[i] for i in range(3))
                              / (depth * th)),
                          abs(sum(v[i] * u[i] for i in range(3))
                              / (depth * tv)))
            return out

        # A plain fit frames for the VIEWPORT, so for a picture of a very
        # different shape it is not enough — asserted so this cannot pass
        # vacuously if the aspect stops being threaded through: without it
        # both calls would frame identically and this line would fail.
        d.call("view", name="top")
        d.call("fit_view")
        viewport_fit = worst_ndc(d.call("get_state")["camera"], 400 / 1100)
        expect(viewport_fit > 1.0,
               f"a fit for aspect {cam['aspect']:.3f} should NOT already "
               f"frame a 400x1100 picture (got {viewport_fit:.3f}) — if it "
               f"does, this check has lost its teeth")
        for w, h in ((400, 1100), (1200, 800), (2000, 500)):
            d.call("view", name="top")
            d.call("fit_view", width=w, height=h)
            got = worst_ndc(d.call("get_state")["camera"], w / h)
            expect(got <= 1.0,
                   f"fit for {w}x{h} left a corner {got:.3f} out of frame")
        # Both or neither: half an aspect is not an aspect.
        expect_error(d, "fit_view", "width AND height", width=800)

        phase("Autosave keeps the whole project, not the open tab")
        # F10. This is the scenario that used to fail: Autosaver was handed
        # the ACTIVE document, so a multi-document project autosaved one part
        # and an assembly tab autosaved nothing at all.
        multi = os.path.join(tmp, "multi.dsgn")
        d.call("new_project")
        p1 = shapes.box(d, (0, 0, 0), (10, 10, 10), "PartOneBody")
        d.call("add_part", name="Second")
        d.call("set_active_part", name="Second")
        shapes.box(d, (0, 0, 0), (20, 20, 20), "PartTwoBody")
        d.call("add_assembly", name="Asm")
        d.call("save_project", path=multi)
        sidecars = multi + ".autosave.d"
        expect(not os.path.isdir(sidecars), "a clean save leaves no sidecars")

        # Dirty a part that is NOT the open tab, and the assembly tab too.
        d.call("set_active_part", name="Part 1")
        grow = dict(next(f for f in d.call("get_state")["features"]
                         if f["id"] == p1["id"]), distance=44)
        d.call("modify_feature", feature_id=p1["id"], feature=grow)
        d.call("set_active_part", name="Asm")  # an assembly tab is open
        d.call("autosave_now")
        expect(os.path.isdir(sidecars), "the project autosaved from an "
                                        "assembly tab (it used to autosave "
                                        "nothing at all there)")
        recovered = d.call("autosave_status")["documents"]
        expect("parts/Part 1" in recovered,
               f"the DIRTY part was kept even though another tab was open: "
               f"{recovered}")

        # And a clean save clears every sidecar.
        d.call("save_project", path=multi)
        expect(not os.path.isdir(sidecars), "saving clears the sidecars")

        phase("A project that moved on disk refuses to be overwritten")
        # F8. The scenario is the terminal panel: you run a git command (or
        # another editor writes, or a second instance saves) while the app
        # holds the documents in memory. Before this, the next Ctrl+S replaced
        # it all with no message.
        moved = os.path.join(tmp, "moved.dsgn")
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (2, 2, 2), "Box 1")
        d.call("save_project", path=moved)
        expect(not d.call("project_changed_on_disk")["changed"],
               "a project just saved has not moved")
        # A save with nothing external in between still works — the app's own
        # writes must never read as somebody else's.
        d.call("save_project", path=moved)

        # Now somebody else writes. A new file is the least ambiguous form.
        with open(os.path.join(moved, "NOTES.txt"), "w") as f:
            f.write("written by something that is not this app\n")
        expect(d.call("project_changed_on_disk")["changed"],
               "an external write is noticed")
        expect_error(d, "save_project", "changed on disk", path=moved)
        expect(os.path.exists(os.path.join(moved, "NOTES.txt")),
               "the refused save left the external write alone")

        # force is the way through for a caller that did the changing.
        d.call("save_project", path=moved, force=True)
        expect(not d.call("project_changed_on_disk")["changed"],
               "saving re-stamps, so the same change is not reported twice")

        # Reopening also re-stamps: the app and the disk agree again.
        with open(os.path.join(moved, "NOTES2.txt"), "w") as f:
            f.write("and again\n")
        expect(d.call("project_changed_on_disk")["changed"], "noticed again")
        d.call("open_project", path=moved)
        expect(not d.call("project_changed_on_disk")["changed"],
               "opening agrees with what is on disk")

        # A Save As to a path we have never seen has nothing to compare
        # against, and must not be blocked by the stamp of a different project.
        elsewhere = os.path.join(tmp, "elsewhere.dsgn")
        with open(os.path.join(moved, "NOTES3.txt"), "w") as f:
            f.write("moved again\n")
        d.call("save_project", path=elsewhere)

        phase("Importing a broken STL")
        # Importing a broken STL (zero triangles) names the file and format
        # instead of a generic regeneration failure. (Valid STLs import as
        # sewn shells/solids — covered in the interchange section below.)
        stl_path = os.path.join(tmp, "mesh.stl")
        with open(stl_path, "wb") as fh:
            fh.write(b"\0" * 84)
        r = d.call("add_feature",
                   feature={"type": "Import", "name": "Mesh",
                            "path": stl_path})
        failed = r["regen"]["failed"]
        expect(failed and "as STL" in failed[0]["error"],
               f"STL import error is specific: {failed}")
        d.call("delete_feature", index=len(d.call("get_state")["features"]) - 1)

        phase("A SECOND simultaneous client works")
        # A SECOND simultaneous client works (multi-client remote): both see
        # the same document and responses route to the right connection.
        d2 = Designer(app.port)
        expect(len(d2.call("get_state")["bodies"]) == 1, "second client reads")
        expect(len(d.call("get_state")["bodies"]) == 1, "first client still ok")
        d2.close()

        phase("Every GUI-dependent method degrades with its specific error")
        # Every GUI-dependent method degrades with its specific error
        # "a rendered frame", not "the GUI": with -DENABLE_TEST_ENGINE=OFF a
        # windowed build has no test engine and still screenshots fine, so the
        # two capabilities are named separately (docs/scripting/mcp.md#the-tool-surface).
        expect_error(d, "screenshot", "requires a rendered frame",
                     path="/tmp/x.png")
        expect_error(d, "menu_click", "unavailable in --headless",
                     path="Edit/Undo")
        expect_error(d, "viewport_drag", "unavailable in --headless")
        expect_error(d, "key_press", "unavailable in --headless",
                     chord="Ctrl+Z")
        expect_error(d, "select_face", "unavailable in --headless",
                     mesh=0, face=0)
        expect_error(d, "orient_to_face", "unavailable in --headless")
        r = d.call("render_still", width=8, height=8, samples=1,
                   path=os.path.join(tmp, "no.png"))
        expect(not r["ok"] and "not available" in r["error"],
               "render_still reports unavailable")

        # Rib (v36): an open profile above a box grows a stiffener down to it
        # — the generic add_feature path carries the new type.
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (10, 10, 10), "Base")
        rib_sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "RibProfile",
            "plane": {"origin": [5, 5, 15], "normal": [0, 0, -1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [-3, 0], "b": [3, 0]}]})
        d.call("add_feature", feature={
            "type": "Rib", "name": "Stiffener", "sketch_id": rib_sk["id"],
            "thickness": 1.0})
        state = d.call("get_state")
        expect(not any("error" in f for f in state["features"]),
               "rib regenerated clean")
        expect(state["bodies"][0]["volume"] > 1000.0 + 1.0, "rib added volume")

        phase("Cosmetic thread")
        # Cosmetic thread (v36): annotates a cylindrical face, geometry
        # untouched.
        cyl = shapes.cylinder(d, (30, 0, 0), "z", 4.0, 20.0, "Boss")
        els = next(b for b in d.call("list_elements")["bodies"]
                   if b["created_by"] == cyl["id"])
        boss_before = next(b for b in d.call("get_state")["bodies"]
                           if abs(b["volume"] - math.pi * 16 * 20) < 1e-3)
        d.call("add_feature", feature={
            "type": "Thread", "name": "M8", "designation": "M8",
            "pitch": 1.25,
            "face": {"feature_id": cyl["id"],
                     "element": els["faces"][0]["name"]}})
        state = d.call("get_state")
        expect(not any("error" in f for f in state["features"]),
               "thread regenerated clean")
        boss_after = next(b for b in state["bodies"]
                          if abs(b["volume"] - math.pi * 16 * 20) < 1e-3)
        expect(abs(boss_after["volume"] - boss_before["volume"]) < 1e-9,
               "cosmetic thread left the volume untouched")

        phase("Ellipse radii through add_constraint, and refusals")
        # An ellipse is solved: its radii are distances from the centre to
        # its axis ends (which 3 major, 5 minor), driven by a variable; a
        # radius on it is REFUSED naming that form, and the sketch is left
        # alone (docs/sketching/README.md#refusals).
        d.call("new_project")
        d.call("set_variable", name="oval_a", expression="30")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Oval",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "ellipse", "center": [0, 0], "r_major": 20,
                 "r_minor": 8, "rotation_deg": 25},
                {"type": "spline", "poles": [[40, 0], [45, 10], [55, 10],
                                             [60, 0]]},
                {"type": "circle", "center": [80, 0], "radius": 3}]})
        el, sp, ci = sk["entity_ids"]
        expect_error(d, "add_constraint", "dimension `distance` from its "
                     "centre", sketch_id=sk["id"], kind="radius",
                     refs=[[el, 0]], value=5)
        expect_error(d, "add_constraint", "a spline is tangent only",
                     sketch_id=sk["id"], kind="tangent",
                     refs=[[ci, 0], [sp, 0]])
        expect_error(d, "add_constraint", "Known kinds:", sketch_id=sk["id"],
                     kind="radius_major", refs=[[el, 0]], value=5)
        d.call("add_constraints", sketch_id=sk["id"], constraints=[
            {"kind": "fixed", "refs": [[el, 0]]},
            {"kind": "horizontal", "refs": [[el, 0]]},
            {"kind": "distance", "refs": [[el, 0], [el, 3]],
             "expr": "oval_a"},
            {"kind": "distance", "refs": [[el, 0], [el, 5]], "value": 10}])
        state = d.call("get_state")
        sk_state = [f for f in state["features"] if f["id"] == sk["id"]][0]
        expect("error" not in sk_state and "warning" not in sk_state,
               "the oval sketch solved clean")
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Oval pad", "sketch_id": sk["id"],
            "distance": 2.0, "mode": "new_body",
            "regions": [[el]]})
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - math.pi * 30 * 10 * 2) < 1e-2,
               f"the oval pads at its dimensioned size ({vol})")
        d.call("set_variable", name="oval_a", expression="40")
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - math.pi * 40 * 10 * 2) < 1e-2,
               f"the oval follows its variable ({vol})")

        phase("Draft")
        # Draft (M8): taper a box's side faces; volume drops
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (10, 10, 10), "Blk")
        bx = d.call("list_elements")["bodies"][0]
        # A side face: pick the first whose name resolves; the feature rejects
        # caps, so try faces until one drafts. Use get_state face normals via
        # measure is overkill — just try face names, catching the cap error.
        vol0 = d.call("get_state")["bodies"][0]["volume"]
        drafted = False
        for f in bx["faces"]:
            try:
                idx = d.call("add_feature", feature={
                    "type": "Draft", "name": "Taper",
                    "body_id": bx["created_by"], "angle_deg": 8.0,
                    "neutral_plane": "xy",
                    "faces": [{"feature_id": bx["created_by"],
                               "element": f["name"]}]})
                st = d.call("get_state")
                if not any("error" in ff for ff in st["features"]):
                    expect(st["bodies"][0]["volume"] < vol0,
                           "draft reduced the volume")
                    drafted = True
                    break
                d.call("delete_feature", index=idx["index"])
            except RuntimeError:
                pass
        expect(drafted, "at least one side face drafted")

        phase("Hole wizard")
        # Hole wizard (M8): a 4-hole bolt pattern from one point sketch,
        # then interference (I4) between two boxes.
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (10, 10, 4), "Plate")
        holes = d.call("add_feature", feature={
            "type": "Sketch", "name": "Bolts",
            "plane": {"origin": [0, 0, 4], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "point", "p": [2, 2]},
                         {"type": "point", "p": [8, 2]},
                         {"type": "point", "p": [8, 8]},
                         {"type": "point", "p": [2, 8]}]})
        d.call("add_feature", feature={
            "type": "Hole", "name": "BoltHoles", "sketch_id": holes["id"],
            "hole_kind": "simple", "diameter": 1.0, "depth": 1.0,
            "through": True})
        import math as _m3
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "holes regenerated clean")
        expect(abs(st["bodies"][0]["volume"]
                   - (400.0 - 4.0 * _m3.pi * 0.25 * 4.0)) < 0.5,
               "4-hole plate volume")

        phase("Multi-part project")
        # Multi-part project (assemblies A1): two parts, switch, save/load
        d.call("new_project")
        expect(d.call("list_parts")["parts"][0]["name"] == "Part 1",
               "starts with one part")
        shapes.box(d, (0, 0, 0), (2, 2, 2), "A")
        d.call("add_part", name="Arm")
        shapes.box(d, (0, 0, 0), (3, 3, 3), "B")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 27) < 1e-6,
               "part 2 has its own box")
        d.call("set_active_part", index=0)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8) < 1e-6,
               "part 1 intact after switching back")
        # Save + reload the whole multi-part project.
        proj = os.path.join(tmp, "asm.dsgn")
        d.call("save_project", path=proj)
        expect(os.path.exists(os.path.join(proj, "project.json")),
               "multi-part writes project.json")
        d.call("new_project")
        d.call("open_project", path=proj)
        parts = d.call("list_parts")["parts"]
        expect([p["name"] for p in parts] == ["Part 1", "Arm"],
               "both parts reloaded")
        d.call("set_active_part", index=1)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 27) < 1e-6,
               "reloaded part 2 geometry")
        # #10: rename an existing part (the default "Part 1" isn't permanent).
        # Resolvable by name; occurrences reference parts by stable id, so the
        # rename never breaks a reference.
        pid0 = next(p for p in parts if p["name"] == "Part 1")["id"]
        d.call("rename_part", part="Part 1", new_name="Wheel")
        renamed = d.call("list_parts")["parts"]
        expect(renamed[0]["name"] == "Wheel" and renamed[0]["id"] == pid0,
               "rename_part renames the slot, keeps its stable id")
        # Also resolvable by slot index; bogus part errors.
        d.call("rename_part", part=0, new_name="Hub")
        expect(d.call("list_parts")["parts"][0]["name"] == "Hub",
               "rename_part resolves a part by slot index")
        expect_error(d, "rename_part", "no part", part="ghost", new_name="X")
        d.call("rename_part", part="Hub", new_name="Part 1")  # restore
        # Delete a part; can't delete the last.
        d.call("delete_part", index=1)
        expect(len(d.call("list_parts")["parts"]) == 1, "part deleted")
        try:
            d.call("delete_part", index=0)
            expect(False, "deleting the last part must fail")
        except RuntimeError:
            pass

        phase("What works on a part tab works on the others")
        # The recurring defect shape in this codebase: something is built and
        # tested on a part tab and is quietly dead on an assembly or drawing
        # tab. The section arrow was this twice, autosave wrote nothing at
        # all on a drawing tab, the sharing RPCs were part-gated, and a
        # drawing-tab dogfooding round found six more. The part gate is a
        # LIST kept apart from the handlers, so widening a handler does not
        # widen its gate — three of those six say in their own comments that
        # they were fixed to cover the whole project, and stayed gated.
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (10, 10, 10), "B")
        d.call("add_assembly", name="Rig")
        d.call("create_drawing", name="Sheet")
        proj = os.path.join(tmp, "tabkinds.dsgn")
        d.call("save_project", path=proj)
        for tab in ["Rig", "Sheet"]:
            d.call("set_active_part", name=tab)
            # Preferences are the user's, not the document's.
            d.call("get_settings")
            d.call("set_settings", ui_font_scale=1.2)
            # The dock layout is the WINDOW's — and resetting it is the
            # recovery for a wedged one, so gating it is backwards.
            d.call("reset_layout")
            # Versioning is whole-project: a checkpoint taken from a drawing
            # tab still has to cover the parts.
            d.call("checkpoint_create", name="cp-from-" + tab)
            names = [c.get("name") for c in
                     d.call("checkpoint_list")["checkpoints"]]
            expect("cp-from-" + tab in names,
                   f"checkpoint from the {tab} tab: {names}")
        # And a typo'd method on a non-part tab has to say what is actually
        # wrong. The gate ran before the unknown-method check, so any
        # misspelling answered "switch to a part tab" and sent you hunting a
        # tab problem that was not there — which is how this very test found
        # out that list_versions is called checkpoint_list.
        try:
            d.call("list_versions")
            expect(False, "list_versions should not exist")
        except RuntimeError as e:
            expect("unknown method" in str(e).lower(), f"typo on a {tab} tab "
                   f"names the method, not the tab: {e}")

        phase("Tab folders")
        # Folders are display grouping for the bottom tab bar — the thing a
        # 100-part STEP import needs so it is one flyout, not 100 tabs. They
        # group tabs of EVERY kind, and they survive a save/reload: a folder
        # that evaporates on open is worse than no folder.
        d.call("new_project")
        d.call("add_part", name="Bolt")
        d.call("add_part", name="Nut")
        d.call("add_assembly", name="Rig")
        d.call("create_folder", folder="Fasteners")
        r = d.call("create_folder", folder="Fasteners")
        expect(r["created"] is False and r["folders"] == ["Fasteners"],
               f"create_folder is idempotent: {r}")
        r = d.call("move_to_folder", part=["Bolt", "Nut"], folder="Fasteners")
        expect([m["name"] for m in r["moved"]] == ["Bolt", "Nut"],
               f"both tabs moved in one call: {r}")
        # A typo must be an error, not a second folder one letter apart.
        expect_error(d, "move_to_folder", "no folder",
                     part="Rig", folder="Fastners")
        # ...and nothing moved: the whole call is resolved before any of it
        # is applied.
        expect_error(d, "move_to_folder", "no part",
                     part=["Bolt", "ghost"], folder="Fasteners")
        folders = {p["name"]: p.get("folder", "")
                   for p in d.call("list_parts")["parts"]}
        expect(folders["Bolt"] == "Fasteners" and folders["Rig"] == "",
               f"failed move changed nothing: {folders}")
        # An assembly or a drawing tab goes in a folder like any other —
        # folders group tabs, not parts.
        d.call("create_drawing", name="Sheet")
        d.call("move_to_folder", part=["Rig", "Sheet"], folder="Fasteners")

        # Survives the round trip.
        fproj = os.path.join(tmp, "folders.dsgn")
        d.call("save_project", path=fproj)
        d.call("new_project")
        d.call("open_project", path=fproj)
        back = d.call("list_parts")
        expect(back["folders"] == ["Fasteners"], f"folder reloaded: {back}")
        expect(all(p.get("folder") == "Fasteners"
                   for p in back["parts"] if p["name"] != "Part 1"),
               f"membership reloaded: {back}")

        r = d.call("rename_folder", folder="Fasteners", new_name="Hardware")
        expect(r["merged"] is False and r["folders"] == ["Hardware"],
               f"rename_folder: {r}")
        # Renaming ONTO an existing folder merges — reported, because that is
        # not what "rename" implies.
        d.call("create_folder", folder="Spares")
        d.call("move_to_folder", part="Nut", folder="Spares")
        r = d.call("rename_folder", folder="Spares", new_name="Hardware")
        expect(r["merged"] is True and r["folders"] == ["Hardware"],
               f"rename onto an existing folder merges: {r}")
        # "" is how a tab leaves a folder.
        d.call("move_to_folder", part="Bolt", folder="")
        expect(d.call("list_parts")["parts"][1].get("folder", "") == "",
               "moved back to the top level")
        # Dissolving frees its tabs; it never deletes documents.
        r = d.call("dissolve_folder", folder="Hardware")
        expect([m["name"] for m in r["moved_to_top_level"]]
               == ["Nut", "Rig", "Sheet"], f"dissolve frees its tabs: {r}")
        after = d.call("list_parts")
        expect("folders" not in after, f"no folders left: {after}")
        expect(len(after["parts"]) == 5, f"documents survive: {after}")
        expect_error(d, "dissolve_folder", "no folder", folder="Hardware")

        phase("Project-wide variables")
        # Project-wide variables (Phase V): ONE variable drives geometry in two
        # parts; editing it changes both; a part-scoped var shadows it locally;
        # an undefined variable is a loud feature error (not a silent literal).
        def rect(w, h):
            return [{"type": "line", "a": [0, 0], "b": [w, 0]},
                    {"type": "line", "a": [w, 0], "b": [w, h]},
                    {"type": "line", "a": [w, h], "b": [0, h]},
                    {"type": "line", "a": [0, h], "b": [0, 0]}]
        d.call("new_project")
        r = d.call("set_variable", name="bore", expression="6")  # default: project
        expect(r["scope"] == "project", "set_variable defaults to project scope")
        sk1 = d.call("add_feature", feature={
            "type": "Sketch", "name": "S1",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]}, "entities": rect(4, 2)})
        # #4: no literal `distance` key at all — only `distance_expr`. The
        # deserializer must default the literal (like Revolve's `angle`) so the
        # expr alone builds a body instead of erroring on a missing key.
        p1 = d.call("add_feature", feature={
            "type": "Extrude", "name": "P1", "sketch_id": sk1["id"],
            "distance_expr": "bore", "mode": "new_body"})
        expect("error" not in p1, f"extrude with only distance_expr built: {p1}")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 6) < 1e-3,
               "part 1 driven by the project variable (distance_expr only)")
        d.call("add_part", name="Two")
        sk2 = d.call("add_feature", feature={
            "type": "Sketch", "name": "S2",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]}, "entities": rect(5, 2)})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "P2", "sketch_id": sk2["id"],
            "distance": 1.0, "distance_expr": "bore * 2", "mode": "new_body"})
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 10 * 12) < 1e-3,
               "part 2 driven by the SAME project variable")
        lv = d.call("list_variables")["variables"]
        expect(any(v["name"] == "bore" and v["scope"] == "project" for v in lv),
               "list_variables tags bore as project scope")
        # Edit the project variable: BOTH parts' geometry follows.
        d.call("set_variable", name="bore", expression="10")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 10 * 20) < 1e-3,
               "part 2 followed the project-variable edit")
        d.call("set_active_part", index=0)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 10) < 1e-3,
               "part 1 followed the project-variable edit too")
        # Project-variable edits are undoable across tabs (their own undo stack,
        # routed by the global most-recent-action rule).
        d.call("undo")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 6) < 1e-3,
               "undo reverted the project-variable edit")
        d.call("redo")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 10) < 1e-3,
               "redo re-applied the project-variable edit")
        # Save + reload preserves the project scope and both geometries.
        pvproj = os.path.join(tmp, "projvars.dsgn")
        d.call("save_project", path=pvproj)
        d.call("new_project")
        d.call("open_project", path=pvproj)
        lv = d.call("list_variables")["variables"]
        expect(any(v["name"] == "bore" and v["scope"] == "project"
                   and abs(v["value"] - 10) < 1e-9 for v in lv),
               "project variable reloaded at project scope")
        d.call("set_active_part", index=1)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 10 * 20) < 1e-3,
               "part 2 geometry survived the reload")
        # A part-scoped variable of the same name shadows the project one here.
        r = d.call("set_variable", name="bore", expression="1", scope="part")
        expect(r["scope"] == "part", "scope='part' honored")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 10 * 2) < 1e-3,
               "part-local bore shadows the project bore in THIS part")
        d.call("set_active_part", index=0)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 10) < 1e-3,
               "the other part still resolves the project bore")
        # Fail-loud: an expression over an undefined variable errors the feature.
        badsk = d.call("add_feature", feature={
            "type": "Sketch", "name": "S3",
            "plane": {"origin": [0, 0, 20], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]}, "entities": rect(2, 2)})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Bad", "sketch_id": badsk["id"],
            "distance": 1.0, "distance_expr": "no_such_var", "mode": "new_body"})
        st = d.call("get_state")
        expect(any("error" in f and "unknown variable" in f["error"]
                   for f in st["features"]),
               f"undefined variable is a loud feature error: {st['features']}")

        phase("Section")
        # Section (I3): cross-section area of a plate at a mid-cut
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (4, 5, 6), "P")
        # set_section just toggles the viewport clip (no visible effect
        # headless), but the RPC must accept and clear it without error.
        d.call("set_section", enabled=True, base_plane="yz", offset=0.0)
        d.call("set_section", enabled=False)
        # A plane nobody named is REFUSED, naming the three there are: every
        # section handler used to read "zx" (or "XY", or "front") as xy and
        # cut the model somewhere it was not asked to.
        expect_error(d, "set_section", "not one of xy, xz, yz",
                     enabled=True, base_plane="zx")
        expect_error(d, "add_section", "not one of xy, xz, yz",
                     base_plane="front")
        sec = d.call("section", base_plane="xy", offset=3.0)  # z=3 -> 4x5=20
        expect(abs(sec["area"] - 20.0) < 1e-3, "cross-section area 20")
        miss = d.call("section", base_plane="xy", offset=99.0)
        expect(abs(miss["area"]) < 1e-6, "section miss = 0 area")

        # P2/P3: several cuts, a tilt, a datum source, and persistence.
        d.call("remove_section", all=True)
        a = d.call("add_section", base_plane="xy", offset=2.0)
        b = d.call("add_section", base_plane="yz", offset=1.0, tilt_u=30.0)
        expect(a["id"] != b["id"], "each cut gets its own id")
        lst = d.call("list_sections")["sections"]
        expect(len(lst) == 2, f"two cuts: {lst}")
        expect(lst[0]["source"]["kind"] == "base_plane", "source named")
        # A tilt turns the plane: the cut normal is no longer the base axis.
        tilted = [s for s in lst if s["id"] == b["id"]][0]
        expect(abs(tilted["normal"][0] - 1.0) > 1e-3,
               f"30 deg tilt moved the normal off X: {tilted['normal']}")
        # ...and pivots ABOUT the offset point rather than sliding the cut
        # away. The test is that the tilted plane still passes through the
        # anchor (1,0,0) — dot(anchor, n) == d — NOT that d is unchanged: d
        # is measured along the new normal, so it is cos(30) here. A naive
        # implementation that kept d = offset would slide the cut off the
        # pivot and fail this.
        anchor = [1.0, 0.0, 0.0]
        through = sum(a * n for a, n in zip(anchor, tilted["normal"]))
        expect(abs(through - tilted["d"]) < 1e-3,
               f"tilt pivots about the offset point: {through} vs {tilted['d']}")
        expect(abs(tilted["d"] - 1.0) > 1e-3,
               "d is along the tilted normal, so it moved")

        # A datum plane as the source, resolved live.
        dat = d.call("add_feature", feature={
            "type": "DatumPlane", "name": "D", "kind": "offset",
            "base_plane": "xy", "offset": 1.5})
        upd = d.call("update_section", id=a["id"], plane_id=dat["id"],
                     offset=0.0)
        expect(abs(upd["d"] - 1.5) < 1e-3,
               f"cut sits on the datum: d={upd['d']}")
        # Move the datum (keeping its id) — the cut follows it, which is the
        # whole reason to reference one.
        d.call("modify_feature", index=dat["index"], feature={
            "type": "DatumPlane", "id": dat["id"], "name": "D",
            "kind": "offset", "base_plane": "xy", "offset": 4.0})
        after = [s for s in d.call("list_sections")["sections"]
                 if s["id"] == a["id"]][0]
        expect(abs(after["d"] - 4.0) < 1e-3,
               f"the cut followed the datum: d={after['d']}")
        expect(after["source"]["resolved"], "source still resolves")

        # And a source that VANISHES stops the cut rather than silently
        # cutting along some other plane: replacing the datum with a
        # genuinely DIFFERENT feature (a new id) leaves the reference
        # dangling.
        #
        # The id is explicit on purpose. This used to omit it and rely on
        # ReplaceFeature taking the incoming 0 verbatim — which destroyed the
        # identity of every feature the modify_feature RPC touched, wrote it
        # to disk as "0-box.json", and dangled every OTHER reference to it
        # too. Identity is now preserved unless you name a different one, so
        # "this is a different feature" has to be said out loud.
        d.call("modify_feature", index=dat["index"], feature={
            "type": "DatumPlane", "id": 9001, "name": "D2", "kind": "offset",
            "base_plane": "xy", "offset": 4.0})
        dangling = [s for s in d.call("list_sections")["sections"]
                    if s["id"] == a["id"]][0]
        expect(not dangling["source"]["resolved"],
               "a dangling datum reference is reported, not guessed")
        d.call("update_section", id=a["id"], base_plane="xy", offset=2.0)

        # Persistence: sections survive save + reopen (they were session
        # state until P3, so a set-up cut vanished on every save).
        with tempfile.TemporaryDirectory() as td:
            path = os.path.join(td, "sections.dsgn")
            d.call("save_project", path=path)
            d.call("new_project")
            expect(not d.call("list_sections")["sections"], "new = no cuts")
            d.call("open_project", path=path)
            back = d.call("list_sections")["sections"]
            expect(len(back) == 2, f"both cuts came back: {back}")
            kinds = sorted(s["source"]["kind"] for s in back)
            expect(kinds == ["base_plane", "base_plane"],
                   f"sources kept: {kinds}")
            tilt = [s for s in back if s["tilt_u"]][0]
            expect(abs(tilt["tilt_u"] - 30.0) < 1e-3, "tilt kept")
        d.call("remove_section", all=True)

        phase("Interference")
        # Interference: two overlapping boxes clash; disjoint ones don't.
        d.call("new_project")
        d.call("set_variable", name="b_x", expression="1")
        shapes.box(d, (0, 0, 0), (2, 2, 2), "A")
        shapes.box(d, (1, 0, 0), (2, 2, 2), "B", offset_expr=["b_x", "0"])
        clash = d.call("interference", a=0, b=1)
        expect(clash["clash"] and abs(clash["volume"] - 4.0) < 1e-3,
               "overlapping boxes clash with overlap volume 4")
        # Move B clear: its sketch's offset follows the variable.
        d.call("set_variable", name="b_x", expression="10")
        expect(not d.call("interference", a=0, b=1)["clash"],
               "disjoint boxes do not clash")

        phase("Assembly interference + cross-part queries")
        # Assembly interference + cross-part queries: two occurrences of one
        # cube part — overlapping placements clash; a flipped fastened mate
        # seats them flush (contact only) and the clash clears.
        d.call("new_project")
        boxf = shapes.box(d, (0, 0, 0), (2, 2, 2), "Cube")
        d.call("set_material", feature_id=boxf["id"], material="aluminum")
        cube = d.call("list_elements")["bodies"][0]
        # #8: every face reports its outward unit surface normal. A box's six
        # planar faces point along ±X/±Y/±Z; check magnitude ~1 and that the
        # full axis-aligned set is present (not the old all-zero placeholder).
        cnorms = [f["normal"] for f in cube["faces"]]
        expect(all(abs((n[0]**2 + n[1]**2 + n[2]**2) ** 0.5 - 1.0) < 1e-4
                   for n in cnorms), "box face normals are unit length")
        def _has(ax):
            return any(abs(n[ax] - 1) < 1e-4 for n in cnorms) and \
                   any(abs(n[ax] + 1) < 1e-4 for n in cnorms)
        expect(_has(0) and _has(1) and _has(2),
               f"box reports +/-X,Y,Z face normals: {cnorms}")
        d.call("add_assembly", name="asm")  # active tab is now the assembly
        o1 = d.call("add_occurrence", part="Part 1", fixed=True)["id"]
        o2 = d.call("add_occurrence", part="Part 1",
                    transform={"position": [1, 1, 1],
                               "orientation": [0, 0, 0, 1]})["id"]
        clash = d.call("interference", a=o1, b=o2)
        expect(clash["clash"] and abs(clash["volume"] - 1.0) < 1e-3,
               f"overlapping occurrences clash with volume 1: {clash}")
        # Occurrence names resolve too (rename first: two occurrences of one
        # part share a default name).
        d.call("rename_occurrence", id=o2, name="CubeB")
        occs = d.call("list_occurrences")["occurrences"]
        names = {o["id"]: o["name"] for o in occs}
        by_name = d.call("interference", a=names[o1], b="CubeB")
        # No pair named -> sweep every visible occurrence pair.
        sweep = d.call("interference")
        expect(sweep["clash"] and len(sweep["pairs"]) == 1
               and abs(sweep["pairs"][0]["volume"] - 1.0) < 1e-3,
               f"all-pairs sweep finds the clash: {sweep}")
        # Project-level reads work from the assembly tab (were part-gated).
        d.call("history_log")
        d.call("checkpoint_list")
        expect(by_name["clash"], "occurrence-name interference works")

        phase("Assembly mass_properties")
        # Assembly mass_properties (finding #5): aggregate both occurrences in
        # WORLD space. o1 (aluminum 2^3 cube) sits at the origin — local CoM
        # (1,1,1); CubeB is shifted +(1,1,1) — world CoM (2,2,2). The total
        # volume/mass are the sum, the world CoM the volume-weighted midpoint.
        amp = d.call("mass_properties")  # active tab is the assembly
        expect(len(amp["occurrences"]) == 2, f"two occurrence rows: {amp}")
        expect(abs(amp["total"]["volume"] - 16.0) < 1e-3,
               f"assembly total volume 16: {amp['total']}")
        one_mass = 2700 * 8 * 1e-6  # aluminum, 8 mm^3
        expect(abs(amp["total"]["mass_g"] - 2 * one_mass) < 1e-6,
               f"assembly total mass = 2x the part: {amp['total']}")
        com = amp["total"]["center_of_mass"]
        expect(all(abs(c - 1.5) < 1e-4 for c in com),
               f"world CoM at the midpoint (1.5,1.5,1.5): {com}")
        shifted = next(o for o in amp["occurrences"] if o["name"] == "CubeB")
        expect(all(abs(c - 2.0) < 1e-4 for c in shifted["center_of_mass"]),
               f"shifted occurrence world CoM at (2,2,2): {shifted}")
        # Hiding an occurrence drops it from the aggregate (BOM convention).
        d.call("set_occurrence_visible", id=o2, visible=False)
        amp2 = d.call("mass_properties")
        expect(len(amp2["occurrences"]) == 1 and
               abs(amp2["total"]["volume"] - 8.0) < 1e-3,
               f"hidden occurrence excluded: {amp2['total']}")
        d.call("set_occurrence_visible", id=o2, visible=True)

        phase("Revolute axial offset")
        # Revolute axial offset (finding #6a) + sign (finding #6c): a positive
        # offset seats CubeB along +Z of a's connector normal (a's outward face
        # normal), not merely coinciding origins. The 2-cube's top face sits at
        # z=2 with a +Z normal, so a +5 offset places CubeB's origin at z=7 —
        # positive = along the normal, no inversion.
        top_face = max((f for f in cube["faces"] if f["kind"] == "plane"),
                       key=lambda f: f["centroid"][2])
        rev = d.call("add_mate", type="revolute", name="pivot",
                     a={"occurrence": o1,
                        "face": {"feature_id": cube["created_by"],
                                 "element": top_face["name"]}},
                     b={"occurrence": o2, "origin": True},
                     flipped=False, offset=5.0)
        expect(rev.get("errored_mates", 0) == 0,
               f"revolute+offset mate solved: {rev}")
        o2occ = next(o for o in d.call("list_occurrences")["occurrences"]
                     if o["id"] == o2)
        expect(abs(o2occ["transform"]["position"][2] - 7.0) < 1e-3,
               f"positive offset seats CubeB at +Z (z=7): {o2occ}")
        # A negative offset seats it the other way (below the face): z = 2 - 5.
        d.call("set_mate", id=rev["id"], offset=-5.0)
        o2occ = next(o for o in d.call("list_occurrences")["occurrences"]
                     if o["id"] == o2)
        expect(abs(o2occ["transform"]["position"][2] - (-3.0)) < 1e-3,
               f"negative offset seats CubeB at -Z (z=-3): {o2occ}")
        # The mate's rotational DOF is RADIANS, and `rotation_deg` is the
        # named alias beside it. Nothing here covered `rotation` at all —
        # only `offset` — which is how a parameter that named no unit came
        # to be read as degrees by every caller: `set_mate(rotation=60)`
        # looked like 60 degrees and pinned the mate to 197.7, so a linkage
        # stepped 10, 20, 30 landed on 213, 66, 279. That reads as a loop
        # solver picking branches at random, and was diagnosed as one twice.
        d.call("set_mate", id=rev["id"], offset=0.0)
        d.call("set_mate", id=rev["id"], rotation=0.0)
        base = pose_of(d, o2)
        d.call("set_mate", id=rev["id"], rotation_deg=90.0)
        back = next(m for m in d.call("list_mates")["mates"]
                    if m["id"] == rev["id"])
        expect(abs(back["rotation_deg"] - 90.0) < 1e-3,
               f"rotation_deg round-trips: {back['rotation_deg']}")
        expect(abs(back["rotation"] - math.pi / 2) < 1e-6,
               f"...and `rotation` is the radians beside it: {back}")
        quarter = pose_of(d, o2)
        expect(quarter != base, "90 degrees actually moved it")
        # The two spellings are the same pose, which is the whole point of
        # the alias: whichever the caller reaches for, it means one angle.
        d.call("set_mate", id=rev["id"], rotation=math.pi / 2)
        expect(all(abs(a - b) < 1e-3 for a, b in zip(pose_of(d, o2), quarter)),
               "rotation=pi/2 and rotation_deg=90 are one pose")
        # And a whole turn is the identity in either spelling.
        d.call("set_mate", id=rev["id"], rotation_deg=360.0)
        expect(all(abs(a - b) < 1e-3 for a, b in zip(pose_of(d, o2), base)),
               "360 degrees returns to where 0 put it")
        # Both at once has no right answer, so it is refused rather than
        # resolved in favour of one of them.
        try:
            d.call("set_mate", id=rev["id"], rotation=1.0, rotation_deg=90.0)
            raise AssertionError("two angles for one mate were accepted")
        except RuntimeError as e:
            expect("not both" in str(e),
                   f"the refusal says what is wrong: {e}")
        d.call("set_mate", id=rev["id"], rotation=0.0)
        d.call("remove_mate", id=rev["id"])

        phase("Cross-part list_elements/measure from the ASSEMBLY tab")
        # Cross-part list_elements/measure from the ASSEMBLY tab (no
        # set_active_part round-trip): same names, enriched fields.
        pels = d.call("list_elements", part="Part 1")["bodies"][0]
        expect([f["name"] for f in pels["faces"]] ==
               [f["name"] for f in cube["faces"]],
               "cross-part list_elements matches the part's own listing")
        face = pels["faces"][0]
        m = d.call("measure", part="Part 1",
                   a={"feature_id": pels["created_by"],
                      "element": face["name"]})
        expect(abs(m["area"] - face["area"]) < 1e-9 and
               abs(m["area"] - 4.0) < 1e-6,
               "cross-part measure: cube face area 4")
        expect_error(d, "measure", "no part with id or index", part="Nope",
                     a={"feature_id": pels["created_by"],
                        "element": face["name"]})
        # #9: `part=<int>` accepts EITHER the stable id OR the slot index, and a
        # bogus number errors (no silent fall-through to the active part).
        pinfo = next(p for p in d.call("list_parts")["parts"]
                     if p["name"] == "Part 1")
        m_by_index = d.call("measure", part=pinfo["index"],
                            a={"feature_id": pels["created_by"],
                               "element": face["name"]})
        m_by_id = d.call("measure", part=pinfo["id"],
                         a={"feature_id": pels["created_by"],
                            "element": face["name"]})
        expect(abs(m_by_index["area"] - 4.0) < 1e-6 and
               abs(m_by_id["area"] - 4.0) < 1e-6,
               "measure resolves a part by slot index AND by stable id")
        expect_error(d, "measure", "no part with id or index", part=99999,
                     a={"feature_id": pels["created_by"],
                        "element": face["name"]})

        phase("Mate the two cubes face-to-face")
        # Mate the two cubes face-to-face (flipped=True: flush, opposing
        # normals) — mated-apart occurrences no longer clash.
        planar = next(f["name"] for f in pels["faces"]
                      if f["kind"] == "plane")
        mate = d.call("add_mate", type="fastened", name="seat",
                      a={"occurrence": o1,
                         "face": {"feature_id": pels["created_by"],
                                  "element": planar}},
                      b={"occurrence": o2,
                         "face": {"feature_id": pels["created_by"],
                                  "element": planar}},
                      flipped=True)
        expect(mate.get("errored_mates", 0) == 0, f"seat mate solved: {mate}")
        seated = d.call("interference", a=o1, b=o2)
        expect(not seated["clash"] and seated["volume"] < 1e-6,
               f"mated-apart occurrences do not clash: {seated}")

        phase("Mate-connector diagnostics")
        # Mate-connector diagnostics: a stale name that is a prefix of the
        # current names is called out per side, with suggestions.
        bad = d.call("add_mate", type="fastened", name="stale",
                     a={"occurrence": o1,
                        "face": {"feature_id": pels["created_by"],
                                 "element": planar + ";M9"}},
                     b={"occurrence": o2, "origin": True})
        err = bad.get("error", "")
        if not err:  # error may surface on the mate record instead
            lm = d.call("list_mates")["mates"]
            err = next(mm.get("error", "") for mm in lm
                       if mm.get("name") == "stale")
        expect("connector A" in err and names[o1] in err,
               f"error names the failing side + occurrence: {err}")
        expect(planar + ";M9" in err, f"error names the failing face: {err}")
        d.call("remove_mate", id=bad["id"])
        # A stale PREFIX (the real modified-by-later-feature case) suggests
        # the current names. Cube face names share the "<id>/g0f" style
        # prefix; use the planar name minus its last char.
        bad2 = d.call("add_mate", type="fastened", name="stale2",
                      a={"occurrence": o1,
                         "face": {"feature_id": pels["created_by"],
                                  "element": planar[:-1]}},
                      b={"occurrence": o2, "origin": True})
        err2 = bad2.get("error", "")
        expect("similar:" in err2 and planar in err2,
               f"stale-prefix error suggests current names: {err2}")
        d.call("remove_mate", id=bad2["id"])

        phase("Expression-driven sketch entities")
        # Expression-driven sketch entities (Decision 2A, v38): a variable
        # drives a circle radius directly; the edit ripples through the
        # sketch into the extrude volume, and survives save/load.
        d.call("new_project")
        d.call("set_variable", name="bore", expression="6")
        esk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Lobe",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 1,
                          "r_expr": "bore / 2"}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": esk["id"],
            "distance": 2.0, "mode": "new_body"})
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "expr-entity model regenerated clean")
        expect(abs(st["bodies"][0]["volume"] - math.pi * 9 * 2) < 1e-2,
               "circle radius follows bore/2 = 3")
        ent = st["features"][0]["entities"][0]
        expect(ent.get("r_expr") == "bore / 2",
               f"entity expr surfaces in get_state: {ent}")
        d.call("set_variable", name="bore", expression="10")
        st = d.call("get_state")
        expect(abs(st["bodies"][0]["volume"] - math.pi * 25 * 2) < 1e-2,
               "set_variable ripples the entity expr into the volume")
        eproj = os.path.join(tmp, "exprs.json")
        d.call("save_project", path=eproj)
        d.call("new_project")
        d.call("open_project", path=eproj)
        st = d.call("get_state")
        expect(abs(st["bodies"][0]["volume"] - math.pi * 25 * 2) < 1e-2,
               "entity exprs survive save/load")
        d.call("set_variable", name="bore", expression="4")
        st = d.call("get_state")
        expect(abs(st["bodies"][0]["volume"] - math.pi * 4 * 2) < 1e-2,
               "reloaded entity expr still drives the geometry")

        phase("Sweep")
        # Sweep (M8): circle along a straight path = a cylinder
        d.call("new_project")
        pf = d.call("add_feature", feature={
            "type": "Sketch", "name": "Prof",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 1}]})
        pa = d.call("add_feature", feature={
            "type": "Sketch", "name": "Path",
            "plane": {"origin": [0, 0, 0], "normal": [0, 1, 0],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [0, 0], "b": [0, 8]}]})
        d.call("add_feature", feature={
            "type": "Sweep", "name": "Pipe", "profile_id": pf["id"],
            "path_id": pa["id"], "mode": "new_body"})
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "sweep regenerated clean")
        import math as _m2
        expect(abs(st["bodies"][0]["volume"] - _m2.pi * 8) < 0.2,
               "swept cylinder volume ~8pi")

        phase("Bent path")
        # A bent tube (v117): the corners and the former, line-arc-line, and
        # `tube_schedule` reading the program back out of it. The count is
        # asserted against the array because it once came from a moved-from
        # vector and answered 0 with a tube in the list.
        d.call("new_project")
        rise, half, former = 60.0, 200.0, 45.0
        path = d.call("add_feature", feature={
            "type": "Curve3D", "name": "Centreline", "kind": "bent_path",
            "radius": former,
            "points": [[-half, 0, 0], [-half, 0, rise],
                       [half, 0, rise], [half, 0, 0]]})
        d.call("add_feature", feature={
            "type": "Pipe", "name": "Tube", "path_id": path["id"],
            "outer_diameter": 20.0, "wall_thickness": 1.5})
        sched = d.request("tube_schedule", {})
        expect(sched["count"] == len(sched["tubes"]) == 1,
               "tube_schedule counts the tubes it lists")
        tube = sched["tubes"][0]
        expect(len(tube["bends"]) == 2, "two corners, two bends")
        expect(all(abs(b["angle_deg"] - 90.0) < 1e-6 and
                   abs(b["radius_mm"] - former) < 1e-9 for b in tube["bends"]),
               "each bend is 90 degrees on the former")
        # Straights + arcs: the tangent length is R at a right angle, so the
        # legs lose R each and every corner adds a quarter of the former.
        legs = 2 * (rise - former) + (2 * half - 2 * former)
        want = legs + 2 * (math.pi / 2.0) * former
        expect(abs(tube["length_mm"] - want) < 1e-6,
               f"cut length {tube['length_mm']:.3f} is the arc length {want:.3f}")
        # A path whose straight is too short for its bends is refused where
        # it is ADDED, with that one reason: committed broken, the pipe the
        # caller added next failed with a second, false one ("must be an
        # open sketch chain or a 3D curve").
        before = len(d.call("get_state")["features"])
        try:
            d.call("add_feature", feature={
                "type": "Curve3D", "name": "Too tight", "kind": "bent_path",
                "radius": former,
                "points": [[0, 0, 0], [0, 0, 20], [100, 0, 20], [100, 0, 0]]})
            refused = None
        except Exception as e:  # noqa: BLE001 - the refusal is the point
            refused = str(e)
        expect(refused is not None and "20 mm" in refused
               and "open sketch chain" not in refused,
               f"a bent path that cannot be bent is refused once: {refused}")
        expect(len(d.call("get_state")["features"]) == before,
               "and nothing was committed for a pipe to run on")

        phase("Cut list")
        # A cut list is a list of members that EXIST (v118). Three ways it
        # used to be wrong, each one an order a shop could not fill.
        d.call("new_project")

        def tube_member(name, pts, od, wall, radius=76.2):
            cl = d.call("add_feature", feature={
                "type": "Curve3D", "name": name + " CL", "kind": "bent_path",
                "radius": radius, "points": pts})
            return d.call("add_feature", feature={
                "type": "Pipe", "name": name, "path_id": cl["id"],
                "outer_diameter": od, "wall_thickness": wall})

        # 1. A MIRRORED member is a member. Half a symmetric frame used to go
        # unordered, because the schedule walked curves and a mirror makes no
        # curve.
        seed = tube_member("Side brace",
                           [[100, 0, 0], [100, 0, 500], [100, 400, 500]],
                           25.4, 1.65)
        src = [b for b in d.call("get_state")["bodies"]
               if b.get("created_by") == seed["id"]][0]["created_by"]
        d.call("add_feature", feature={
            "type": "Pattern", "name": "Mirrored", "kind": "mirror",
            "source_id": src, "base_plane": "yz", "count": 2})
        sched = d.request("tube_schedule", {})
        expect(len(d.call("get_state")["bodies"]) == 2 and
               sched["members"] == 2 and sched["count"] == 1,
               "a mirrored member is ordered too, as a quantity of two")

        # 2. A COPED member is cut to the metal, not to the node. The
        # difference is 8.4 mm on a 19.05 brace into a 25.4 run, per end.
        d.call("new_project")
        run = tube_member("Run", [[0, -300, 0], [0, 300, 0]], 25.4, 2.4)
        brace = tube_member("Brace", [[400, 0, 0], [0, 0, 0]], 19.05, 1.65)
        d.call("add_feature", feature={
            "type": "Cope", "name": "Cope", "member_id": brace["id"],
            "against": [run["id"]]})
        rows = {t["name"]: t for t in d.request("tube_schedule", {})["tubes"]}
        cut = rows["Brace"]
        expect(abs(cut["path_length_mm"] - 400.0) < 1e-6,
               "the path is still the node-to-node 400 mm")
        expect(8.0 < cut["coped_off_mm"] < 9.0 and
               abs(cut["cut_length_mm"] + cut["coped_off_mm"] - 400.0) < 1e-6,
               f"the cope took {cut['coped_off_mm']:.2f} mm off what is cut")
        expect(len(d.call("get_state")["bodies"]) == 2,
               "and the run it was coped to is still there")

        # 3. Identical members collapse into one row that says how many.
        d.call("new_project")
        for i in range(4):
            tube_member(f"Brace {i + 1}",
                        [[0, i * 100.0, 0], [400, i * 100.0, 0]], 19.05, 1.65)
        sched = d.request("tube_schedule", {})
        expect(sched["count"] == 1 and sched["members"] == 4 and
               sched["tubes"][0]["quantity"] == 4 and
               len(sched["tubes"][0]["bodies"]) == 4,
               "four identical braces are one row of four")

        # 4. An UNCOPED member is cut to its whole path — including a BENT
        # one. Measuring along the path found only orthogonal projections at
        # first, and a tube's end face has none on the leg it sits at the end
        # of, so a two-bend hoop measured 1511 mm of its 2335 mm path: one
        # whole leg missing, and nothing said so.
        d.call("new_project")
        hoop_r = 76.2
        tube_member("Hoop", [[0, 0, 0], [0, 0, 900], [600, 0, 900],
                             [600, 0, 0]], 25.4, 2.4, radius=hoop_r)
        row = d.request("tube_schedule", {})["tubes"][0]
        expect(abs(row["cut_length_mm"] - row["path_length_mm"]) < 1e-6,
               "an uncoped bent member is cut to its whole developed path")
        legs = (900 - hoop_r) + (600 - 2 * hoop_r) + (900 - hoop_r)
        want_hoop = legs + 2 * (math.pi / 2.0) * hoop_r
        expect(abs(row["cut_length_mm"] - want_hoop) < 1e-6,
               f"and that is {want_hoop:.2f} mm: three legs and two arcs")

        phase("The order")
        # The files a shop is actually sent, read back to the same numbers.
        out = os.path.join(tmp, "cut_list.csv")
        d.call("set_tube_rules", material="steel")
        res = d.call("export_cut_list", path=out, stock_length_mm=6000.0,
                     kerf_mm=2.0)
        with open(out) as f:
            lines = [ln for ln in f.read().splitlines() if ln]
        header, first = lines[0], lines[1].split(",")
        expect(header.startswith("member,quantity,material,size,"
                                 "cut_length_mm"),
               "the cut list's columns follow the BOM's where they carry over")
        expect(abs(float(first[4]) - row["cut_length_mm"]) < 0.01,
               "the CSV carries the same cut length the schedule reports")
        expect(res["sticks"] and res["sticks"][0]["offcut_mm"] > 0.0 and
               res["sticks_needed"] >= 1,
               "and it says how many lengths of stock to buy")
        expect(all("size" in s for s in res["sticks"]),
               "packed per SIZE — a 19 mm member cannot come off a 25 mm "
               "stick")

        bends_csv = os.path.join(tmp, "bends.csv")
        res = d.call("export_bend_schedule", path=bends_csv)
        with open(bends_csv) as f:
            rows = [ln for ln in f.read().splitlines() if ln]
        expect(rows[0].startswith("member,bend,distance_mm,angle_deg,roll_deg"),
               "the bend schedule is one row per BEND, as a bender is set up")
        expect(len(rows) - 1 == res["bends"] == 2,
               "two bends, two rows")

        # Loft (M8): frustum between two squares on offset datum planes
        d.call("new_project")
        dp = d.call("add_feature", feature={
            "type": "DatumPlane", "name": "High", "kind": "offset",
            "base_plane": "xy", "offset": 6.0})
        a = d.call("add_feature", feature={
            "type": "Sketch", "name": "Bottom",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 3}]})
        b = d.call("add_feature", feature={
            "type": "Sketch", "name": "Top", "plane_ref": dp["id"],
            "plane": {"origin": [0, 0, 6], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 1}]})
        d.call("add_feature", feature={
            "type": "Loft", "name": "Cone", "sketch_ids": [a["id"], b["id"]],
            "mode": "new_body"})
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "loft regenerated clean")
        # Conical frustum r=3->1 over h=6: V = pi h/3 (R^2+Rr+r^2) = pi*2*13.
        import math as _m
        expect(abs(st["bodies"][0]["volume"] - _m.pi * 2 * 13) < 1.0,
               "loft frustum volume")

        phase("What the shop gets")
        # A DXF can carry a MARK — what the machine etches rather than cuts —
        # because thirty flat blanks arrive at a welding table looking alike.
        d.call("new_project")
        d.call("set_sheet_metal_rules", material="steel", gauge="3mm")
        plate_sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Plate",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "line", "a": [0, 0], "b": [200, 0]},
                {"type": "line", "a": [200, 0], "b": [200, 120]},
                {"type": "line", "a": [200, 120], "b": [0, 120]},
                {"type": "line", "a": [0, 120], "b": [0, 0]}]})
        d.call("add_feature", feature={
            "type": "BaseFlange", "name": "Plate",
            "sketch_id": plate_sk["id"]})
        marked = os.path.join(tmp, "marked.dxf")
        res = d.call("export_dxf", path=marked, mark="MH-L")
        with open(marked) as fh:
            dxf = fh.read()
        expect(res["mark"] == "MH-L" and "MARK" in dxf and "MH-L" in dxf,
               "the blank's DXF carries its mark on a MARK layer")
        expect(dxf.count("TEXT") >= 1,
               "as a TEXT entity, which every nesting package reads and no "
               "font is needed for")

        # A blank that does not lie square to the axes is still a blank. The
        # flatness test used to project the AXIS-ALIGNED box onto the face
        # normal, so a plate at an angle measured as thick as it was wide —
        # and a suspension tab welded to an angled tube was refused while the
        # identical tab on a straight tube went out fine.
        d.call("new_project")
        d.call("set_sheet_metal_rules", material="steel", gauge="3mm")
        tilted = d.call("add_feature", feature={
            "type": "Sketch", "name": "Tilted",
            "plane": {"origin": [0, 0, 0], "normal": [0.6, 0.8, 0.0],
                      "uAxis": [0.8, -0.6, 0.0]},
            "entities": [
                {"type": "line", "a": [0, 0], "b": [200, 0]},
                {"type": "line", "a": [200, 0], "b": [200, 120]},
                {"type": "line", "a": [200, 120], "b": [0, 120]},
                {"type": "line", "a": [0, 120], "b": [0, 0]}]})
        d.call("add_feature", feature={
            "type": "BaseFlange", "name": "Tilted",
            "sketch_id": tilted["id"]})
        slanted = os.path.join(tmp, "tilted.dxf")
        d.call("export_dxf", path=slanted, mark="TILT")
        with open(slanted) as fh:
            tdxf = fh.read()
        expect(tdxf.count("LINE") >= 4 and "TILT" in tdxf,
               "a blank at an angle to the axes exports like any other")

        # One STEP per member, the file and the product inside it both named
        # for the member.
        d.call("new_project")
        for i, y in enumerate((0.0, 200.0)):
            cl = d.call("add_feature", feature={
                "type": "Curve3D", "name": f"M{i} CL", "kind": "bent_path",
                "radius": 60.0,
                "points": [[0, y, 0], [400, y, 0]]})
            d.call("add_feature", feature={
                "type": "Pipe", "name": f"Member {i}", "path_id": cl["id"],
                "outer_diameter": 25.4, "wall_thickness": 1.65})
        members_dir = os.path.join(tmp, "members")
        res = d.call("export_members", dir=members_dir)
        names = sorted(f["file"] for f in res["files"])
        expect(res["count"] == 2 and names == ["Member_0.step",
                                               "Member_1.step"],
               "export_members writes one file per member, named for it")
        expect(all(os.path.getsize(os.path.join(members_dir, n)) > 0
                   for n in names),
               "and they are real files")
        try:
            d.call("export_members", dir=members_dir)
            expect(False, "a second export into the same directory refuses")
        except RemoteError as e:
            expect("overwrite" in str(e),
                   "a second export refuses rather than half-replacing an "
                   "order nobody could then check")

        phase("Curve points")
        # A bent path's `points` are the CORNERS and the metal turns SHORT of
        # them, so a caller asking where a member really runs must not have to
        # reconstruct the arcs. On a right angle the metal misses the corner
        # by R*(sqrt(2)-1), which is 63 mm on a 152.4 former — the distance a
        # brace was once attached into thin air across.
        d.call("new_project")
        former = 152.4
        cl = d.call("add_feature", feature={
            "type": "Curve3D", "name": "Hoop CL", "kind": "bent_path",
            "radius": former,
            "points": [[0, 0, 0], [0, 0, 900], [600, 0, 900], [600, 0, 0]]})
        cp = d.call("curve_points", curve=cl["id"])
        pts, at = cp["points"], cp["at_mm"]
        expect(cp["kind"] == "bent_path" and len(pts) == len(at) > 8,
               "curve_points samples the curve the feature built")
        expect(abs(at[0]) < 1e-9 and abs(at[-1] - cp["length_mm"]) < 1e-9,
               "and carries the arc length of each point from the start")
        legs = (900 - former) + (600 - 2 * former) + (900 - former)
        want_len = legs + 2 * (math.pi / 2.0) * former
        # A micron, not an epsilon: this is an arc length INTEGRATED along a
        # sampled wire, so it lands a few ten-thousandths off the closed form
        # over two metres, and a tolerance tighter than the measurement is a
        # test that fails for being right.
        expect(abs(cp["length_mm"] - want_len) < 1e-3,
               f"length {cp['length_mm']:.4f} is three legs and two arcs "
               f"({want_len:.4f})")
        corner = [0.0, 0.0, 900.0]
        near = min(math.dist(p, corner) for p in pts)
        want_gap = former * (math.sqrt(2.0) - 1.0)
        expect(abs(near - want_gap) < 1.0,
               f"the metal misses the corner by {near:.1f} mm, not 0 — which "
               f"is R*(sqrt(2)-1) = {want_gap:.1f}")

        phase("Shell")
        # Shell (M8): hollow a box into a bin via list_elements face name
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (10, 10, 10), "Blk")
        bels = d.call("list_elements")["bodies"][0]
        d.call("add_feature", feature={
            "type": "Shell", "name": "Bin", "body_id": bels["created_by"],
            "thickness": 1.0,
            "open_faces": [{"feature_id": bels["created_by"],
                            "element": bels["faces"][0]["name"]}]})
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "shell regenerated clean")
        expect(abs(st["bodies"][0]["volume"] - 424.0) < 5.0, "bin volume ~424")

        phase("Export STEP/STL/BREP")
        # Export STEP/STL/BREP (E1), then re-import the STEP to verify it
        # round-trips to the same volume.
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (2, 3, 4), "Blk")
        step = os.path.join(tmp, "part.step")
        stl = os.path.join(tmp, "part.stl")
        brep = os.path.join(tmp, "part.brep")
        expect(d.call("export_step", path=step)["bodies"] == 1, "export step")
        stl_rep = d.call("export_stl", path=stl, deflection=0.05)
        expect(stl_rep["watertight"] is True and stl_rep["shells"] == 1 and
               stl_rep["free_edges"] == 0,
               f"exported solid reports watertight: {stl_rep}")
        d.call("export_brep", path=brep)
        for f in (step, stl, brep):
            expect(os.path.getsize(f) > 100, f"{f} written")
        # STL binary header is 80 bytes + 4-byte count; not ASCII "solid".
        with open(stl, "rb") as fh:
            expect(not fh.read(5).startswith(b"solid") or
                   os.path.getsize(stl) > 84, "stl is binary")
        d.call("new_project")
        rep = d.call("import_step", path=step)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 24.0) < 1e-3,
               "re-imported STEP has the original volume")
        # Centred on import, and the reply says by how much — as add_feature
        # does for an Import. The box is 2x3x4 from the origin, so its centre
        # moved by half of that.
        shift = rep.get("recentred_by")
        expect(shift is not None and
               abs(sum(c * c for c in shift) ** 0.5 -
                   (1.0 + 2.25 + 4.0) ** 0.5) < 1e-3,
               f"import_step reports recentred_by ({rep})")
        d.call("new_project")
        kept = d.call("import_step", path=step, keep_origin=True)
        expect("recentred_by" not in kept,
               "keep_origin imports report no shift")
        # STL import: the watertight mesh comes back as a closed SOLID (the
        # sewn shell is promoted), so volume works; a box tessellates exactly.
        d.call("new_project")
        d.call("import_step", path=stl)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 24.0) < 1e-3,
               "re-imported STL is a solid with the original volume")
        # An STL carries no unit, so the caller says what it was drawn in
        # (docs/interchange/README.md#importing-into-a-part): read as cm, the 2x3x4 box is 10x per side.
        d.call("new_project")
        d.call("import_step", path=stl, units="cm")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 24000.0) < 0.1,
               "an STL read in cm is 10x per side")
        expect_error(d, "import_step", "Accepted", path=stl, units="furlong")
        expect_error(d, "import_step", "declares its own", path=step,
                     units="in")
        expect_error(d, "add_feature", "Accepted", feature={
            "type": "Import", "name": "Bad", "path": stl, "units": "furlong"})
        # The default said out loud is not an unknown field, though it does
        # not serialize back (absent = mm).
        d.call("add_feature", feature={
            "type": "Import", "name": "Explicit", "path": stl, "units": "mm",
            "keep_origin": False})

        # `keep_origin` through the RPC, which is where a bought part needs
        # it. Without it the import is CENTRED on its bounding box, so the
        # placement a part was exported at is lost and every one of them
        # lands on the origin — 47 clashing pairs, on a cabinet whose four
        # bought assemblies all stacked in the top corner.
        d.call("new_project")
        shapes.box(d, (400, 0, 0), (10, 10, 10), "Away")
        placed = os.path.join(tmp, "placed.step")
        d.call("export_step", path=placed)
        for keep, want in ((False, 0.0), (True, 405.0)):
            d.call("new_project")
            args = {"path": placed}
            if keep:
                args["keep_origin"] = True
            d.call("import_step", **args)
            box = d.call("get_state")["bodies"][0]
            mid = (box["bbox_min"][0] + box["bbox_max"][0]) / 2.0
            expect(abs(mid - want) < 0.01,
                   f"keep_origin={keep} put the box at x={mid:.2f}, "
                   f"expected {want}")

        phase("Assembly import")
        # Assembly import (I2) into an UNSAVED project: it ADDS to the
        # current project (hand-modeled and imported parts mix); the .brep
        # sidecars stage under the app state dir until the next save adopts
        # them into <project>/imported/.
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (1, 1, 1), "Mine")
        rep = d.call("import_step", path=step, as_assembly=True)
        expect(len(rep["occurrences"]) == 1,
               "assembly import works without a saved project")
        # The response says whether the parts it created actually built.
        # Importing the SO-101 arm left 12 of 97 tabs broken and reported
        # success, and the only way to find out was to open each tab.
        expect(rep["ok"] and "failed" not in rep,
               f"a clean import says so: {rep}")
        expect(rep["parts"] == 1, f"parts it created: {rep}")
        expect(not any(o["name"].startswith("=>")
                       for o in rep["occurrences"]),
               f"XCAF pointer markers are not names: {rep['occurrences']}")
        parts = d.call("list_parts")["parts"]
        expect(len(parts) == 3 and
               [p["type"] for p in parts].count("assembly") == 1,
               f"own part + imported part + assembly tab: {parts}")
        proj = os.path.join(tmp, "miximport.dsgn")
        d.call("save_project", path=proj)
        adopted = glob.glob(os.path.join(proj, "imported", "**", "*.brep"),
                            recursive=True)
        expect(len(adopted) == 1,
               f"staged sidecar adopted into the project on save: {adopted}")
        d.call("open_project", path=proj)
        parts = d.call("list_parts")["parts"]
        # By its import folder, not its name: the STEP carries the name of
        # the tab it was exported from ("Part 1", like ours) since X4.
        imported = [p for p in parts
                    if p["type"] == "part" and p.get("folder") == "part"]
        expect(len(imported) == 1, f"imported part reloads: {parts}")
        d.call("set_active_part", index=imported[0]["index"])
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 24.0) < 1e-3,
               "adopted sidecar regenerates with the original volume")
        d.call("new_project")  # empty document
        try:
            d.call("export_step", path=os.path.join(tmp, "empty.step"))
            expect(False, "export with no bodies must fail")
        except RuntimeError:
            pass

        phase("Checkpoints")
        # Checkpoints (F4): create, list, mutate, restore, and error paths
        d.call("new_project")  # fresh document: no project path yet
        shapes.box(d, (0, 0, 0), (2, 2, 2), "Seed")
        try:
            d.call("checkpoint_create", name="nope")
            expect(False, "checkpoint without a saved project must fail")
        except RuntimeError as e:
            expect("save the project" in str(e), "checkpoint needs a project")
        cp_project = os.path.join(tmp, "cp.json")
        d.call("save_project", path=cp_project)
        d.call("checkpoint_create", name="v1")
        expect(os.path.exists(cp_project + ".checkpoints/v1.json"),
               "checkpoint file written beside the project")
        volume_before = d.call("get_state")["bodies"][0]["volume"]
        shapes.box(d, (90, 0, 0), (1, 1, 1), "Extra")
        expect(len(d.call("get_state")["bodies"]) == 2, "mutated after cp")
        cps = d.call("checkpoint_list")["checkpoints"]
        expect([c["name"] for c in cps] == ["v1"], "checkpoint listed")
        d.call("checkpoint_restore", name="v1")
        st = d.call("get_state")
        expect(len(st["bodies"]) == 1
               and abs(st["bodies"][0]["volume"] - volume_before) < 1e-9,
               "restore returns the checkpointed state")
        try:
            d.call("checkpoint_restore", name="missing")
            expect(False, "restoring a missing checkpoint must fail")
        except RuntimeError:
            pass

        phase("Directory format + git history")
        # Directory format + git history (F3+F5): saves commit, checkpoints
        # tag, restore rewinds
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (2, 2, 2), "Base")
        proj = os.path.join(tmp, "gitproj.dsgn")
        d.call("save_project", path=proj, message="first")
        expect(os.path.isdir(proj) and
               os.path.exists(os.path.join(proj, "index.json")),
               "directory project written")
        expect(os.path.isdir(os.path.join(proj, ".git")),
               "save initialized a git repo")
        shapes.box(d, (5, 0, 0), (1, 1, 1), "More")
        d.call("save_project", path=proj, message="second")
        log = d.call("history_log")["log"]
        expect([e["subject"] for e in log] == ["second", "first"],
               "one commit per save, newest first")
        d.call("checkpoint_create", name="beta")
        shapes.box(d, (10, 0, 0), (1, 1, 1), "Even more")
        d.call("save_project", path=proj, message="third")
        d.call("history_restore", commit=log[-1]["hash"])  # back to "first"
        # A box is two features: its Sketch and its Extrude.
        expect(len(d.call("get_state")["features"]) == 2,
               "restored the first commit's single box")
        d.call("history_restore", commit="cp/beta")  # checkpoint tag works
        expect(len(d.call("get_state")["features"]) == 4,
               "restored the checkpoint tag (two boxes)")
        try:
            d.call("history_restore", commit="nonsense")
            expect(False, "unknown commit must fail")
        except RuntimeError:
            pass

        phase("Versions cover the whole project")
        # F6: a named version is the WHOLE project. The F4 checkpoint it
        # replaced snapshotted `state.document` — the ACTIVE PART — so a part
        # that exists in the version but not now could never come back, and
        # on a multi-part project the other tabs were simply not in the file.
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (4, 4, 4), "Base")
        d.call("add_part", name="Lid")
        shapes.box(d, (0, 0, 0), (4, 1, 4), "Plate")
        multi = os.path.join(tmp, "multipart.dsgn")
        d.call("save_project", path=multi)
        d.call("checkpoint_create", name="both parts")
        expect([c["name"] for c in d.call("checkpoint_list")["checkpoints"]]
               == ["both parts"], "named version listed")

        # Delete a whole part AND edit the survivor, then go back.
        d.call("set_active_part", index=0)
        shapes.box(d, (20, 0, 0), (1, 1, 1), "Wart")
        d.call("delete_part", index=1)
        d.call("save_project", path=multi)
        expect(len(d.call("list_parts")["parts"]) == 1, "part deleted")

        d.call("checkpoint_restore", name="both parts")
        names = [pp["name"] for pp in d.call("list_parts")["parts"]]
        expect(names == ["Part 1", "Lid"],
               f"the deleted part came back: {names}")
        expect(len(d.call("get_state")["features"]) == 2,
               "and the survivor's later edit is gone (one box)")
        # Linear history: the state we left is still reachable.
        expect(len(d.call("history_log")["log"]) >= 4,
               "the restore is itself a version")

        phase("Machines over the RPC")
        # Skipped in a build without CAM (`cmake -DENABLE_CAM=OFF`), where these
        # methods do not exist at all. Gated on what `ping` says this BUILD
        # contains rather than on catching "unknown method", which would equally
        # swallow a genuine typo or a method dropped by accident.
        if not has_cam:
            expect_error(d, "list_machines", "unknown method")
            # The phase below opens with a reset, and what follows it counts
            # tabs; leave the same state either way.
            d.call("new_project")
        else:
            # The store is global (a scratch XDG_CONFIG_HOME here) and a setup
            # EMBEDS a copy of the machine it names; the RPC says how the two
            # stand (docs/cam/setup.md#machine-profiles).
            for stale in ("Smoke router",):
                try:
                    d.call("delete_machine", name=stale)
                except RuntimeError:
                    pass
            before = len(d.call("list_machines")["machines"])
            saved = d.call("save_machine", machine={
                "name": "Smoke router", "post": "mach3",
                "travel": {"min": [0, 0, -90], "max": [600, 400, 0]},
                "rapid_feed": 2500})
            expect(saved["name"] == "Smoke router", f"save_machine echoes: {saved}")
            machines = d.call("list_machines")["machines"]
            expect(len(machines) == before + 1, "the store grew by one")
            expect_error(d, "save_machine", "name",
                         machine={"name": "", "post": "mach3"})
            expect_error(d, "save_machine", "haas",
                         machine={"name": "Haas", "post": "haas"})
            d.call("new_project")
            shapes.box(d, (0, 0, 0), (40, 30, 20), "Plate")
            d.call("machine_part")
            on = d.call("add_cam_setup", name="On the router", machine="Smoke router")
            expect(on["machine"]["name"] == "Smoke router" and
                   on["machine_match"] == "global",
                   f"a setup on a named machine is `global`: {on}")
            n_before = len(d.call("list_cam_setups")["setups"])
            expect_error(d, "add_cam_setup", "Nobody's", name="x",
                         machine="Nobody's")
            expect(len(d.call("list_cam_setups")["setups"]) == n_before,
                   "a refused add_cam_setup adds nothing")
            # Editing the copy makes it differ; the store did not change.
            d.call("set_cam_setup", id=on["id"], fit_arcs=False)
            st = [s_ for s_ in d.call("list_cam_setups")["setups"]
                  if s_["id"] == on["id"]][0]
            expect(st["machine_match"] == "differs" and not st["fit_arcs"],
                   f"an edited copy reports `differs`: {st['machine_match']}")
            d.call("set_cam_setup", id=on["id"], machine="Smoke router")
            st = [s_ for s_ in d.call("list_cam_setups")["setups"]
                  if s_["id"] == on["id"]][0]
            expect(st["machine_match"] == "global" and st["fit_arcs"],
                   "re-picking the machine restores the copy")
            # A rotary index needs a machine with the axis. `SetupToModel`
            # applies rotationA whatever the machine says while `EmitsRotary`
            # needs the flag before it posts an A word -- so an index accepted
            # here would machine the part AS IF turned and write a file that
            # never turns it: plausible G-code, wrong part, no message. The
            # GUI cannot reach that state and a loaded job cannot either; this
            # door was the one left open.
            expect_error(d, "set_cam_setup", "needs a machine with a 4th axis",
                         id=on["id"], rotation_a=90.0)
            st = [s_ for s_ in d.call("list_cam_setups")["setups"]
                  if s_["id"] == on["id"]][0]
            expect(abs(st["rotation_a"]) < 1e-9,
                   "a refused index leaves the setup unturned")
            # ...and with the axis it goes in.
            d.call("set_cam_setup", id=on["id"], has_rotary_axis=True,
                   rotation_a=90.0)
            st = [s_ for s_ in d.call("list_cam_setups")["setups"]
                  if s_["id"] == on["id"]][0]
            expect(abs(st["rotation_a"] - 90.0) < 1e-9 and
                   st["has_rotary_axis"],
                   "an index on a 4th-axis machine is accepted")
            d.call("set_cam_setup", id=on["id"], rotation_a=0.0,
                   has_rotary_axis=False)
            d.call("delete_machine", name="Smoke router")
            st = [s_ for s_ in d.call("list_cam_setups")["setups"]
                  if s_["id"] == on["id"]][0]
            expect(st["machine_match"] == "missing" and
                   st["machine"]["name"] == "Smoke router",
                   f"a deleted machine leaves the job its copy: {st['machine_match']}")
            d.call("new_project")

        phase("Job undo over the RPC")
        # Skipped in a build without CAM (`cmake -DENABLE_CAM=OFF`), where these
        # methods do not exist at all. Gated on what `ping` says this BUILD
        # contains rather than on catching "unknown method", which would equally
        # swallow a genuine typo or a method dropped by accident.
        if not has_cam:
            expect_error(d, "machine_part", "unknown method")
            # The phase below opens with a reset, and what follows it counts
            # tabs; leave the same state either way.
            d.call("new_project")
        else:
            # One undo step per RPC that changes the job (router-round.md R7).
            d.call("new_project")
            shapes.box(d, (0, 0, 0), (40, 30, 20), "Plate")
            d.call("machine_part")
            undo_setup = d.call("add_cam_setup", name="Undo")["id"]
            d.call("add_cam_op", setup=undo_setup, type="profile",
                   top_z=21.0, bottom_z=0.0, stepdown=8.0)
            expect(len(d.call("list_cam_setups")["setups"][0]["operations"]) == 1,
                   "the op is there")
            d.call("undo")
            expect(len(d.call("list_cam_setups")["setups"][0]["operations"]) == 0,
                   "undo took the op back out")
            d.call("redo")
            expect(len(d.call("list_cam_setups")["setups"][0]["operations"]) == 1,
                   "redo put it back")
            d.call("undo")
            d.call("undo")
            expect(len(d.call("list_cam_setups")["setups"]) == 0,
                   "and the setup before it")
            d.call("new_project")

        phase("Laying a face up in a job of several parts")
        # Every part's first feature is id 1, so in a job of two a feature id
        # names a face on each: lay_face_up refuses to guess, and takes `part`.
        if has_cam:
            d.call("new_project")
            first = shapes.box(d, (0, 0, 0), (40, 30, 20), "A")
            d.call("add_part", name="B")
            second = shapes.box(d, (0, 0, 0), (30, 30, 10), "B")
            expect(first["id"] == second["id"],
                   f"both parts' first feature share an id: {first} {second}")
            d.call("machine_part", part="Part 1")
            jp = d.call("add_job_part", part="B")["id"]
            sid = d.call("add_cam_setup", name="Both")["id"]
            # The top cap: an Extrude names it `<id>/cap1` in BOTH parts
            # (a side face is named by its sketch entity, which differs).
            face = next(f["name"] for f in
                        d.call("list_elements", part="B")["bodies"][0]["faces"]
                        if f["normal"][2] > 0.999)
            expect(face.endswith("/cap1"), f"the top cap: {face}")
            pick = {"feature_id": second["id"], "element": face}
            expect_error(d, "set_cam_setup", "say which", id=sid,
                         lay_face_up=pick)
            expect_error(d, "set_cam_setup", "no job part", id=sid,
                         lay_face_up=dict(pick, part=9999))
            d.call("set_cam_setup", id=sid, lay_face_up=dict(pick, part=jp))
            d.call("new_project")

        phase("Workholding over the RPC")
        # docs/cam/setup.md#workholding-fixtures: a clamp is a keep-out the verifier
        # sees -- a keep-out box, or any part the job does not cut.
        if has_cam:
            d.call("new_project")
            shapes.box(d, (0, 0, 0), (40, 30, 20), "Plate")
            d.call("add_part", name="Clamp")
            shapes.box(d, (0, 0, 0), (8, 10, 30), "Jaw")
            d.call("machine_part", part="Part 1")
            sid = d.call("add_cam_setup", name="Clamped")["id"]
            d.call("add_cam_op", setup=sid, type="profile", top_z=21.0,
                   bottom_z=0.0, stepdown=8.0)
            box = d.call("add_job_fixture", setup=sid, box=[10, 30, 25])
            # The first takes -X, flat on the bed: the stock runs -2..42 in X
            # and -1..21 in Z, so the box spans X -12..-2 and stands to 24.
            expect(abs(box["pos"][0] + 12.0) < 1e-6 and
                   abs(box["pos"][2] + 1.0) < 1e-6,
                   f"a box goes against -X, on the bed: {box}")
            expect(box["safe_z"] > 24.0,
                   f"safe Z clears a fixture taller than the stock: {box}")
            jaw = d.call("add_job_fixture", setup=sid, part="Clamp")
            expect(jaw["part"] > 0 and jaw["name"] == "Clamp",
                   f"a part is a fixture too: {jaw}")
            expect_error(d, "add_job_fixture", "CUTS", setup=sid,
                         part="Part 1")
            expect_error(d, "add_job_fixture", "exactly one", setup=sid,
                         part="Clamp", box=[1, 1, 1])
            expect_error(d, "add_job_fixture", "centre", setup=sid,
                         box=[1, 1, 1], side="left")
            expect_error(d, "set_job_fixture", "pass one", setup=sid,
                         fixture=jaw["id"], side="top", pos=[0, 0, 0])
            moved = d.call("set_job_fixture", setup=sid, fixture=jaw["id"],
                           side="top")
            expect(abs(moved["pos"][2] - 21.0) < 1e-6,
                   f"`top` lays it on the stock: {moved}")
            # A seat RESTS it on the stock by one of its faces
            # (docs/cam/setup.md#seats): the jaw's underside on the
            # top, 6 mm in over the +Y edge, 10 mm along from -X.
            jels = d.call("list_elements", part="Clamp")["bodies"][0]
            under = next(f["name"] for f in jels["faces"]
                         if f["normal"][2] < -0.999)
            top = next(f["name"] for f in jels["faces"]
                       if f["normal"][2] > 0.999)
            seat = {"feature_id": jels["created_by"], "element": under,
                    "edge": "+y", "along_mm": 10.0, "over_mm": 6.0}
            expect_error(d, "set_job_fixture", "without pos", setup=sid,
                         fixture=jaw["id"], seat=seat, pos=[0, 0, 0])
            expect_error(d, "set_job_fixture", "does not face down",
                         setup=sid, fixture=jaw["id"],
                         seat=dict(seat, element=top))
            expect_error(d, "set_job_fixture", "-x, +x, -y or +y",
                         setup=sid, fixture=jaw["id"],
                         seat=dict(seat, edge="left"))
            expect_error(d, "set_job_fixture", "no faces", setup=sid,
                         fixture=box["id"], seat=seat)
            seated = d.call("set_job_fixture", setup=sid, fixture=jaw["id"],
                            seat=seat)
            expect(seated.get("seat", {}).get("edge") == "+y" and
                   abs(seated["pos"][2] - 21.0) < 1e-6,
                   f"the seat rests its underside on the stock top: {seated}")
            expect_error(d, "set_job_fixture", "rests on the stock",
                         setup=sid, fixture=jaw["id"], side="top")
            unseated = d.call("set_job_fixture", setup=sid,
                              fixture=jaw["id"], seat=None)
            expect("seat" not in unseated and
                   unseated["pos"] == seated["pos"],
                   f"seat: null stands it where it was: {unseated}")
            moved = d.call("set_job_fixture", setup=sid, fixture=jaw["id"],
                           side="top")
            listed = d.call("list_cam_setups")["setups"][0]["fixtures"]
            expect(len(listed) == 2 and "box" in listed[0] and
                   "part" in listed[1], f"the setup lists both: {listed}")
            # The profile's cutter runs round the plate at X -3: into the box
            # on the bed, and under the jaw lying across the -X edge -- the
            # deeper of the two, so the one reported worst.
            v = d.call("cam_verify", setup=sid)
            expect(v["fixture_collisions"] > 0 and v["fixtures_checked"] == 2
                   and v["worst_fixture"] == "Clamp" and not v["ok"],
                   f"verify reports the cutter in the clamps: "
                   f"{ {k: v[k] for k in v if 'fixture' in k} }")
            expect(any('fixture "Clamp"' in line for line in v["says"]),
                   "and says so in words")
            d.call("set_job_fixture", setup=sid, fixture=box["id"],
                   enabled=False)
            v = d.call("cam_verify", setup=sid)
            expect(v["fixtures_checked"] == 1,
                   f"a fixture switched off is drawn, not checked: "
                   f"{ {k: v[k] for k in v if 'fixture' in k} }")
            d.call("remove_job_fixture", setup=sid, fixture=box["id"])
            expect(len(d.call("list_cam_setups")["setups"][0]["fixtures"]) == 1,
                   "remove takes it off")
            d.call("undo")
            expect(len(d.call("list_cam_setups")["setups"][0]["fixtures"]) == 2,
                   "and undo puts it back")
            d.call("new_project")

        phase("A job is addressed by name, not by whichever tab is in front")
        if not has_cam:
            expect_error(d, "list_cam_setups", "unknown method")
            d.call("new_project")
        else:
            # Every job verb used to act on the ACTIVE job document. That is
            # the defect `check_drawing` had — it judged whichever sheet was in
            # front and reported "ok" — and here it is worse: a `cam_post`
            # after an incidental tab switch writes ANOTHER job's G-code to the
            # path you named.
            d.call("new_project")
            for name in ("Plate A", "Plate B"):
                d.call("add_part", name=name)
                shapes.box(d, (0, 0, 0), (40, 30, 20), name)
                d.call("machine_part", name=f"Job {name[-1]}")
            # Job B is the tab in front; every call below names Job A.
            a = d.call("add_cam_setup", job="Job A", name="A's setup")["id"]
            d.call("add_cam_op", job="Job A", setup=a, type="profile",
                   top_z=20.0, bottom_z=0.0, stepdown=8.0)
            mine = d.call("list_cam_setups", job="Job A")["setups"]
            expect(len(mine) == 1 and mine[0]["id"] == a and
                   len(mine[0]["operations"]) == 1,
                   f"the named job is the one that answers: {mine}")
            expect(len(d.call("list_cam_setups", job="Job B")["setups"]) == 0,
                   "and the other job is untouched")
            paths = d.call("cam_toolpath", job="Job A", setup=a)
            expect(len(paths["operations"]) == 1,
                   f"a toolpath is generated for the job named: {paths}")
            # An unknown name is refused naming the jobs that exist, not
            # silently answered by the active one.
            expect_error(d, "list_cam_setups", "Job C", job="Job C")
            expect_error(d, "list_cam_setups", "Job A", job="Job C")
            d.call("new_project")

        phase("CAM over the RPC")
        # Skipped in a build without CAM (`cmake -DENABLE_CAM=OFF`), where these
        # methods do not exist at all. Gated on what `ping` says this BUILD
        # contains rather than on catching "unknown method", which would equally
        # swallow a genuine typo or a method dropped by accident.
        if not has_cam:
            expect_error(d, "add_cam_setup", "unknown method")
            # The phase below opens with a reset, and what follows it counts
            # tabs; leave the same state either way.
            d.call("new_project")
        else:
            # Every operation type must survive the round trip through the RPC.
            # OpType::Adaptive was REPORTED as "profile" for a while — the way in
            # rejected an unknown name loudly, but the way out had a `default:`,
            # so a script that wrote an adaptive op read back a profile and had no
            # way to know.
            d.call("new_project")
            shapes.box(d, (0, 0, 0), (40, 30, 20), "Plate")
            # A setup lives in a JOB document since v73, so machining a part is
            # two steps: make the job, then add setups on it.
            d.call("machine_part")
            cam_setup = d.call("add_cam_setup", name="Job")["id"]
            for kind in ("profile", "pocket", "face", "drill", "adaptive"):
                op = d.call("add_cam_op", setup=cam_setup, type=kind,
                            top_z=20.0, bottom_z=10.0)["id"]
                back = [o for s_ in d.call("list_cam_setups")["setups"]
                        for o in s_["operations"] if o["id"] == op]
                expect(len(back) == 1, f"{kind} op came back")
                expect(back[0]["type"] == kind,
                       f"{kind} op read back as {back[0]['type']}")
                d.call("remove_cam_op", id=op)
            # An ELLIPTICAL pocket. The vendored Adaptive2d throws on particular
            # vertex layouts — not particular shapes — and every ellipse OCC
            # discretises at the 0.02 mm chord tolerance lands in the failing set,
            # so from the GUI it looked ellipse-specific: three clicks on "Clear a
            # pocket" and the face never changed. The pocket still has to be
            # cleared, so adaptive falls back to the concentric strategy and SAYS
            # which one cut it (docs/cam/operations.md#pocket-and-adaptive).
            d.call("new_project")
            shapes.box(d, (-100, -60, 0), (200, 120, 20), "Plate")
            d.call("add_feature", feature={
                "type": "Sketch", "name": "Pocket sketch",
                "plane": {"origin": [0, 0, 20], "normal": [0, 0, 1],
                          "uAxis": [1, 0, 0]},
                "entities": [{"type": "ellipse", "center": [0, 0],
                              "r_major": 61.224365234375,
                              "r_minor": 29.614774703979492,
                              "rotation": -0.3941636085510254}]})
            sk = [f for f in d.call("get_state")["features"]
                  if f["type"] == "Sketch"][-1]["id"]
            d.call("add_feature", feature={
                "type": "Extrude", "name": "Pocket", "sketch_id": sk,
                "distance": 11.0, "mode": "cut", "reversed": True})
            # Faces are picked on the PART, before the job tab takes over.
            floor = None
            for body in d.call("list_elements")["bodies"]:
                for f in body["faces"]:
                    n = f.get("normal") or [0, 0, 0]
                    c = f.get("centroid") or [0, 0, 0]
                    if f.get("kind") == "plane" and n[2] > 0.99 and abs(c[2] - 9.0) < 0.5:
                        floor = {"feature_id": body["created_by"],
                                 "element": f["name"]}
            expect(floor is not None, "the elliptical pocket has a floor to click")
            d.call("machine_part")
            ell_setup = d.call("add_cam_setup", name="Ellipse")["id"]
            op = d.call("add_cam_op", setup=ell_setup, type="pocket", tool=1,
                        target=floor)
            expect(op["side"] == "inside", f"a pocket clears inside: {op}")
            got = d.call("cam_toolpath", setup=ell_setup)["operations"][0]
            expect(got["moves"] > 0,
                   f"an elliptical pocket is cleared, not skipped: {got}")
            expect("concentric" in got.get("strategy", ""),
                   f"and it says which strategy cut it: {got}")
            # Whatever cut it, it must cut the right thing.
            v = d.call("cam_verify", setup=ell_setup, resolution=0.8)
            expect(v["ok"], f"the fallback cuts cleanly: {v}")
            # The same pick with its depths typed keeps its target. It was
            # read only by the derivation, which typed depths skip: the op
            # came out with no target at all -- the whole part -- and the
            # call answered "unknown parameter 'target'" after making it.
            typed = d.call("add_cam_op", setup=ell_setup, type="pocket",
                           tool=1, target=floor, top_z=op["top_z"],
                           bottom_z=op["bottom_z"])
            expect(len(typed.get("targets", [])) == 1,
                   f"a typed-depth op keeps its one target: {typed}")
            d.call("remove_cam_op", id=typed["id"])
            expect_error(d, "add_cam_op", "not both", setup=ell_setup,
                         type="pocket", tool=1, target=floor,
                         targets=[floor], top_z=1.0, bottom_z=0.0)

            d.call("new_project")
            shapes.box(d, (0, 0, 0), (40, 30, 20), "Plate")
            d.call("machine_part")
            cam_setup = d.call("add_cam_setup", name="Job")["id"]
            # Arc fitting is a property of the machine, so it persists and the
            # posted file honours it (C7).
            d.call("add_cam_op", setup=cam_setup, type="profile",
                   top_z=21.0, bottom_z=0.0, stepdown=8.0)
            d.call("set_cam_setup", id=cam_setup, fit_arcs=False)
            expect(d.call("list_cam_setups")["setups"][0]["fit_arcs"] is False,
                   "fit_arcs round-trips")
            nc = os.path.join(tmp, "arcs.nc")
            d.call("cam_post", setup=cam_setup, path=nc)
            plain = open(nc).read()
            expect("G2 " not in plain, "arcs off means no arcs in the file")
            d.call("set_cam_setup", id=cam_setup, fit_arcs=True)
            d.call("cam_post", setup=cam_setup, path=nc)
            curved = open(nc).read()
            expect("G2 " in curved, "arcs on means arcs in the file")
            expect(len(curved) < len(plain), "and a smaller file")
            # One file per tool (router-round.md R2): `path` is the base and the
            # answer names every file written, each a complete program.
            d.call("set_cam_setup", id=cam_setup, split_output=True)
            expect(d.call("list_cam_setups")["setups"][0]["split_output"] is True,
                   "split_output round-trips")
            split = d.call("cam_post", setup=cam_setup, path=nc)
            expect(len(split["files"]) == 1 and
                   split["files"][0]["path"].endswith("arcs-T1.nc"),
                   f"a one-tool job splits into one named file: {split['files']}")
            part = open(split["files"][0]["path"]).read()
            expect("G2 " in part and "M30" in part, "the tool's file is complete")
            d.call("set_cam_setup", id=cam_setup, split_output=False)

            # Cutter compensation (docs/cam/post.md#cutter-compensation): by
            # name, read back, an unknown mode refused naming both, and the
            # file says G41/G42 and G40 with the tool's D.
            prof = d.call("list_cam_setups")["setups"][0]["operations"][0]
            d.call("set_cam_op", id=prof["id"], cutter_comp="wear")
            back = d.call("list_cam_setups")["setups"][0]["operations"][0]
            expect(back.get("cutter_comp") == "wear",
                   f"cutter_comp applies and reads back: {back}")
            try:
                d.call("set_cam_op", id=prof["id"], cutter_comp="control")
                raise AssertionError("an unknown cutter_comp must refuse")
            except RuntimeError as e:
                expect("off, wear" in str(e),
                       f"the refusal names the modes there are: {e}")
            d.call("cam_post", setup=cam_setup, path=nc)
            comp = open(nc).read()
            expect(("G41 D1" in comp or "G42 D1" in comp) and "\nG40\n" in comp
                   and "(comp op " in comp,
                   "a compensated profile posts G41/G42 D1 ... G40 and says so")
            ver = d.call("cam_verify", setup=cam_setup)
            expect(ver.get("cutter_comp") == [prof["id"]] and
                   any("compensated in the controller" in s
                       for s in ver.get("says", [])),
                   f"the verdict says the op is compensated: "
                   f"{ver.get('cutter_comp')}")
            # The setup sheet: a PDF, and under --headless no picture, said.
            sheet = os.path.join(tmp, "sheet.pdf")
            made = d.call("export_setup_sheet", setup=cam_setup, path=sheet)
            pdf = open(sheet, "rb").read()
            expect(pdf.startswith(b"%PDF-1.4") and b"%%EOF" in pdf[-16:],
                   "export_setup_sheet writes a PDF")
            expect(made["picture"] is False and
                   any("no renderer" in w for w in made["warnings"]),
                   f"headless, the sheet says it has no picture: {made}")
            expect(made["tools"] == 1 and made["operations"] == 1,
                   f"one tool, one op: {made}")
            try:
                d.call("export_setup_sheet", setup=cam_setup,
                       path=os.path.join(tmp, "sheet.png"))
                raise AssertionError("a setup sheet that is not a .pdf must "
                                     "refuse")
            except RuntimeError as e:
                expect(".pdf" in str(e), f"the refusal says what it writes: {e}")
            d.call("set_cam_op", id=prof["id"], cutter_comp="off")

            # R9 (2026-08-26): a typed top_z below the stock top warns at
            # toolpath time, naming the op and both heights — the verifier's
            # later "rapid through material at line N" names neither.
            cam_ops = d.call("list_cam_setups")["setups"][0]["operations"]
            d.call("set_cam_op", id=cam_ops[0]["id"], top_z=0.5)
            warned = d.call("cam_toolpath", setup=cam_setup)
            expect(any("stock top" in w for w in warned.get("warnings", [])),
                   f"low top_z warns: {warned.get('warnings')}")
            d.call("set_cam_op", id=cam_ops[0]["id"], top_z=cam_ops[0]["top_z"])
            # THAT warning is gone. Not "no warnings at all": this fixture
            # also cuts 20 mm with a 20 mm cutter, which the reach check is
            # right to mention, and an assertion that nothing at all is said
            # is really an assertion that only one thing can ever be said.
            again = d.call("cam_toolpath", setup=cam_setup).get("warnings", [])
            expect(not any("stock top" in w for w in again),
                   f"restored top_z clears that warning: {again}")
            # ...but not against the UNTOUCHED top when a face op runs first:
            # it has already taken the top down to its own bottom, and an op
            # starting there has nothing to rapid through.
            face = d.call("add_cam_op", setup=cam_setup, type="face")
            d.call("move_cam_op", id=face["id"], to=0)
            d.call("set_cam_op", id=cam_ops[0]["id"], top_z=face["bottom_z"])
            skimmed = d.call("cam_toolpath", setup=cam_setup).get("warnings", [])
            expect(not any("stock top" in w for w in skimmed),
                   f"no low-top warning after a skim: {skimmed}")
            # An operation that produces nothing is named by the verdict,
            # not just by an empty file that verifies "ok".
            broken = d.call("add_cam_op", setup=cam_setup, type="profile",
                            side="inside", top_z=1.0, bottom_z=0.0)
            ver = d.call("cam_verify", setup=cam_setup)
            expect(any(f["op"] == broken["id"] for f in ver["failed_ops"]),
                   f"a failed op is named: {ver['failed_ops']}")
            expect(any("produced no toolpath" in l for l in ver["says"]),
                   f"and said: {ver['says'][:2]}")
            d.call("remove_cam_op", id=broken["id"])
            d.call("remove_cam_op", id=face["id"])
            low = d.call("cam_toolpath", setup=cam_setup).get("warnings", [])
            expect(any("stock top" in w for w in low),
                   f"and the same top_z warns without it: {low}")
            d.call("set_cam_op", id=cam_ops[0]["id"], top_z=cam_ops[0]["top_z"])
            # A tool with no `number` is REFUSED. It used to default to 1 and
            # silently edit the first tool -- rename it, resize it, change
            # its type -- and report success, which is how a second cutter
            # asked for as tool 2 became a rename of tool 1 with nothing said
            # (docs/cam/README.md#invariants).
            try:
                d.call("set_cam_setup", id=cam_setup,
                       tool={"name": "no number here"})
                raise AssertionError("a numberless tool must refuse")
            except RuntimeError as e:
                expect("needs a `number`" in str(e) and "SIBLING" in str(e),
                       f"the refusal names the field and the call shape: {e}")

            # The stock reads back what was set, not just the box it derives.
            d.call("set_cam_setup", id=cam_setup,
                   stock={"mode": "relative", "margin": [4.0, 4.0, 1.0],
                          "bottom_margin": 0.5})
            stk = d.call("list_cam_setups")["setups"][0]["stock"]
            expect(stk["mode"] == "relative" and stk["margin"] == [4.0, 4.0, 1.0]
                   and abs(stk["bottom_margin"] - 0.5) < 1e-9,
                   f"the stock request reads back: {stk}")
            expect("box" in stk and len(stk["box"]["max"]) == 3,
                   f"and so does the box it resolves to: {stk.get('box')}")

            # A field the op type does not have is REFUSED, naming what it
            # does take. Every one of these used to be accepted for every
            # type and then never read: a drill carried tabs, a face carried
            # a peck, and `get_state` reported them like settings that meant
            # something.
            # This op is a PROFILE, which reads no stepover and no edge_size.
            for bad, val in (("edge_size", 1.0), ("stepover", 0.3)):
                try:
                    d.call("set_cam_op", id=cam_ops[0]["id"], **{bad: val})
                    raise AssertionError(f"{bad} on a profile must refuse")
                except RuntimeError as e:
                    expect(f"has no {bad}" in str(e) and "it takes" in str(e),
                           f"the refusal names the field and what it does take: {e}")
            # ...and a field it DOES have applies and reads back, so the
            # response shape is exactly what set_cam_op accepts.
            d.call("set_cam_op", id=cam_ops[0]["id"], lead_arcs=True)
            back = d.call("list_cam_setups")["setups"][0]["operations"][0]
            expect(back.get("lead_arcs") is True,
                   f"a profile's lead_arcs applies and reads back: {back}")
            expect("stepover" not in back and "edge_size" not in back,
                   f"and it reports no field it does not read: {back}")
            d.call("set_cam_op", id=cam_ops[0]["id"], lead_arcs=False)

            # Reach, where it is MADE (docs/cam/setup.md#reach): a shank wider
            # than the cutter, below the flutes, is driven into the wall, so
            # the edit that would ask for it is refused -- the tool's or the
            # op's -- and leaves things as they were. Short of that, the edit
            # goes through and says so in its OWN response.
            op0 = cam_ops[0]
            tools = d.call("list_cam_setups")["setups"][0]["tools"]
            t0 = next(t for t in tools if t["number"] == op0["tool"])
            depth = op0["top_z"] - op0["bottom_z"]
            wide = t0["diameter"] * 2.0
            try:
                d.call("set_cam_setup", id=cam_setup,
                       tool={"number": t0["number"],
                             "flute_length": depth - 2.0,
                             "shank_diameter": wide})
                raise AssertionError("a tool edit that buries its shank "
                                     "must refuse")
            except RuntimeError as e:
                expect("driven into it" in str(e) and op0["name"] in str(e),
                       f"the refusal names the op and what happens: {e}")
            kept = next(t for t in d.call("list_cam_setups")["setups"][0]
                        ["tools"] if t["number"] == t0["number"])
            expect(kept["flute_length"] == t0["flute_length"] and
                   kept["shank_diameter"] == 0.0,
                   f"a refused tool edit changes nothing: {kept}")
            # The wide shank alone: this op is cut within the flutes, so it
            # is allowed -- and the reply says how little is to spare.
            said = d.call("set_cam_setup", id=cam_setup,
                          tool={"number": t0["number"],
                                "flute_length": depth,
                                "shank_diameter": wide})
            expect(any("to spare" in w for w in said.get("warnings", [])),
                   f"a tight reach comes back with the edit: {said}")
            try:
                d.call("set_cam_op", id=op0["id"],
                       bottom_z=op0["bottom_z"] - 1.0)
                raise AssertionError("deeper than the flutes on a wide "
                                     "shank must refuse")
            except RuntimeError as e:
                expect("driven into it" in str(e),
                       f"set_cam_op refuses the collision: {e}")
            back = d.call("list_cam_setups")["setups"][0]["operations"][0]
            expect(back["bottom_z"] == op0["bottom_z"],
                   f"and leaves the op as it was: {back['bottom_z']}")
            # A shank the cutter's width only rubs: allowed, and said.
            d.call("set_cam_setup", id=cam_setup,
                   tool={"number": t0["number"],
                         "shank_diameter": t0["diameter"]})
            rubbed = d.call("set_cam_op", id=op0["id"],
                            bottom_z=op0["bottom_z"] - 1.0)
            expect(any("cut by the shank" in w
                       for w in rubbed.get("warnings", [])),
                   f"a rub is allowed and said: {rubbed}")
            d.call("set_cam_op", id=op0["id"], bottom_z=op0["bottom_z"])
            d.call("set_cam_setup", id=cam_setup,
                   tool={"number": t0["number"],
                         "flute_length": t0["flute_length"],
                         "shank_diameter": 0.0})

            # R7: a targetless drill derive teaches, instead of "not a verb".
            try:
                d.call("cam_derive_op", setup=cam_setup, type="drill")
                raise AssertionError("targetless drill derive must refuse")
            except RuntimeError as e:
                expect("pass targets" in str(e), f"drill refusal teaches: {e}")
            # A job is a second document in the project, so the phases below --
            # which count tabs and lean on the single-document .json form -- start
            # from a clean one rather than inheriting this one.
            d.call("new_project")

        phase("A thread milled over the RPC")
        # docs/cam/operations.md#thread-mill: a 3/8-16 UNC-2B tapped hole, the
        # tap drill bored, the thread milled from the Thread on the bore and
        # judged on its own terms by cam_verify.
        if has_cam:
            d.call("new_project")
            sk = d.call("add_feature", feature={
                "type": "Sketch", "name": "Plate outline",
                "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                          "uAxis": [1, 0, 0]},
                "entities": [{"type": "rect", "c1": [-20, -20],
                              "c2": [20, 20]}]})
            d.call("add_feature", feature={
                "type": "Extrude", "name": "Plate", "sketch_id": sk["id"],
                "distance": 10.0, "mode": "new_body"})
            centre = d.call("add_feature", feature={
                "type": "Sketch", "name": "Tripod centre",
                "plane": {"origin": [0, 0, 10], "normal": [0, 0, 1],
                          "uAxis": [1, 0, 0]},
                "entities": [{"type": "point", "p": [0, 0]}]})
            d.call("add_feature", feature={
                "type": "Hole", "name": "Tripod hole",
                "sketch_id": centre["id"], "hole_kind": "simple",
                "diameter": 7.9, "through": True,
                "fit": {"thread": "3/8-16 UNC", "kind": "tapped"}})
            bore = [{"feature_id": b["created_by"], "element": f["name"]}
                    for b in d.call("list_elements")["bodies"]
                    for f in b["faces"] if f.get("role") == "bore"]
            expect(len(bore) == 1, f"the tapped hole has one bore: {bore}")
            d.call("add_feature", feature={
                "type": "Thread", "name": "Tripod thread", "face": bore[0],
                "designation": "3/8-16 UNC-2B"})
            d.call("machine_part")
            ts = d.call("add_cam_setup", name="Tripod")["id"]
            d.call("set_cam_setup", id=ts, stock={
                "mode": "relative", "margin": [5.0, 5.0, 0.0],
                "bottom_margin": 0.0})
            d.call("set_cam_setup", id=ts, tool={
                "number": 1, "name": "6 mm flat", "diameter": 6.0})
            d.call("set_cam_setup", id=ts, tool={
                "number": 2, "name": "6 mm thread mill", "diameter": 6.0,
                "tool_type": "thread_mill"})
            mill = [t for s_ in d.call("list_cam_setups")["setups"]
                    if s_["id"] == ts for t in s_["tools"]
                    if t["number"] == 2][0]
            # The head, derived from the diameter where nobody measured it.
            expect(mill["tool_type"] == "thread_mill", f"a thread mill: {mill}")
            expect(abs(mill["tip_angle"] - 60.0) < 1e-9, f"60 degrees: {mill}")
            expect(abs(mill["tip_flat"] - 0.1) < 1e-9, f"flat d/60: {mill}")
            expect(abs(mill["neck_diameter"] - 3.6) < 1e-9, f"neck: {mill}")
            expect(abs(mill["flute_length"] - 1.4856406) < 1e-6,
                   f"its flute is the head's width: {mill}")
            expect_error(d, "set_cam_setup", "flute_length", id=ts, tool={
                "number": 2, "flute_length": 20.0})
            d.call("add_cam_op", setup=ts, type="drill", tool=1,
                   target=bore[0])
            thread_op = d.call("add_cam_op", setup=ts, type="thread_mill",
                               target=bore[0])
            expect(thread_op["tool"] == 2,
                   f"the setup's thread mill, not tool 1: {thread_op}")
            expect(any("the middle of 2B" in w
                       for w in thread_op.get("warnings", [])),
                   f"it says what it aims at: {thread_op}")
            expect_error(d, "set_cam_op", "depths are its thread's",
                         id=thread_op["id"], top_z=12.0)
            d.call("set_cam_op", id=thread_op["id"], passes=2)
            v = d.call("cam_verify", setup=ts, resolution=0.1)
            threads = v.get("threads", [])
            expect(len(threads) == 1, f"one thread judged: {v.get('says')}")
            t = threads[0]
            expect(t["judged"] and t["in_band"] and t["good"] and t["ok"],
                   f"the thread is right: {t}")
            expect(t["passes"] == 2, f"the passes it was told: {t}")
            expect(abs(t["pitch_measured_mm"] - 25.4 / 16) < 1e-3,
                   f"16 threads an inch: {t}")
            expect(v["ok"] and v["max_gouge_mm"] < 0.15,
                   f"and nothing else is gouged: {v.get('says')}")
            nc = os.path.join(tmp, "tripod.nc")
            d.call("cam_post", setup=ts, path=nc)
            with open(nc) as f:
                text = f.read()
            expect(any(line.startswith("G3") and " Z" in line
                       for line in text.splitlines()),
                   "the helix is posted as helical G3 arcs")
            d.call("new_project")

        phase("Every op field set over the RPC reads back")
        # docs/cam/operations.md#the-capability-table-and-the-field-table. A field on an operation
        # goes through three hand-kept places on this surface -- the owned
        # list, the apply step and the answer -- and a field accepted but not
        # reported (or reported but not applied) is invisible. The fields come
        # from each op's OWN answer, so this grows with the table.
        if has_cam:
            d.call("new_project")
            shapes.box(d, (0, 0, 0), (40, 30, 20), "Block")
            d.call("machine_part")
            sid = d.call("add_cam_setup", name="Every field")["id"]
            d.call("set_cam_setup", id=sid, has_rotary_axis=True, tool={
                "number": 2, "name": "finisher", "diameter": 3.0})
            enums = {"side": ["outside", "inside", "on"],
                     "corner_relief": ["none", "dogbone", "tbone"],
                     "rotary_pattern": ["parallel", "spiral"],
                     "cutter_comp": ["off", "wear"]}
            # Not fields to poke: identity, the type itself, and `tool`, whose
            # change re-derives the stepdown (Retool) by design.
            fixed = {"id", "type", "tool", "targets", "derived"}

            def other(key, value):
                if key in enums:
                    return next(v for v in enums[key] if v != value)
                if key in ("finish_tool", "rest_from_tool"):
                    return 2  # a tool NUMBER: the setup's second cutter
                if isinstance(value, bool):
                    return not value
                if isinstance(value, (int, float)):
                    return round(value * 0.5 + 0.35, 6)
                if isinstance(value, str):
                    return value + " poked"
                if isinstance(value, list):
                    return [0.0, 90.0]  # index_a_deg: a list of angles
                raise AssertionError(f"no poke for {key}={value!r}")

            types = ("profile", "pocket", "face", "drill", "adaptive",
                     "edge_round", "surface_parallel", "surface_waterline",
                     "surface_rotary")
            for t in types:
                op = d.call("add_cam_op", setup=sid, type=t, tool=1,
                            top_z=20.0, bottom_z=5.0)
                want = {k: other(k, v) for k, v in op.items() if k not in fixed}
                want["tabs"] = {"count": 3, "width": 5.0, "height": 1.25}
                try:
                    d.call("set_cam_op", id=op["id"], **want)
                except RuntimeError as e:
                    # Tabs are only reported once there are some, so they are
                    # offered to every type; a type without them refuses by
                    # name, which is the other half of the contract.
                    expect("has no tabs" in str(e), f"{t}: {e}")
                    del want["tabs"]
                    d.call("set_cam_op", id=op["id"], **want)
                back = next(o for s_ in d.call("list_cam_setups")["setups"]
                            for o in s_["operations"] if o["id"] == op["id"])
                for k, v in want.items():
                    got = back.get(k)
                    same = (abs(got - v) < 1e-9
                            if isinstance(v, float) and isinstance(got, (int, float))
                            else got == v)
                    expect(same, f"{t}.{k}: set {v!r}, read back {got!r}")
                # ...and the other direction: every field the type ACCEPTS is
                # reported. The refusal for a field it lacks lists what it
                # takes, which is the owned list's own answer.
                takes = None
                for probe in ("peck", "tabs", "z_step_mm", "cut_angle_deg"):
                    try:
                        d.call("set_cam_op", id=op["id"], **{probe: 1})
                    except RuntimeError as e:
                        m = re.search(r"it takes (.*?)\. Setting", str(e))
                        if m:
                            takes = m.group(1).split(", ")
                            break
                expect(takes is not None, f"{t}: no probe field was refused")
                for k in takes:
                    expect(k in back, f"{t} accepts `{k}` but does not report "
                                      f"it: {sorted(back)}")
            d.call("new_project")

        phase("Tab addressing, duplicate names, the .json refusal")
        # R4/R5/R16 (2026-08-26). Names address tabs, so duplicates refuse;
        # {part: name|id|index} is one resolver across the tab RPCs; and a
        # multi-document project saved to a .json path used to mint a
        # history-less directory NAMED .json — refused now, naming .dsgn.
        first_tab = d.call("list_parts")["parts"][0]["name"]
        d.call("add_part", name="addr-a")
        try:
            d.call("add_part", name="addr-a")
            raise AssertionError("duplicate part name must refuse")
        except RuntimeError as e:
            expect("already exists" in str(e), f"dup refusal says so: {e}")
        try:
            d.call("rename_part", part=first_tab, new_name="addr-a")
            raise AssertionError("rename onto a taken name must refuse")
        except RuntimeError as e:
            expect("already exists" in str(e), f"rename refusal: {e}")
        d.call("rename_part", part=first_tab, new_name=first_tab)  # self: ok
        # Default names skip taken ones (size+1 collided after a deletion).
        tabs = d.call("list_parts")["parts"]
        auto = d.call("add_part")
        expect(auto["name"] not in {t["name"] for t in tabs},
               f"default name is fresh: {auto}")
        # One resolver: {part: <name>} on set_active and delete.
        d.call("set_active_part", part="addr-a")
        expect(d.call("list_parts")["active"] ==
               next(t["index"] for t in d.call("list_parts")["parts"]
                    if t["name"] == "addr-a"), "set_active_part by part=")
        try:
            d.call("set_active_part")
            raise AssertionError("no-arg set_active_part must refuse")
        except RuntimeError as e:
            expect("part:" in str(e) and "index" in str(e),
                   f"refusal names every accepted form: {e}")
        # R16: this project has several documents now.
        try:
            d.call("save_project", path=os.path.join(tmp, "multi.json"))
            raise AssertionError("multi-doc .json save must refuse")
        except RuntimeError as e:
            expect(".dsgn" in str(e), f"refusal names .dsgn: {e}")
        expect(not os.path.exists(os.path.join(tmp, "multi.json")),
               "nothing written on the refused save")
        d.call("delete_part", part=auto["name"])
        d.call("delete_part", part="addr-a")
        d.call("set_active_part", part=first_tab)

        phase("Autosave + crash recovery")
        # Autosave + crash recovery (F2): save clean, dirty the doc, let the
        # 1s autosave fire, SIGKILL (a real crash — no signal handler), then
        # prove a fresh instance sees and recovers the autosave.
        crash_project = os.path.join(tmp, "crashme.json")
        d.call("save_project", path=crash_project)
        saved_features = len(d.call("get_state")["features"])
        expect(not os.path.exists(crash_project + ".autosave"),
               "clean save leaves no sidecar")
        shapes.box(d, (50, 0, 0), (2, 2, 2), "Unsaved work")
        # The app's 1 s autosave timer is driven by its main loop, which a
        # loaded machine starves; 60 s of budget, and the real elapsed time is
        # printed so a creeping regression is visible before it fails.
        if not wait_for_file(crash_project + ".autosave", 60.0,
                             "autosave sidecar"):
            # This tripped for months under parallel load and was written off
            # as a flake; the cause was the autosaver treating a version
            # number as a document identity (fixed 2026-08-19). 60 s is
            # generous for a 1 s timer, so if it trips again the interesting
            # question is what the app was doing — answer it here.
            alive = app.poll() is None
            state = {}
            try:
                state = d.call("get_state")
            except Exception as e:  # noqa: BLE001 - diagnostics only
                state = {"get_state failed": str(e)}
            listing = sorted(os.listdir(os.path.dirname(crash_project)))
            tail = "".join(app.log().splitlines(keepends=True)[-15:])
            expect(False,
                   "autosave sidecar written while dirty\n"
                   f"      app alive: {alive} (exit {app.poll()})\n"
                   f"      features now: {len(state.get('features', []))}"
                   f"  keys: {sorted(state)[:6]}\n"
                   f"      dir: {listing}\n"
                   f"      app log tail:\n{tail}")
        app.send_signal(signal.SIGKILL)
        wait_exit(app, 30, "SIGKILLed app")

        app2 = launch_app(binary, extra_args=["--autosave", "1"], env=env)
        try:
            d2 = Designer.wait_for(app2.port)
            r = d2.call("open_project", path=crash_project)
            expect(r["autosave_available"], "recovery offered after crash")
            expect(len(d2.call("get_state")["features"]) == saved_features,
                   "the saved file itself has the pre-crash features")
            d2.call("open_project", path=r["autosave_path"])
            # The unsaved box: its Sketch and its Extrude.
            expect(len(d2.call("get_state")["features"]) == saved_features + 2,
                   "the autosave carries the unsaved work")
            d2.call("save_project", path=crash_project)
            expect(not os.path.exists(crash_project + ".autosave"),
                   "saving the recovery clears the sidecar")
        finally:
            app2.terminate()
            wait_exit(app2, 30, "recovery instance")

        phase("Clean shutdown on SIGTERM")
        # Clean shutdown on SIGTERM (fresh instance; the first was SIGKILLed)
        app3 = launch_app(binary, env=env)
        Designer.wait_for(app3.port).close()
        app3.send_signal(signal.SIGTERM)
        expect(wait_exit(app3, 30, "SIGTERMed app") == 0, "exit 0 on SIGTERM")

        phase("A taken port must exit non-zero")
        # A taken port must exit non-zero (CI fail-fast), not serve nothing
        blocker = socket.socket()
        blocker.bind(("127.0.0.1", 0))
        blocker.listen(1)
        p = subprocess.run(
            [binary, "--headless", "--remote",
             str(blocker.getsockname()[1])],
            env=env, capture_output=True, timeout=60)
        expect(p.returncode == 1, "taken port exits 1")
        blocker.close()

        print("headless_smoke: all assertions passed")
    finally:
        if app.poll() is None:
            app.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
