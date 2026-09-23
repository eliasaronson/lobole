#!/usr/bin/env python3
"""A quick still is the viewport's own picture, at the size asked for.

`render_still {mode: "quick"}` draws the model with the raster renderers into
an image of W x H (supersampled k times and averaged down), not the window's
framebuffer (docs/rendering/still-render.md#quick-mode). What must hold:

  - the PNG is exactly W x H, whatever k was;
  - a still of the model is not one colour, and a still of nothing says so;
  - path-tracer parameters are REFUSED in quick mode, naming the one given,
    and an unknown mode is refused naming the modes;
  - nothing is left changed: it resizes every scene renderer, the SSAO
    targets and the line widths for the capture, so the viewport's next
    screenshot must be byte-identical to the one before.

Needs a renderer, not ray tracing, so it runs on llvmpipe too.

Usage: render_quick_smoke.py <path-to-designer-binary>
"""
import hashlib
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_remote import Designer, RemoteError  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    assert head[:8] == b"\x89PNG\r\n\x1a\n", f"{path} is not a PNG"
    return struct.unpack(">II", head[16:24])


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def refused(d, params, needle):
    try:
        r = d.request("render_still", params)
    except RemoteError as e:
        return needle in str(e)
    return not r.get("ok") and needle in (r.get("error") or "")


def main():
    binary = sys.argv[1]
    scratch = tempfile.mkdtemp(prefix="designer_quick_")
    env = dict(os.environ)
    env["XDG_STATE_HOME"] = os.path.join(scratch, "state")
    env["XDG_CONFIG_HOME"] = os.path.join(scratch, "config")

    app = launch_app(binary, extra_args=["--offscreen"], env=env,
                     headless=False)
    try:
        d = Designer(app.port, timeout=300)
        d.request("new_project", {})
        d.request("add_feature", {"feature": {
            "type": "Box", "name": "Block", "position": [0, 0, 0],
            "width": 40, "depth": 30, "height": 20}})
        d.request("fit_view", {})

        before = os.path.join(scratch, "before.png")
        d.request("screenshot", {"path": before})

        # An aspect unlike the viewport's, and k that does not divide it
        # evenly into anything the window has — the size must still be exact.
        for k in (1, 3):
            out = os.path.join(scratch, f"quick{k}.png")
            r = d.request("render_still", {"mode": "quick", "path": out,
                                           "width": 333, "height": 211,
                                           "supersample": k})
            expect(r.get("ok"), f"a quick still at {k}x renders ({r})")
            expect(png_size(out) == (333, 211),
                   f"and is exactly 333 x 211 at {k}x ({png_size(out)})")
            expect(not r.get("warnings"),
                   f"and is a picture of the model ({r.get('warnings')})")

        after = os.path.join(scratch, "after.png")
        d.request("screenshot", {"path": after})
        expect(sha(before) == sha(after),
               "the viewport draws exactly what it drew before the still")

        for param, value in (("samples", 64), ("seed", 2), ("ground", False),
                             ("backdrop", "studio"), ("outlines", True)):
            expect(refused(d, {"mode": "quick", param: value}, param),
                   f"`{param}` is refused in quick mode, by name")
        expect(refused(d, {"mode": "raster"}, "quick"),
               "an unknown mode is refused, naming the modes")
        expect(refused(d, {"mode": "quick", "supersample": 5}, "supersample"),
               "a supersample past 4 is refused")

        d.request("set_camera", {"target": [400000, 0, 0]})
        empty = os.path.join(scratch, "empty.png")
        r = d.request("render_still", {"mode": "quick", "path": empty,
                                       "width": 160, "height": 90})
        expect(r.get("ok"), "a quick still of nothing still writes a file")
        expect(bool(r.get("warnings")), "and says it is one colour")
        return 0
    finally:
        app.terminate()


if __name__ == "__main__":
    sys.exit(main())
