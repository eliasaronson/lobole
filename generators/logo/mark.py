# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Loböle mark, read from its master SVG (assets/icons/org.lobole.Lobole.svg).

The SVG is the one place the mark is drawn. Everything else -- the 3D
versions, the lockup, the Windows icon, the social preview -- reads it from
here, so a change to the master reaches every copy on the next run.

`read()` returns the mark as filled outlines on the SVG's 24-unit grid
(y DOWN, as in the SVG): each stroked path becomes the outline of its stroke
(square caps, mitred corners), each mark-coloured rect stays a rect. Only
what the master uses is understood -- absolute M/H/V/L/Z on axis-aligned
paths, and rects -- and anything else is refused, naming it, rather than
read as something nearby.
"""
import re
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MASTER = ROOT / "assets" / "icons" / "org.lobole.Lobole.svg"
SVG_NS = "{http://www.w3.org/2000/svg}"


class Mark:
    """The mark on the 24-unit grid: `outlines` is a list of loops (each a
    list of (x, y) corners, outer loops and holes alike -- a sketch tells
    them apart by nesting); `tile` the tile rect's {x, y, w, h, rx, fill,
    stroke, stroke_width}; `color` the mark's colour."""

    def __init__(self, outlines, tile, color, view):
        self.outlines = outlines
        self.tile = tile
        self.color = color
        self.view = view

    def bbox(self):
        xs = [x for loop in self.outlines for x, _ in loop]
        ys = [y for loop in self.outlines for _, y in loop]
        return min(xs), min(ys), max(xs), max(ys)


def _polyline(d):
    """Absolute M/H/V/L/Z only. Returns (points, closed)."""
    tokens = re.findall(r"[MHVLZmhvlz]|-?\d*\.?\d+(?:e-?\d+)?", d)
    pts, closed, i, cmd = [], False, 0, None
    x = y = 0.0
    while i < len(tokens):
        t = tokens[i]
        if t.isalpha():
            if t.islower():
                raise SystemExit(f"mark: relative path command '{t}' in "
                                 f"'{d}': the master uses absolute M/H/V/L/Z")
            cmd = t
            i += 1
            if cmd == "Z":
                closed = True
            continue
        if cmd in ("M", "L"):
            x, y = float(tokens[i]), float(tokens[i + 1])
            i += 2
        elif cmd == "H":
            x = float(t)
            i += 1
        elif cmd == "V":
            y = float(t)
            i += 1
        else:
            raise SystemExit(f"mark: path command '{cmd}' in '{d}' is not "
                             f"one of M/H/V/L/Z")
        pts.append((x, y))
    return pts, closed


def _union_outline(rects):
    """The boundary loops of a union of axis-aligned rects (x0, y0, x1, y1):
    compress the coordinates, mark the covered cells, walk the edges."""
    xs = sorted({v for r in rects for v in (r[0], r[2])})
    ys = sorted({v for r in rects for v in (r[1], r[3])})
    covered = set()
    for i in range(len(xs) - 1):
        for j in range(len(ys) - 1):
            cx, cy = (xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2
            if any(r[0] < cx < r[2] and r[1] < cy < r[3] for r in rects):
                covered.add((i, j))
    # Directed boundary edges, counter-clockwise around covered cells (in a
    # y-down frame), keyed by their start corner.
    edges = {}
    for i, j in covered:
        for (a, b), n in (((((i, j), (i, j + 1))), (i - 1, j)),
                          ((((i, j + 1), (i + 1, j + 1))), (i, j + 1)),
                          ((((i + 1, j + 1), (i + 1, j))), (i + 1, j)),
                          ((((i + 1, j), (i, j))), (i, j - 1))):
            if n not in covered:
                edges.setdefault(a, []).append(b)
    loops = []
    while edges:
        start = next(iter(edges))
        loop, at = [start], start
        while True:
            nxt = edges[at].pop()
            if not edges[at]:
                del edges[at]
            if nxt == start:
                break
            loop.append(nxt)
            at = nxt
        # Drop the corners where the boundary runs straight on.
        pts = [(xs[i], ys[j]) for i, j in loop]
        keep = [p for k, p in enumerate(pts)
                if not ((pts[k - 1][0] == p[0] == pts[(k + 1) % len(pts)][0])
                        or (pts[k - 1][1] == p[1]
                            == pts[(k + 1) % len(pts)][1]))]
        loops.append(keep)
    return loops


def _stroke_rects(pts, closed, w):
    """Each segment as the rect its square-capped stroke covers."""
    h = w / 2
    segs = list(zip(pts, pts[1:] + ([pts[0]] if closed else [])))
    rects = []
    for (x0, y0), (x1, y1) in segs:
        if x0 != x1 and y0 != y1:
            raise SystemExit(f"mark: a diagonal segment ({x0},{y0})-({x1},"
                             f"{y1}): only axis-aligned strokes are read")
        rects.append((min(x0, x1) - h, min(y0, y1) - h,
                      max(x0, x1) + h, max(y0, y1) + h))
    return rects


def read(path=MASTER):
    root = ET.parse(path).getroot()
    view = [float(v) for v in root.get("viewBox").split()]
    tile, color, outlines = None, None, []

    def walk(el, inherited):
        nonlocal tile, color
        style = dict(inherited)
        for k in ("fill", "stroke", "stroke-width", "stroke-linecap"):
            if el.get(k) is not None:
                style[k] = el.get(k)
        tag = el.tag.replace(SVG_NS, "")
        if tag == "rect":
            x, y = float(el.get("x", 0)), float(el.get("y", 0))
            w, h = float(el.get("width")), float(el.get("height"))
            if el.get("rx") is not None and tile is None:
                tile = {"x": x, "y": y, "w": w, "h": h,
                        "rx": float(el.get("rx")), "fill": style.get("fill"),
                        "stroke": style.get("stroke"),
                        "stroke_width": float(style.get("stroke-width", 0))}
            else:
                outlines.extend(_union_outline([(x, y, x + w, y + h)]))
                color = color or style.get("fill")
        elif tag == "path":
            if style.get("stroke-linecap") != "square":
                raise SystemExit("mark: a stroked path without square caps; "
                                 "the outline is computed for square caps")
            pts, closed = _polyline(el.get("d"))
            w = float(style["stroke-width"])
            outlines.extend(_union_outline(_stroke_rects(pts, closed, w)))
            color = color or style.get("stroke")
        for child in el:
            walk(child, style)

    walk(root, {})
    if tile is None or not outlines:
        raise SystemExit(f"mark: {path} has no tile rect or no mark")
    return Mark(outlines, tile, color, view)


if __name__ == "__main__":
    m = read()
    print(f"{len(m.outlines)} loops, bbox {m.bbox()}, colour {m.color}, "
          f"tile {m.tile}")
    for loop in m.outlines:
        print("  ", loop)
