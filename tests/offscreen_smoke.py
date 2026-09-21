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

        phase("frame_stats: per-pass GPU time, which is what this mode is for")
        # No frame loop here, so no fps and no CPU phase split — and this is
        # the one place a PASS can be measured honestly: with no swapchain
        # there is no image to wait for, so `main` is shading rather than the
        # display stalling inside it (docs/rendering/
        # offscreen-gpu-timing-plan.md).
        # Earlier phases have already rendered, so start a fresh window and
        # check that a window with nothing in it says so rather than
        # answering with stale numbers.
        d.request("frame_stats", {"reset": True})
        expect_error(d, "frame_stats", "nothing rendered yet")
        for i in range(4):
            d.request("screenshot", {"path": os.path.join(tmp, f"fs{i}.png")})
        fs = d.request("frame_stats")
        expect(fs["mode"] == "offscreen", f"mode named ({fs})")
        expect("fps" not in fs and "phases_ms" not in fs,
               f"no frame numbers where there is no frame loop ({fs})")
        expect(fs["renders"] >= 4, f"one sample per capture ({fs})")
        g = fs["gpu"]
        expect("supported" in g, f"timestamp support is stated ({g})")
        if g["supported"]:
            expect(g["samples"] >= 4, f"samples counted ({g})")
            expect(g["total_ms"] > 0.0, f"a pass took real time ({g})")
            # The median is what a measurement reads: a mean over a handful
            # of renders carries the first one's first-use work.
            expect(g["total_p50_ms"] > 0.0, f"medians reported ({g})")
        # A reset starts a fresh window rather than averaging in what came
        # before — six renders of a new configuration once read as the
        # average of those six and the 114 before them.
        r = d.request("frame_stats", {"reset": True})
        expect(r.get("reset") is True, f"reset acknowledged ({r})")
        expect_error(d, "frame_stats", "nothing rendered yet")

        phase("frame_stats {renders}: a burst, which is how to measure")
        # Saving a PNG between renders is CPU work that leaves the GPU idle,
        # and an idle laptop GPU drops to its lowest clock: the same scene
        # measured 12.5 ms a frame over saved captures and 1.4 ms over a
        # burst, which is the difference between measuring the card and
        # measuring its idle floor. The burst also warms up first, because
        # pipelines are built on first use and the clocks ramp over the first
        # tenth of a second.
        burst = d.request("frame_stats", {"renders": 40})
        expect(burst["renders"] >= 40, f"the burst rendered ({burst})")
        expect("fps" not in burst, f"still no frame loop to report ({burst})")
        if burst["gpu"]["supported"]:
            expect(burst["gpu"]["total_p50_ms"] > 0.0,
                   f"the burst measured something ({burst['gpu']})")
        expect_error(d, "frame_stats", "between 1 and 10000", renders=0)
        expect_error(d, "frame_stats", "between 1 and 10000", renders=999999)

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

        phase("a rotary simulation screenshots, and turns the part")
        # Two things at once. SyncCamPlayback runs on the offscreen path, and
        # it asked ImGui for the time -- a null dereference where there is no
        # ImGui context, so cam_simulate followed by a screenshot killed the
        # process with SIGSEGV from the day the 12 Hz remesh cap landed until
        # the rotary picture made somebody take that screenshot. And the
        # picture itself: a program whose A moves plays now, on a radius
        # field, instead of being refused (docs/cam/rotary-plan.md).
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Cylinder", "name": "Bar", "center": [0, 0, 0],
            "direction": [1, 0, 0], "radius": 15.0, "height": 40.0})
        job = d.call("machine_part")
        sid = d.call("add_cam_setup", name="Trunnion", post="mach3")["id"]
        d.call("set_cam_setup", id=sid, has_rotary_axis=True, safe_z=30.0,
               stock={"mode": "relative", "margin": [0.0, 1.0, 1.0],
                      "bottom_margin": 0.0})
        d.call("set_cam_setup", id=sid, tool={
            "number": 1, "name": "6 mm ball", "diameter": 6.0,
            "tool_type": "ball"})
        d.call("add_cam_op", setup=sid, type="surface_rotary", tool=1,
               stock_to_leave=1.0, stepover=0.6)
        sim = d.request("cam_simulate", {"setup": sid, "at": 0.5},
                        timeout=600)
        expect(sim.get("rotary") is True, f"plays as rotary ({sim})")
        expect(sim.get("removed_mm3", 0) > 0,
               f"the radius field was carved ({sim})")
        # The crash was HERE, in the scene sync the screenshot triggers.
        shot = d.request("screenshot", {"path": os.path.join(tmp, "rot.png")},
                         timeout=300)
        expect(os.path.exists(os.path.join(tmp, "rot.png")),
               f"the screenshot was written ({shot})")
        expect(d.call("ping")["ok"], "and the app is still alive after it")

        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
