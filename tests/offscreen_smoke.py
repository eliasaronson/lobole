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

        phase("GUI-driving refusals survive, naming the mode")
        expect_error(d, "menu_click", "offscreen", path="Edit/Undo")
        expect_error(d, "key_press", "offscreen", chord="Ctrl+Z")
        expect_error(d, "viewport_drag", "offscreen")
        expect_error(d, "frame_stats", "offscreen")
        # Commit A: raster capture is not wired yet; the refusal must point
        # at what IS available instead of parroting the --headless text.
        expect_error(d, "screenshot", "render_still",
                     path=os.path.join(tmp, "shot.png"))

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

        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
