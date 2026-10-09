#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

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
    screenshot must be byte-identical to the one before;
  - the LINES are the viewport's, in output pixels, at any supersample:
    across three edges of a light-grey block, what the lines take off the
    same picture drawn without them -- at their darkest, and summed across
    the line -- agrees with the viewport's within 10% at 2x and 4x (1x is
    the viewport's picture, byte for byte)
    (docs/rendering/still-render.md#lines-in-a-quick-still). They used to
    be drawn 1 px wide at k times the size and averaged down in linear
    light: a third of the viewport's darkness at 2x, a ninth at 4x;
  - `outlines: false` draws none, `line_weight` scales them, and a bad value
    of either is refused.

Needs a renderer, not ray tracing, so it runs on llvmpipe too.

Usage: render_quick_smoke.py <path-to-designer-binary>
"""
import hashlib
import math
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_remote import Designer, RemoteError  # noqa: E402
from render_glass_smoke import read_png  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


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


def _sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def _dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def _unit(a):
    n = math.sqrt(_dot(a, a))
    return [x / n for x in a]


def projector(eye, target, up, fov_deg, w, h):
    """World point -> pixel, for the camera set_camera was given."""
    f = _unit(_sub(target, eye))
    s = _unit(_cross(f, up))
    u = _cross(s, f)
    t = math.tan(math.radians(fov_deg) / 2)

    def proj(p):
        d = _sub(p, eye)
        z = _dot(d, f)
        return ((_dot(d, s) / (z * t * w / h) + 1) / 2 * w,
                (1 - _dot(d, u) / (z * t)) / 2 * h)
    return proj


# Three edges of the 40 x 20 x 30 block, each between two faces the camera
# sees: a line on the part, not a silhouette against the background.
EDGES = (((40, 0, 0), (40, 0, 30)), ((0, 0, 30), (40, 0, 30)),
         ((40, 0, 30), (40, 20, 30)))


def _profiles(path, proj):
    """Luminance across each EDGE at points along it: 23 samples a pixel
    apart on the edge's normal, centred on where it projects. Encoded 8-bit
    luminance -- the picture as it is looked at."""
    _, _, rows = read_png(path)

    def at(x, y):
        r, g, b = rows[y][x]
        return 0.2126 * r + 0.7152 * g + 0.0722 * b

    def lum(x, y):  # bilinear
        x0, y0 = int(x), int(y)
        fx, fy = x - x0, y - y0
        return ((1 - fx) * (1 - fy) * at(x0, y0) + fx * (1 - fy) * at(x0 + 1, y0)
                + (1 - fx) * fy * at(x0, y0 + 1) + fx * fy * at(x0 + 1, y0 + 1))

    out = []
    for a, b in EDGES:
        pa, pb = proj(a), proj(b)
        dx, dy = pb[0] - pa[0], pb[1] - pa[1]
        n = (-dy / math.hypot(dx, dy), dx / math.hypot(dx, dy))
        for t in (0.3, 0.4, 0.5, 0.6, 0.7):
            px, py = pa[0] + t * dx, pa[1] + t * dy
            out.append([lum(px + k * n[0], py + k * n[1])
                        for k in range(-11, 12)])
    return out


def line_ink(lines, bare, proj):
    """(peak, ink) of the lines alone: what drawing them took off the same
    picture without them -- the most at one sample, and the sum across the
    line -- averaged along the edges. Measured against the bare picture, not
    the faces beside the line, so shading that moves with the resolution
    (SSAO is drawn at the capture's size) does not count as line."""
    peaks, inks = [], []
    for p, q in zip(_profiles(lines, proj), _profiles(bare, proj)):
        diff = [q[i] - p[i] for i in range(6, 17)]  # +-5 px of the edge
        peaks.append(max(diff))
        inks.append(sum(diff))
    return sum(peaks) / len(peaks), sum(inks) / len(inks)


def darkest_line(path, proj):
    """How far the darkest sample near each edge falls below the darker face
    beside it, averaged: about 0 where nothing is drawn on the edge."""
    dark = []
    for p in _profiles(path, proj):
        c = min(range(8, 15), key=lambda i: p[i])
        dark.append(min(sum(p[c - 7:c - 4]), sum(p[c + 5:c + 8])) / 3 - p[c])
    return sum(dark) / len(dark)


def lines_match_the_viewport(d, scratch):
    d.request("new_project", {})
    block = box(d, (0, 0, 0), (40, 20, 30), "Block")
    d.request("set_appearance", {"feature_id": block["id"],
                                 "color": [0.6, 0.6, 0.6]})  # light grey
    eye, target, up = [110, -120, 95], [20, 10, 15], [0, 0, 1]
    d.request("set_camera", {"eye": eye, "target": target, "up": up})
    # Stills at the viewport's own size and camera, so a line is the same
    # line in every picture.
    shot = os.path.join(scratch, "lines_viewport.png")
    d.request("screenshot", {"path": shot})
    w, h = png_size(shot)
    proj = projector(eye, target, up,
                     d.request("get_state", {})["camera"]["fov_deg"], w, h)

    def still(name, **params):
        out = os.path.join(scratch, f"lines_{name}.png")
        r = d.request("render_still", {"mode": "quick", "path": out,
                                       "width": w, "height": h, **params})
        expect(r.get("ok"), f"the {name} still renders ({r.get('error')})")
        return out, r

    # The viewport cannot be asked to draw without its lines, but a quick
    # still at 1x IS the viewport's picture -- byte for byte -- so its bare
    # twin is the viewport's.
    one, _ = still("1x", supersample=1)
    expect(sha(one) == sha(shot),
           "a 1x still of the viewport's size is the viewport's picture")
    vp_peak, vp_ink = line_ink(shot, still("1x_bare", supersample=1,
                                           outlines=False)[0], proj)
    print(f"  viewport: line peak {vp_peak:.1f}, ink {vp_ink:.1f}", flush=True)
    expect(vp_peak > 25, f"the viewport draws dark lines ({vp_peak:.1f})")

    for k in (2, 4):
        lines, r = still(f"{k}x", supersample=k)
        bare, bare_r = still(f"{k}x_bare", supersample=k, outlines=False)
        peak, ink = line_ink(lines, bare, proj)
        print(f"  quick {k}x: line peak {peak:.1f}, ink {ink:.1f}", flush=True)
        expect(abs(peak - vp_peak) <= 0.1 * vp_peak,
               f"at {k}x a line is as dark as the viewport's "
               f"({peak:.1f} vs {vp_peak:.1f})")
        expect(abs(ink - vp_ink) <= 0.1 * vp_ink,
               f"and as heavy ({ink:.1f} vs {vp_ink:.1f})")
        expect(r.get("line_weight") == 1.0,
               f"and the reply says it drew the viewport's 1 px "
               f"({r.get('line_weight')})")
        if k == 2:
            bare2, bare2_r = bare, bare_r

    dark = darkest_line(bare2, proj)
    expect(dark < 0.1 * vp_peak, f"outlines: false draws no lines ({dark:.1f})")
    expect(bare2_r.get("outlines") is False and "line_weight" not in bare2_r,
           "and the reply says so")
    heavy, r = still("weight2", line_weight=2)
    peak2, ink2 = line_ink(heavy, bare2, proj)
    expect(ink2 > 1.6 * vp_ink and peak2 > vp_peak,
           f"line_weight 2 draws heavier lines (ink {ink2:.1f} vs "
           f"{vp_ink:.1f})")
    expect(r.get("line_weight") == 2.0, "and reports the weight it drew")
    light, _ = still("weight_half", line_weight=0.5)
    _, ink_half = line_ink(light, bare2, proj)
    expect(ink_half < 0.7 * vp_ink,
           f"line_weight 0.5 draws lighter ones (ink {ink_half:.1f})")


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
        box(d, (0, 0, 0), (40, 20, 30), "Block")
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
                             ("backdrop", "studio")):
            expect(refused(d, {"mode": "quick", param: value}, param),
                   f"`{param}` is refused in quick mode, by name")
        for value in (0, 0.1, 5, -1, "thick", None):
            expect(refused(d, {"mode": "quick", "line_weight": value},
                           "line_weight"),
                   f"line_weight {value!r} is refused, naming the range")
        expect(refused(d, {"mode": "quick", "outlines": "yes"}, "outlines"),
               "outlines must be true or false")
        expect(refused(d, {"mode": "path", "line_weight": 2}, "quick"),
               "line_weight is refused on a traced still, naming quick mode")
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

        lines_match_the_viewport(d, scratch)
        return 0
    finally:
        app.terminate()


if __name__ == "__main__":
    sys.exit(main())
