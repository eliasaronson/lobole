#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The 2D brand files, made from the master mark
(assets/icons/org.lobole.Lobole.svg) and nothing else drawn by hand:

- assets/icons/org.lobole.Lobole.png   the 256 px raster the desktop installs
- assets/icons/org.lobole.Lobole.ico   the Windows icon: 16-256 px in one file
- assets/brand/lobole-lockup-dark.svg  mark + "Loböle", for a dark ground
- assets/brand/lobole-lockup-light.svg the same for a light ground
- assets/brand/social-preview.png      GitHub's 1280x640 card

The wordmark is Archivo SemiBold (SIL OFL 1.1, assets/brand/README.md),
converted to outlines here so the font itself never ships. It needs the
font file, fetched once:

    curl -LO 'https://github.com/google/fonts/raw/main/ofl/archivo/Archivo%5Bwdth%2Cwght%5D.ttf'
    nix-shell -p librsvg imagemagick \\
        'python3.withPackages (p: [ p.fonttools p.uharfbuzz ])' --run \\
        "python3 generators/logo/brand.py --font 'Archivo[wdth,wght].ttf'"

Without --font only the icons are made.
"""
import argparse
import io
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import mark as markmod  # noqa: E402

ROOT = markmod.ROOT
ICONS = ROOT / "assets" / "icons"
BRAND = ROOT / "assets" / "brand"
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
# The wordmark's ink on each ground: the app's text colour on the viewport,
# and the viewport colour on white.
INK = {"dark": "#e8eaed", "light": "#23262e"}
TAGLINE = "Open-source parametric CAD"


def need(tool):
    if shutil.which(tool) is None:
        raise SystemExit(f"brand: '{tool}' is not on PATH (see this file's "
                         f"docstring for the nix-shell line)")


def rasterize(svg_path, size_px, out, height_px=None):
    need("rsvg-convert")
    subprocess.run(["rsvg-convert", "-w", str(size_px), "-h",
                    str(height_px or size_px), str(svg_path), "-o", str(out)],
                   check=True)


def icons():
    master = markmod.MASTER
    rasterize(master, 256, ICONS / "org.lobole.Lobole.png")
    need("magick")
    with tempfile.TemporaryDirectory() as tmp:
        pngs = []
        for s in ICO_SIZES:
            p = Path(tmp) / f"{s}.png"
            rasterize(master, s, p)
            pngs.append(str(p))
        subprocess.run(["magick", *pngs, str(ICONS / "org.lobole.Lobole.ico")],
                       check=True)
    print(f"icons: {ICONS / 'org.lobole.Lobole.png'}, "
          f"{ICONS / 'org.lobole.Lobole.ico'} ({', '.join(map(str, ICO_SIZES))} px)")


# --- the wordmark as outlines ----------------------------------------------
class Face:
    """One static instance of the variable font: shaped with HarfBuzz (so
    kerning applies), drawn with fontTools."""

    def __init__(self, path, weight):
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
        import uharfbuzz as hb
        font = TTFont(path)
        if "fvar" in font:
            font = instancer.instantiateVariableFont(
                font, {"wght": weight, "wdth": 100})
        buf = io.BytesIO()
        font.save(buf)
        self.tt = TTFont(io.BytesIO(buf.getvalue()))
        self.hb_font = hb.Font(hb.Face(buf.getvalue()))
        self.upem = self.tt["head"].unitsPerEm
        self.cap = self.tt["OS/2"].sCapHeight

    def path(self, text, x0, baseline, cap_height):
        """SVG path data for `text`, its baseline at `baseline` and its
        capitals `cap_height` tall, starting at x0. Returns (d, width)."""
        import uharfbuzz as hb
        from fontTools.pens.svgPathPen import SVGPathPen
        from fontTools.pens.transformPen import TransformPen
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(self.hb_font, buf, {"kern": True, "liga": True})
        k = cap_height / self.cap
        glyphs = self.tt.getGlyphSet()
        order = self.tt.getGlyphOrder()
        pen = SVGPathPen(glyphs)
        x = 0
        for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
            name = order[info.codepoint]
            gx = x0 + (x + pos.x_offset) * k
            gy = baseline - pos.y_offset * k
            # Font units are y-up; the SVG is y-down.
            glyphs[name].draw(TransformPen(pen, (k, 0, 0, -k, gx, gy)))
            x += pos.x_advance
        return pen.getCommands(), x * k


def tile_svg(m, x, y, size):
    """The master tile and mark, placed at (x, y) and `size` across."""
    k = size / m.view[2]
    t = m.tile
    loops = " ".join(
        "M" + " L".join(f"{x + px * k:.3f} {y + py * k:.3f}" for px, py in loop)
        + " Z" for loop in m.outlines)
    return (f'<rect x="{x + t["x"] * k:.3f}" y="{y + t["y"] * k:.3f}" '
            f'width="{t["w"] * k:.3f}" height="{t["h"] * k:.3f}" '
            f'rx="{t["rx"] * k:.3f}" fill="{t["fill"]}" stroke="{t["stroke"]}" '
            f'stroke-width="{t["stroke_width"] * k:.3f}"/>'
            f'<path d="{loops}" fill="{m.color}" fill-rule="evenodd"/>')


def lockups(m, face):
    BRAND.mkdir(parents=True, exist_ok=True)
    # The tile is 24 units; the capitals are half of it, centred on it, and
    # the word starts a quarter of the tile clear of it.
    size, cap = 24.0, 12.0
    baseline = size / 2 + cap / 2
    d, width = face.path("Loböle", size * 1.25, baseline, cap)
    w = size * 1.25 + width + size * 0.1
    for ground, ink in INK.items():
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 '
               f'{w:.3f} {size}" width="{w * 10:.0f}" height="{size * 10:.0f}">'
               f'<title>Loböle</title>{tile_svg(m, 0, 0, size)}'
               f'<path d="{d}" fill="{ink}"/></svg>\n')
        out = BRAND / f"lobole-lockup-{ground}.svg"
        out.write_text(svg)
        print(f"lockup: {out}")


def social(m, face, regular):
    """1280x640: the viewport's colour with the sketch grid on it, the tile,
    the word and the tagline."""
    W, H = 1280, 640
    grid = []
    for gx in range(0, W + 1, 40):
        grid.append(f'<path d="M{gx} 0V{H}" stroke="#30374a" '
                    f'stroke-width="{2 if gx % 200 == 0 else 1}"/>')
    for gy in range(0, H + 1, 40):
        grid.append(f'<path d="M0 {gy}H{W}" stroke="#30374a" '
                    f'stroke-width="{2 if gy % 200 == 0 else 1}"/>')
    size = 240
    word_cap = 96
    d, wword = face.path("Loböle", 0, 0, word_cap)
    gap = size * 0.25
    total = size + gap + wword
    x0 = (W - total) / 2
    y0 = (H - size) / 2 - 30
    baseline = y0 + size / 2 + word_cap / 2
    word = (f'<path transform="translate({x0 + size + gap:.2f} '
            f'{baseline:.2f})" d="{d}" fill="{INK["dark"]}"/>')
    td, wtag = regular.path(TAGLINE, 0, 0, 30)
    tag = (f'<path transform="translate({(W - wtag) / 2:.2f} '
           f'{y0 + size + 110:.2f})" d="{td}" fill="#8cd9ff"/>')
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}">'
           f'<rect width="{W}" height="{H}" fill="#23262e"/>{"".join(grid)}'
           f'{tile_svg(m, x0, y0, size)}{word}{tag}</svg>\n')
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "social.svg"
        src.write_text(svg)
        out = BRAND / "social-preview.png"
        rasterize(src, W, out, H)
    print(f"social preview: {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--font", help="Archivo's variable TTF (see above)")
    args = ap.parse_args()
    m = markmod.read()
    icons()
    if not args.font:
        print("brand: no --font, so no lockup or social preview")
        return 0
    if not os.path.isfile(args.font):
        raise SystemExit(f"brand: no font at {args.font}")
    semibold = Face(args.font, 600)
    lockups(m, semibold)
    social(m, semibold, Face(args.font, 400))
    return 0


if __name__ == "__main__":
    sys.exit(main())
