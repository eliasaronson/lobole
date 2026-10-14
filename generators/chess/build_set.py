#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The set assembled, its bill of materials, and the pieces' drawing.

    python3 generators/chess/build_set.py --port N

Opens scratch/chess/set.dsgn (build_board.py): places the board and the 32
pieces in the starting position, checks the BOM and the FIDE fits, draws the
six ranks on one sheet with the weight bore's fit, and saves set.dsgn.
"""
import argparse
import json
import math
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What the run makes -- projects, programs, renders, reports -- is
# regenerable, so it goes to the untracked scratch/, not beside the code.
# DESIGNER_SCRATCH moves it (tests/generator_runs.py runs it in a private
# directory); unset, the repo's scratch/.
OUT = Path(os.environ.get("DESIGNER_SCRATCH")
           or HERE.parent.parent / "scratch") / "chess"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(HERE))
from lobole.sdk import connect  # noqa: E402
from build_piece import SIZES, ORDER  # noqa: E402
from build_board import SLAB_T, KING_H, WOODS  # noqa: E402

BACK = ["rook", "knight", "bishop", "queen", "king", "bishop", "knight",
        "rook"]


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qaxis(axis, deg):
    h = math.radians(deg) / 2
    s = math.sin(h)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h))


# The pieces are modelled along +X, underside at x = 0: stood up, +X -> +Z.
UPRIGHT = qaxis((0, 1, 0), -90)


def assemble(app):
    asm = app.assembly("chess_set")
    asm.activate()
    board = app.call("add_occurrence", part="board", fixed=True,
                     name="board")
    sq = 2 * 0.34 * KING_H
    frame = 0.3 * sq
    placed = []
    for wood, back_rank, pawn_rank, face in (("light", 0, 1, -90),
                                             ("dark", 7, 6, 90)):
        # a knight's nose is modelled toward +Z, which stands up as -X; turn
        # each side's to face the other
        turn = qmul(qaxis((0, 0, 1), face), UPRIGHT)
        for f in range(8):
            for rank, kind in ((back_rank, BACK[f]), (pawn_rank, "pawn")):
                pos = [frame + sq * (f + 0.5), frame + sq * (rank + 0.5),
                       SLAB_T]
                name = f"{kind} {'abcdefgh'[f]}{rank + 1}"
                # the one piece part, as this rank, in this side's wood
                r = app.call("add_occurrence", part="piece", name=name,
                             configuration=kind, material=WOODS[wood],
                             transform={"position": pos,
                                        "orientation": list(turn)})
                placed.append((name, r["id"]))
    return asm, placed


def fits(app):
    """FIDE: the square twice the pawn's base across; every base inside its
    square with room to take it by hand."""
    sq = 2 * 0.34 * KING_H
    out = {}
    for kind in ORDER:
        base_d = SIZES[kind][1] * KING_H
        out[kind] = {"base_d": round(base_d, 2),
                     "square": round(sq, 2),
                     "fills": round(base_d / sq, 3)}
        assert base_d < 0.75 * sq, (kind, base_d, sq)
    return out


def drawing(app):
    piece_cfgs = {c["name"]: c["id"] for c in app.call(
        "list_configurations", part="piece")["configurations"]}
    d = app.call("create_drawing", name="Pieces",
                 sheet={"size": "a3", "landscape": True,
                        "title_block": {"project": "Chess set",
                                        "document": "Turned pieces",
                                        "author": "Designer dogfood",
                                        "notes": "king_h = 85; every size "
                                                 "follows it"}})
    views = {}
    # The pieces lie along X; each front view is turned 90 so it stands
    # upright, one row of six at 1:1 with room between for the dimensions.
    for i, kind in enumerate(["king", "queen", "bishop", "knight", "rook",
                              "pawn"]):
        v = app.call("add_drawing_view", source="piece", orientation="front",
                     scale=1.0, position=[45.0 + 62.0 * i, 160.0],
                     rotation_deg=90.0, configuration=piece_cfgs[kind])
        views[kind] = v["id"]
    app.call("update_drawing", budget_s=120)
    # Each view's height and base: the underside is the long level line at
    # the bottom; the top is the highest point of whatever curve reaches
    # it -- an `extreme`, because on a ball, a dome or the knight's head it
    # is neither end of its curve.
    for kind, vid in views.items():
        # edges and silhouettes both: a turned part's outline in a side view
        # is mostly silhouette. A silhouette is picked as `sil_face`.
        geo = [g for g in app.call("list_drawing_geometry", view=vid)
               ["geometry"] if g.get("visible") and "bbox" in g
               and g.get("kind") in ("edge", "silhouette")]
        ref = lambda g, **k: dict({("sil_face" if g["kind"] == "silhouette"
                                    else "edge"): g["ref"],
                                   "occurrence": g["source_id"]}, **k)
        ymin = min(g["bbox"][0][1] for g in geo)
        under = max((g for g in geo if abs(g["bbox"][1][1] - ymin) < 1e-3),
                    key=lambda g: g["bbox"][1][0] - g["bbox"][0][0])
        top = max(geo, key=lambda g: g["bbox"][1][1])
        for kind_, refs in (("distance_y", [ref(under),
                                            ref(top, extreme="top")]),
                            ("distance_x", [ref(under, end=1),
                                            ref(under, end=2)])):
            try:
                app.call("add_drawing_dim", view=vid, kind=kind_, refs=refs)
            except Exception as e:
                print(f"  {kind} {kind_}: {e}")
    app.call("add_drawing_note",
             text="Weight: 12 mm steel, press fit P7/h9",
             position=[110.0, 60.0])
    app.call("add_drawing_note", text="Every size follows king_h (85)",
             position=[110.0, 52.0])
    chk = app.call("check_drawing")
    for ext in ("pdf", "svg"):
        app.call("export_drawing", path=str(OUT / f"pieces_drawing.{ext}"))
    return views, chk


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    args = ap.parse_args()
    app = connect(args.port)
    app.call("open_project", path=str(OUT / "set.dsgn"))
    report = {"fits": fits(app)}
    asm, placed = assemble(app)
    report["occurrences"] = len(placed)
    bom = app.call("bom")
    report["bom"] = bom
    # one line per rank and wood, all of them the piece part
    rows = {f"{r['part']} ({r['configuration']}) {r['material']}":
            r["quantity"] for r in bom["rows"]}
    print("BOM:", json.dumps(rows))
    assert len(rows) == 13 and sum(rows.values()) == 33, rows
    app.call("export_bom", path=str(OUT / "chess_bom.csv"))
    asm.render(str(OUT / "chess_set.png"), view="iso")
    try:
        views, chk = drawing(app)
        report["drawing"] = {"views": views, "check": chk}
        print("drawing:", json.dumps(chk)[:600])
    except Exception as e:
        report["drawing"] = f"FAILED: {e}"
        print("drawing failed:", e)
    app.save_project(path=str(OUT / "set.dsgn"))
    (OUT / "set_report.json").write_text(json.dumps(report, indent=1,
                                                     default=str))


if __name__ == "__main__":
    main()
