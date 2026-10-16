#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The board, the base jig, and the set: added to scratch/chess/blanks.dsgn.

    python3 generators/chess/build_board.py --port N

Board: one slab of the light wood, 8 x 8 squares twice the pawn's base
across (FIDE), a frame, the dark squares pocketed to take tinted epoxy. Their
inside corners are rounded to what a 2 mm cutter leaves, so the model is what
the machine cuts. The frame's top edge is chamfered for a V-bit.

Base jig: an MDF plate the turned pieces hang in, upside down, seated on
their coves, so their undersides (felt recess, weight bore) can be cut on
the bed. One hole per piece of one colour.

There are no piece parts per rank and wood: the set places the one piece
part in a rank's configuration and a side's material (v135).
"""
import argparse
import math
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What the run makes -- projects, programs, renders, reports -- is
# regenerable, so it goes to the untracked scratch/, not beside the code.
# LOBOLE_SCRATCH moves it (tests/generator_runs.py runs it in a private
# directory); unset, the repo's scratch/.
OUT = Path(os.environ.get("LOBOLE_SCRATCH")
           or HERE.parent.parent / "scratch") / "chess"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(HERE))
from lobole.sdk import connect, Plane  # noqa: E402
from build_piece import SIZES, ORDER  # noqa: E402

KING_H = 85.0
SLAB_T = 19.0   # the board as bought: 3/4 in
POCKET = 2.0    # epoxy depth, mm
CORNER_R = 1.0  # a dark square's inside corner: what a 2 mm cutter leaves
PLATE_T = 18.0  # the jig's MDF
WOODS = {"light": "wood_maple", "dark": "wood_walnut"}
# the ranks of one side, and how many of each
SIDE = {"king": 1, "queen": 1, "bishop": 2, "knight": 2, "rook": 2,
        "pawn": 8}


def board(app):
    part = app.part("board")
    part.activate()
    sq = part.param("square", "2 * 0.34 * king_h",
                    comment="FIDE: twice the pawn's base across")
    frame = part.param("frame", "0.3 * square", comment="the border")
    slab = part.param("slab_t", SLAB_T, comment="the board as bought")
    depth = part.param("pocket_d", POCKET, comment="epoxy depth")
    side = sq * 8 + frame * 2
    sk = part.sketch(Plane.xy(), name="Slab")
    sk.polyline([(0, 0), (side, 0), (side, side), (0, side)], close=True)
    body = part.extrude(sk.add(), slab, name="Slab")
    # The dark squares: a1 is dark, so (file + rank) even.
    sk = part.sketch(Plane.xy(SLAB_T), name="Dark squares")
    r = CORNER_R
    for f in range(8):
        for k in range(8):
            if (f + k) % 2:
                continue
            x0 = frame + sq * f
            y0 = frame + sq * k
            x1, y1 = x0 + sq, y0 + sq
            # a rounded square, each corner an arc of CORNER_R
            sk.line((x0 + r, y0), (x1 - r, y0))
            sk.arc((x1 - r, y0 + r), r, -90, 0)
            sk.line((x1, y0 + r), (x1, y1 - r))
            sk.arc((x1 - r, y1 - r), r, 0, 90)
            sk.line((x1 - r, y1), (x0 + r, y1))
            sk.arc((x0 + r, y1 - r), r, 90, 180)
            sk.line((x0, y1 - r), (x0, y0 + r))
            sk.arc((x0 + r, y0 + r), r, 180, 270)
    s = sk.add()
    app.add_feature(feature={"type": "Extrude", "name": "Dark squares",
                             "sketch_id": s.id, "mode": "cut",
                             "distance": POCKET, "distance_expr": depth.expr,
                             "reversed": True})
    # The frame's top outer edge, chamfered: where the slab's top meets its
    # sides.
    slab_f = body.feature
    top_outer = part.bodies()[0].edges(between=(slab_f.end_cap(),
                                                slab_f.walls()))
    assert len(top_outer) == 4, [e.name for e in top_outer]
    part.chamfer(top_outer, 3.0, name="Frame chamfer")
    # The files a-h along the near border and the ranks 1-8 up the left one,
    # single stroke (Hershey) for a V-bit, each centred on its square's line
    # in the middle of the border: sized from the border, so a bigger set
    # gets bigger letters.
    label = part.param("label_h", "0.3 * frame",
                       comment="cap height of the a-h and 1-8 labels")
    sk = part.sketch(Plane.xy(SLAB_T), name="Labels")
    for i in range(8):
        along = frame + sq * i + sq * 0.5
        sk.text("abcdefgh"[i], (along, frame * 0.5), label, font="hershey",
                halign="center", valign="middle")
        sk.text(str(i + 1), (frame * 0.5, along), label, font="hershey",
                halign="center", valign="middle")
    sk.add()
    part.material("wood_oak")
    return part


def jig(app):
    """The plate for one side's sixteen pieces, hanging upside down."""
    part = app.part("base_jig")
    part.activate()
    # Hole per rank: the piece seats on its cove where the profile is 0.66 of
    # the base radius across -- a ring contact, self-centring.
    holes = []
    x = 20.0
    cols = []
    for kind, n in SIDE.items():
        base_r = SIZES[kind][1] / 2 * KING_H
        for _ in range(n):
            cols.append((kind, base_r))
    pitch_y = 2 * max(SIZES[k][1] / 2 * KING_H for k in SIDE) + 10
    # two rows of eight
    row = [cols[:8], cols[8:]]
    width = 0.0
    for ri, rr in enumerate(row):
        x = 0.0
        for kind, base_r in rr:
            x += base_r + 6
            holes.append((kind, x, 20 + pitch_y * (ri + 0.5), base_r))
            x += base_r + 6
        width = max(width, x)
    depth_y = 40 + pitch_y * 2
    sk = part.sketch(Plane.xy(), name="Plate")
    sk.polyline([(-10, 0), (width + 10, 0), (width + 10, depth_y),
                 (-10, depth_y)], close=True)
    part.extrude(sk.add(), PLATE_T, name="Plate")
    sk = part.sketch(Plane.xy(PLATE_T), name="Seats")
    for kind, hx, hy, base_r in holes:
        sk.circle((hx, hy), r=0.66 * base_r)
    s = sk.add()
    app.add_feature(feature={"type": "Extrude", "name": "Seats",
                             "sketch_id": s.id, "mode": "cut",
                             "end_mode": "through_all", "distance": 1.0,
                             "reversed": True})
    part.material("mdf")
    return part, holes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    args = ap.parse_args()
    app = connect(args.port)
    app.call("open_project", path=str(OUT / "blanks.dsgn"))
    piece = app.part("piece", create=False)
    b = board(app)
    print("board:", app.call("get_state")["regen"], b.bbox())
    j, holes = jig(app)
    print("jig:", app.call("get_state")["regen"]["ok"], j.bbox(), len(holes))
    app.save_project(path=str(OUT / "set.dsgn"))


if __name__ == "__main__":
    main()
