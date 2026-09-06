#!/usr/bin/env python3
"""--offscreen: a real Vulkan renderer with no window and no display server.

The claim this suite exists to pin: an agent on a machine with only the
installed binary — no repo, no Xvfb, no X at all — gets renders. So DISPLAY
and WAYLAND_DISPLAY are stripped before launch; if the mode secretly needs a
display server, the boot phase fails here first.

Skips (77) where there is no Vulkan ICD at all (a container, a bare CI
runner): the femgpu precedent — a render queue needs no display, but it does
need a device. Ray-traced assertions additionally soft-skip when the device
has no RT (lavapipe in CI renders raster, not RT).
"""
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch_app  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_remote import Designer  # noqa: E402

_T0 = time.time()
_PHASE = "startup"


def phase(name):
    global _PHASE
    print(f"  [{time.time() - _T0:6.1f}s] {name}", flush=True)
    _PHASE = name


def expect(condition, message):
    if not condition:
        raise AssertionError(f"[phase: {_PHASE}] {message}")


def expect_error(d, method, fragment, **params):
    try:
        d.call(method, **params)
        raise AssertionError(f"{method} must refuse in --offscreen")
    except RuntimeError as e:
        expect(fragment in str(e),
               f"{method} refusal names '{fragment}': {e}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)  # the headline claim: no display server
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_offscreen_")
    env["XDG_STATE_HOME"] = tmp  # a private registry — read back below
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")

    phase("boots with no display")
    try:
        app = launch_app(binary, env=env, headless="offscreen")
    except RuntimeError as e:
        # No ICD is an environment, not a defect. The app names the condition
        # on its own exit path; VkResult is the raw-abort fallback.
        if "no usable Vulkan device" in str(e) or "VkResult" in str(e):
            print(f"SKIP: no usable Vulkan device here ({e})")
            return 77
        raise
    try:
        d = Designer.wait_for(app.port)
        pong = d.call("ping")
        expect(pong["ok"], "ping answers")
        expect(pong["mode"] == "offscreen", f"ping mode is offscreen ({pong})")
        expect(pong["ui_actions"] is False,
               f"no test engine without a window ({pong})")
        expect(pong["port"] == app.port, f"ping names its port ({pong})")

        phase("instance registry announces offscreen")
        entry = os.path.join(tmp, "designer", "instances", f"{app.port}.json")
        expect(os.path.exists(entry), f"registry file exists ({entry})")
        with open(entry) as f:
            info = json.load(f)
        expect(info.get("mode") == "offscreen",
               f"registry mode is offscreen ({info})")

        phase("the document engine is intact")
        r = d.call("add_feature", feature={
            "type": "Box", "name": "Box 1", "position": [0, 0, 0],
            "width": 20, "height": 30, "depth": 40})
        expect(r.get("regen", {}).get("ok"), f"box regenerates ({r})")
        mp = d.call("mass_properties")
        expect(abs(mp["total"]["volume"] - 24000.0) < 1.0,
               f"box volume is 24000 mm^3 ({mp})")
        # A second box forming a step: the concave corner is where shadows
        # and AO visibly land, so the render-option toggle below has pixels
        # to change on ANY device (a lone convex box barely self-shadows).
        r = d.call("add_feature", feature={
            "type": "Box", "name": "Box 2", "position": [20, 0, 0],
            "width": 20, "height": 12, "depth": 40})
        expect(r.get("regen", {}).get("ok"), f"step regenerates ({r})")

        phase("the projection is LAZY: edits nobody looks at cost nothing")
        # The mode draws only when asked, so the document->renderer
        # projection (tessellate + upload EVERY body, which on a 200-body
        # part is seconds) must not run on the tick after an edit. Read the
        # app's own sync log rather than a timing, which would be a flake.
        def syncs():
            return app.log().count("Document synced")

        before = syncs()
        for i in range(5):
            d.call("add_feature", feature={
                "type": "Box", "name": f"Lazy {i}", "position": [0, 0, 60 + i * 30],
                "width": 10, "height": 10, "depth": 10})
        time.sleep(0.5)  # several ticks: an eager loop would have projected
        expect(syncs() == before,
               f"5 edits with nothing looking projected nothing "
               f"({syncs() - before} syncs)")
        # ...and the deferred work still happens, once, when something looks.
        d.request("screenshot", {"path": os.path.join(tmp, "lazy.png")})
        expect(syncs() == before + 1,
               f"the capture projects exactly once ({syncs() - before})")
        # The capture is of the CURRENT document, not the last projected one:
        # every box added above has to be in the count it drew.
        expect(f"{5 + 2} bodies" in app.log().rsplit("Document synced", 1)[-1],
               "the projection the capture forced is the current document")
        for i in reversed(range(2, 7)):  # the five, after Box 1 and Box 2
            d.call("delete_feature", index=i)
        time.sleep(0.2)

        phase("a stale projection reports no render id rather than a wrong one")
        # get_state is the hot poll and does NOT force a projection, so after
        # an edit the volatile bodies[].mesh id would describe superseded
        # geometry. It is omitted instead: absent is loud, wrong is silent.
        d.call("add_feature", feature={
            "type": "Box", "name": "Stale probe", "position": [0, 0, 200],
            "width": 10, "height": 10, "depth": 10})
        bodies = d.call("get_state")["bodies"]
        expect(all("mesh" not in b for b in bodies),
               f"no render ids while the projection is behind ({bodies})")
        d.call("delete_feature", index=2)
        d.request("screenshot", {"path": os.path.join(tmp, "current.png")})
        bodies = d.call("get_state")["bodies"]
        expect(all("mesh" in b for b in bodies),
               f"render ids are back once something has drawn ({bodies})")

        phase("GUI-driving refusals survive, naming the mode")
        expect_error(d, "menu_click", "offscreen", path="Edit/Undo")
        expect_error(d, "key_press", "offscreen", chord="Ctrl+Z")
        expect_error(d, "viewport_drag", "offscreen")
        expect_error(d, "frame_stats", "offscreen")

        phase("screenshot: a real framed raster capture, no warnings")
        shot1 = os.path.join(tmp, "shot1.png")
        r = d.request("screenshot",
                      {"path": shot1, "view": "iso", "fit": True})
        expect(os.path.getsize(shot1) > 1000, f"a real PNG ({r})")
        expect(not r.get("warnings"),
               f"a framed shot of a part carries no warnings ({r})")

        phase("set_render_options reaches the pixels")
        d.call("set_render_options", shadows=False,
               ssao="off")
        shot2 = os.path.join(tmp, "shot2.png")
        d.request("screenshot", {"path": shot2})
        bytes1 = open(shot1, "rb").read()
        bytes2 = open(shot2, "rb").read()
        expect(bytes1 != bytes2,
               "shadows+AO off changes the capture (options are not "
               "silently ignored offscreen)")
        d.call("set_render_options", shadows=True, ssao="half")

        phase("select_face picks against real render meshes")
        face_name = d.call("list_elements")["bodies"][0]["faces"][0]["name"]
        f = d.call("select_face", element=face_name)
        expect("normal" in f and "center" in f,
               f"select_face returns geometry ({f})")
        d.call("orient_to_face")
        # The capture after an orient must wait for the ~1 s camera
        # animation to land (the hold path this mode reimplements), and the
        # view genuinely changed, so no identical-frame warning either.
        shot3 = os.path.join(tmp, "shot3.png")
        r = d.request("screenshot", {"path": shot3})
        expect(not r.get("warnings"),
               f"post-orient capture is fresh and framed ({r})")
        expect(open(shot3, "rb").read() != bytes2,
               "orienting to a face moved the camera")

        phase("render_sheet composites standard views")
        sheet = os.path.join(tmp, "sheet.png")
        d.request("render_sheet", {"path": sheet})
        expect(os.path.getsize(sheet) > 1000, "a composited sheet PNG")

        phase("render_still: deterministic path-traced PNG")
        rt = d.call("rt_build")
        if not rt.get("has_tlas"):
            print("      (no ray tracing on this device — raster-only run)")
        else:
            a = os.path.join(tmp, "still_a.png")
            b = os.path.join(tmp, "still_b.png")
            for path in (a, b):
                r = d.request("render_still", {
                    "width": 96, "height": 72, "samples": 8, "seed": 1,
                    "path": path})
                expect(r.get("ok"), f"render_still succeeds ({r})")
            bytes_a, bytes_b = open(a, "rb").read(), open(b, "rb").read()
            expect(len(bytes_a) > 1000 and b"IHDR" in bytes_a,
                   f"a real PNG came back ({len(bytes_a)} bytes)")
            expect(bytes_a == bytes_b, "same seed, byte-identical render")
            try:
                from PIL import Image
                colors = Image.open(a).convert("RGB").getcolors(maxcolors=64)
                expect(colors is None or len(colors) > 1,
                       "the render is not a flat frame")
            except ImportError:
                print("      (pillow not installed; flat-frame check skipped)")

        phase("drawing tabs refuse offscreen screenshots, naming the remedy")
        # Sheets are ImGui-drawn; the offscreen capture is scene-only, and
        # returning the empty frame cost a session five blank PNGs.
        d.call("create_drawing", name="dwg")
        try:
            d.request("screenshot", {"path": os.path.join(tmp, "dwg.png")})
            raise AssertionError("offscreen drawing screenshot must refuse")
        except RuntimeError as e:
            expect("export_drawing" in str(e), f"names the remedy ({e})")
        d.call("set_active_part", name="Part 1")  # back for the face phase

        phase("orient_to_face is ungated on the assembly tab")
        # It sat in rpc_gate_smoke's GATED_ON_PURPOSE ("assembly-tab
        # behaviour unverified") until this mode provided a render surface
        # to verify on. The verified contract: the part-tab selection does
        # NOT leak across the tab switch, so with nothing selected on the
        # assembly the refusal is the honest "no face selected" — not the
        # part gate's misleading "switch to a part tab".
        d.call("select_face", element=face_name)
        d.call("add_assembly", name="Asm")
        try:
            d.call("orient_to_face")
            raise AssertionError(
                "orient_to_face with no assembly-tab selection must refuse")
        except RuntimeError as e:
            expect("no face selected" in str(e),
                   f"honest refusal, not the part gate ({e})")

        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
