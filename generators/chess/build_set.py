#!/usr/bin/env python3
"""The set assembled, its bill of materials, and the pieces' drawing.

    python3 generators/chess/build_set.py --port N

Opens scratch/chess/set.dsgn (build_board.py): places the board and the 32
pieces in the starting position, checks the BOM and the FIDE fits, draws the
six ranks on one sheet with the weight bore's fit, and saves set.dsgn.
"""
import argparse
import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What the run makes -- projects, programs, renders, reports -- is
# regenerable, so it goes to the untracked scratch/, not beside the code.
OUT = HERE.parent.parent / "scratch" / "chess"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(HERE.parent.parent / "tools"))
sys.path.insert(0, str(HERE))
from designer_sdk import connect  # noqa: E402
from build_piece import SIZES, ORDER  # noqa: E402
from build_board import SLAB_T, KING_H  # noqa: E402

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
                r = app.call("add_occurrence", part=f"{kind}_{wood}",
                             name=name,
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
    # read on the part's own tab: list_configurations answers for the
    # active one only
    app.part("piece", create=False).activate()
    piece_cfgs = {c["name"]: c["id"] for c in app.call(
        "list_configurations")["configurations"]}
    d = app.call("create_drawing", name="Pieces",
                 sheet={"size": "a3", "landscape": True,
                        "title_block": {"project": "Chess set",
                                        "document": "Turned pieces",
                                        "author": "Designer dogfood",
                                        "notes": "king_h = 85; every size "
                                                 "follows it"}})
    views = {}
    # The pieces lie along X, so each view is as wide as its piece is tall:
    # two columns of three at 1:1, each view clear of the next by room for
    # its dimensions.
    slots = [(40.0, 245.0), (40.0, 165.0), (40.0, 90.0),
             (230.0, 245.0), (230.0, 165.0), (230.0, 90.0)]
    for kind, pos in zip(["king", "queen", "bishop", "knight", "rook",
                          "pawn"], slots):
        v = app.call("add_drawing_view", source="piece", orientation="front",
                     scale=1.0, position=list(pos),
                     configuration=piece_cfgs[kind])
        views[kind] = v["id"]
    app.call("update_drawing", budget_s=120)
    # Each view's height and base: the underside is its longest line at the
    # low X end, the top whatever reaches furthest along X.
    for kind, vid in views.items():
        # edges and silhouettes both: a turned part's outline in a side view
        # is mostly silhouette. A silhouette is picked as `sil_face`.
        geo = [g for g in app.call("list_drawing_geometry", view=vid)
               ["geometry"] if g.get("visible") and "p1" in g
               and g.get("kind") in ("edge", "silhouette")]
        # Each extreme is an edge's END, picked as such: a distance from an
        # edge alone measures from its midpoint, and the edge that reaches
        # the top is often a slope that only ends there.
        ends = [(g, k, g["p1" if k == 0 else "p2"]) for g in geo
                for k in (0, 1)]
        ref = lambda g, k: {("sil_face" if g["kind"] == "silhouette"
                             else "edge"): g["ref"],
                            "occurrence": g["source_id"], "end": k + 1}
        xmin = min(p[0] for _, _, p in ends)
        base = [(g, k, p) for g, k, p in ends if abs(p[0] - xmin) < 1e-3]
        lo = min(base, key=lambda e: e[2][1])
        hi = max(base, key=lambda e: e[2][1])
        top = max(ends, key=lambda e: e[2][0])
        for kind_, refs in (("distance_x", [ref(*lo[:2]), ref(*top[:2])]),
                            ("distance_y", [ref(*lo[:2]), ref(*hi[:2])])):
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
    rows = {r.get("name") or r.get("part"): r.get("quantity") or r.get("qty")
            for r in bom.get("rows", bom.get("items", []))}
    print("BOM:", json.dumps(rows))
    try:
        app.call("export_bom", path=str(OUT / "chess_bom.csv"))
    except Exception as e:
        print("export_bom:", e)
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
