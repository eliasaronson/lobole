#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Loböle mark modelled and rendered: the logo made in the program it
names. The mark is read from its master SVG (mark.py), so these always match
the icon.

    python3 generators/logo/build.py --port 8801
    python3 generators/logo/build.py --port 8801 --only log_end --samples 1024

Builds, each in a fresh project, and renders with the path tracer:
- log_end: a pine log with the mark branded into its end grain
  (docs/gallery/lobole-log-end.jpg);
- badge: the app icon as a part, the tile in the viewport's colour and the
  mark raised in the sketch colour.

Every size derives from --mark (the mark tile's width, mm): the log, the
brand's depth, the badge's thickness. Output (generated, untracked):
scratch/logo/<name>.png and <name>.lob.
"""
import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
from _lib import check, connect  # noqa: E402
import mark as markmod  # noqa: E402

ROOT = markmod.ROOT
OUT = Path(os.environ["LOBOLE_SCRATCH"]) / "logo" if \
    os.environ.get("LOBOLE_SCRATCH") else ROOT / "scratch" / "logo"


def linear(hexcol):
    """An sRGB hex colour as the LINEAR rgb set_appearance takes."""
    c = [int(hexcol[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    return [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4
            for x in c]


class Builder:
    def __init__(self, d, m, mark_mm, samples):
        self.d, self.m, self.samples = d, m, samples
        # Grid units to mm, the mark centred on the origin, y up.
        self.k = mark_mm / m.view[2]
        self.cx, self.cy = m.view[2] / 2, m.view[3] / 2

    def uv(self, p):
        return [round(self.k * (p[0] - self.cx), 6),
                round(self.k * (self.cy - p[1]), 6)]

    def mark_entities(self, loops=None):
        ents = []
        for loop in loops if loops is not None else self.m.outlines:
            pts = [self.uv(p) for p in loop]
            ents += [{"type": "line", "a": pts[i], "b": pts[(i + 1) % len(pts)]}
                     for i in range(len(pts))]
        return ents

    def add(self, feature, what):
        return check(self.d.request("add_feature", {"feature": feature}), what)

    def sketch(self, z, name, loops=None):
        return self.add({"type": "Sketch", "name": name, "plane": {
            "origin": [0, 0, z], "normal": [0, 0, 1], "uAxis": [1, 0, 0]},
            "entities": self.mark_entities(loops)}, name)["id"]

    def extrude(self, sk, dist, mode, name, **kw):
        f = {"type": "Extrude", "name": name, "sketch_id": sk,
             "distance": dist, "mode": mode}
        f.update(kw)
        return self.add(f, name)["id"]

    def look(self, fid, hexcol=None, **kw):
        p = {"feature_id": fid, **kw}
        if hexcol:
            p["color"] = linear(hexcol)
        self.d.request("set_appearance", p)

    def round_corners(self, fid, r):
        """Fillet a box's four vertical edges."""
        els = self.d.request("list_elements", {})
        body = next(b for b in els["bodies"] if b["created_by"] == fid)
        edges = [{"feature_id": fid, "element": e["name"]}
                 for e in body["edges"] if e["kind"] == "line"
                 and abs(e["midpoint"][0]) > 0 and abs(e["midpoint"][1]) > 0
                 and abs(abs(e["midpoint"][0]) - abs(e["midpoint"][1])) < 1e-6]
        if len(edges) != 4:
            raise SystemExit(f"logo: expected the box's 4 vertical edges, "
                             f"found {len(edges)}")
        self.add({"type": "Fillet", "name": "Corners", "edges": edges,
                  "radius": r}, "corners")

    def shoot(self, name, eye, target):
        self.d.request("set_camera", {"eye": eye, "target": target,
                                      "up": [0, 0, 1]})
        OUT.mkdir(parents=True, exist_ok=True)
        r = self.d.request("render_still", {
            "path": str(OUT / f"{name}.png"), "width": 1600, "height": 1000,
            "samples": self.samples, "fit": True, "outlines": False})
        self.d.request("save_project", {"path": str(OUT / f"{name}.lob")})
        print(f"{name}: {r.get('path')}")

    def log_end(self):
        """The mark branded 1/34 of its width deep into a pine log end,
        the char a thin dark body on the brand's floor."""
        s = self.m.view[2] * self.k
        self.d.request("new_project", {})
        height = s * 1.6
        self.add({"type": "Cylinder", "name": "Log", "on": "Top",
                  "diameter": s * 1.6, "height": height,
                  "material": "wood_pine"}, "log")
        depth = s / 34
        self.extrude(self.sketch(height, "Brand"), depth, "cut", "Brand",
                     reversed=True)
        char = self.extrude(self.sketch(height - depth, "Char"), depth / 6,
                            "new_body", "Char")
        self.look(char, "#1a1310", roughness=0.95)
        self.shoot("log_end", [s * 2.5, -s * 4.2, height + s * 4.2],
                   [0, 0, height * 0.7])

    def badge(self):
        """The icon as a part: the tile (its corner radius the master's)
        in the viewport's colour, the mark raised on it."""
        t = self.m.tile
        s = t["w"] * self.k
        self.d.request("new_project", {})
        thick = s / 8
        tile = self.add({"type": "Box", "name": "Tile", "on": "Top",
                         "width": s, "depth": s, "height": thick}, "tile")["id"]
        self.round_corners(tile, t["rx"] * self.k)
        self.look(tile, t["fill"], roughness=0.45)
        raised = self.extrude(self.sketch(thick, "Mark"), thick / 3,
                              "new_body", "Mark")
        self.look(raised, self.m.color, roughness=0.4)
        self.shoot("badge", [s * 1.15, -s * 2.0, s * 2.5], [0, 0, thick])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--port", type=int)
    ap.add_argument("--mark", type=float, default=48.0,
                    help="the mark tile's width, mm (default 48)")
    ap.add_argument("--samples", type=int, default=512)
    ap.add_argument("--only", choices=["log_end", "badge"])
    args = ap.parse_args()
    b = Builder(connect(args.port), markmod.read(), args.mark, args.samples)
    for name in ("log_end", "badge"):
        if args.only in (None, name):
            getattr(b, name)()
    return 0


if __name__ == "__main__":
    sys.exit(main())
