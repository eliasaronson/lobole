#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Glass is see-through in a path-traced still.

`glass` rendered as an opaque, near-white mirror: the ray tracer declared a
`transmission` on both sides of its material upload and never read it, so a
watch crystal hid the dial in every still (docs/rendering/materials.md#glass).

The shape of the test: a red block behind a pane, the camera looking through
the pane at the block. With the pane `glass` the middle of the frame must be
red — the block, seen through it; with the pane `steel` it must not be. The
one scene rendered twice is what makes the colour mean something: any other
reason for red would be red both times.

Needs a GPU with ray tracing; skips (77) without one, as the other still
tests do. No image library: the PNG is decoded with zlib, which is stdlib.

Usage: render_glass_smoke.py <path-to-designer-binary>
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
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def read_png(path):
    """(width, height, rows of (r, g, b)) for an 8-bit RGB/RGBA PNG."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB",
                                                                body)
            assert depth == 8 and ctype in (2, 6) and interlace == 0, \
                (depth, ctype, interlace)
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc
                                      else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(w)])
        prev = line
    return w, h, rows


def centre_colour(path):
    w, h, rows = read_png(path)
    px = [rows[y][x] for y in range(h // 2 - 4, h // 2 + 4)
          for x in range(w // 2 - 4, w // 2 + 4)]
    return tuple(sum(p[i] for p in px) / len(px) for i in range(3))


def main():
    binary = sys.argv[1]
    scratch = tempfile.mkdtemp(prefix="designer_glass_")
    env = dict(os.environ)
    env["XDG_STATE_HOME"] = os.path.join(scratch, "state")
    env["XDG_CONFIG_HOME"] = os.path.join(scratch, "config")

    app = launch_app(binary, extra_args=["--offscreen"], env=env,
                     headless=False)
    try:
        d = Designer(app.port, timeout=600)
        d.request("new_project", {})
        # The block: 20 mm, red. The pane: 2 mm thick, well in front of it
        # and wider than it, square to the camera.
        box(d, (0, 0, 0), (20, 20, 20), "Block")
        box(d, (-15, -20, -10), (50, 2, 40), "Pane")
        d.request("set_appearance", {"body": 0, "color": [0.85, 0.04, 0.04],
                                     "metallic": 0.0, "roughness": 0.5})
        d.request("set_camera", {"target": [10, 10, 10],
                                 "eye": [10, -110, 10]})

        got = {}
        for pane in ("glass", "steel"):
            d.request("set_material", {"body": 1, "material": pane})
            out = os.path.join(scratch, f"{pane}.png")
            r = d.request("render_still", {"path": out, "width": 96,
                                           "height": 72, "samples": 32,
                                           "ground": False})
            if not r.get("ok"):
                print(f"render_glass: no ray tracing here "
                      f"({r.get('error')}) — skipping")
                return 77
            got[pane] = centre_colour(out)
            print(f"  {pane} pane: centre rgb "
                  f"{tuple(round(v) for v in got[pane])}", flush=True)

        r, g, b = got["glass"]
        expect(r > 1.6 * g and r > 1.6 * b and r > 60,
               f"through GLASS the frame's middle is the red block behind it "
               f"({got['glass']})")
        r, g, b = got["steel"]
        expect(not (r > 1.6 * g and r > 1.6 * b),
               f"through STEEL it is not — the colour is the pane's doing "
               f"({got['steel']})")
        return 0
    finally:
        app.terminate()


if __name__ == "__main__":
    sys.exit(main())
