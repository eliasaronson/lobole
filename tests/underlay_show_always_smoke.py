#!/usr/bin/env python3
"""A reference image marked `show_always` is in a quick still.

Underlays drew only while their sketch was being edited, so a model could
never be shown over the plan it was traced from: a render had no sketch in
edit, and the quick still recorded the mesh, field and outline renderers
only (docs/sketching/README.md#reference-images-underlays). `show_always`
(v166) keeps an image drawn whenever its sketch is shown, and a quick still
draws it.

The shape of the test, on an --offscreen instance (which has no frame loop,
so the capture itself has to hand the underlay renderer its images): a pure
red image on a sketch beside a grey block, the camera looking down on both,
no sketch in edit. Without the flag the still has no red in it; with it, the
image's patch is red. Suppressing the sketch takes it away again. Then
`rotation_deg` on add: a long thin image turned 90 degrees stands tall in the
frame where the unturned one lies wide.

Needs a renderer, not ray tracing. No image library: render_glass_smoke's
zlib PNG reader.

Usage: underlay_show_always_smoke.py <path-to-designer-binary>
"""
import os
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_remote import Designer  # noqa: E402
from render_glass_smoke import read_png  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def write_png(path, w, h, rgb):
    """A w x h PNG of one colour."""
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw)))
        f.write(chunk(b"IEND", b""))


def red_pixels(path):
    """The (x, y) of every clearly red pixel."""
    w, h, rows = read_png(path)
    return [(x, y) for y in range(h) for x in range(w)
            if rows[y][x][0] > 150 and rows[y][x][1] < 70
            and rows[y][x][2] < 70]


def main():
    binary = sys.argv[1]
    scratch = tempfile.mkdtemp(prefix="designer_underlay_")
    env = dict(os.environ)
    env["XDG_STATE_HOME"] = os.path.join(scratch, "state")
    env["XDG_CONFIG_HOME"] = os.path.join(scratch, "config")
    square = os.path.join(scratch, "red.png")
    write_png(square, 8, 8, (255, 0, 0))
    strip = os.path.join(scratch, "strip.png")
    write_png(strip, 16, 4, (255, 0, 0))

    app = launch_app(binary, extra_args=["--offscreen"], env=env,
                     headless=False)
    try:
        d = Designer(app.port, timeout=300)
        d.request("new_project", {})
        box(d, (0, 0, 0), (20, 20, 20), "Block")
        plan = d.call("add_feature", feature={
            "type": "Sketch", "name": "Plan",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [-60, -20], "b": [-58, -20]}]
        })["id"]
        # Looking down on the plane, just off vertical so "up" is defined.
        d.request("set_camera", {"target": [-20, 0, 0],
                                 "eye": [-20, -2, 160]})

        n = [0]

        def still():
            n[0] += 1
            out = os.path.join(scratch, f"still{n[0]}.png")
            r = d.request("render_still", {"mode": "quick", "path": out,
                                           "width": 160, "height": 120,
                                           "supersample": 1})
            expect(r.get("ok"), f"quick still {n[0]} rendered")
            return red_pixels(out)

        img = d.call("add_reference_image", sketch_id=plan, path=square,
                     at=[-40, 0], units_per_pixel=3.0, opacity=1.0)["image_id"]
        expect(len(still()) == 0,
               "without show_always the underlay is not in a quick still")

        d.call("set_reference_image", sketch_id=plan, image_id=img,
               show_always=True)
        red = still()
        expect(len(red) > 200,
               f"with show_always it is: {len(red)} red pixels")

        d.call("suppress_feature", feature_id=plan, suppressed=True)
        expect(len(still()) == 0, "a suppressed sketch's underlay is not drawn")
        d.call("suppress_feature", feature_id=plan, suppressed=False)
        d.call("delete_reference_image", sketch_id=plan, image_id=img)

        def extent(pixels):
            xs = [p[0] for p in pixels]
            ys = [p[1] for p in pixels]
            return max(xs) - min(xs) + 1, max(ys) - min(ys) + 1

        flat = d.call("add_reference_image", sketch_id=plan, path=strip,
                      at=[-40, 0], units_per_pixel=3.0, opacity=1.0,
                      show_always=True)["image_id"]
        w0, h0 = extent(still())
        d.call("delete_reference_image", sketch_id=plan, image_id=flat)
        d.call("add_reference_image", sketch_id=plan, path=strip,
               at=[-40, 0], units_per_pixel=3.0, opacity=1.0,
               show_always=True, rotation_deg=90)
        w1, h1 = extent(still())
        expect(w0 > 2 * h0 and h1 > 2 * w1,
               f"rotation_deg turns it: {w0}x{h0} unturned, {w1}x{h1} turned")
        return 0
    finally:
        app.terminate()


if __name__ == "__main__":
    sys.exit(main())
