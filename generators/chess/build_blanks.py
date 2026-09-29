#!/usr/bin/env python3
"""The blanks the rotary turns: pieces strung along X on necks.

    python3 generators/chess/build_blanks.py --port N

Opens scratch/chess/piece.dsgn (build_piece.py), adds one part per blank and
saves scratch/chess/blanks.dsgn.
Each is Derived copies of the `piece` part, each in its own configuration,
placed base-first along +X from the chuck, joined by necks the operator saws
through; a square grip for the chuck at x < 0 and a stub for the tailstock's
centre past the last piece.

The pieces' undersides (felt recess, weight bore) are cut later in the base
jig: the rotary cannot reach an end face. So each piece's underside is PLUGGED
here, to the depth of its bore -- the blank is the piece as turned.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What the run makes -- projects, programs, renders, reports -- is
# regenerable, so it goes to the untracked scratch/, not beside the code.
OUT = HERE.parent.parent / "scratch" / "chess"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(HERE.parent.parent / "tools"))
sys.path.insert(0, str(HERE))
from designer_sdk import connect, Plane  # noqa: E402
from build_piece import SIZES, SLUG_D  # noqa: E402

# Which pieces each blank carries, chuck end first. A blank's length stays
# under the rotary's chuck-to-centre capacity (ROTARY_LEN).
BLANKS = {
    "blank_pawns": ["pawn", "pawn", "pawn", "pawn"],
    "blank_rook_bishop": ["rook", "bishop"],
    "blank_queen_king": ["queen", "king"],
    "blank_knights": ["knight", "knight"],
}
# Per wood: 2 x pawns, 2 x rook+bishop, 1 x queen+king, 1 x knights.
BLANK_COUNT = {"blank_pawns": 2, "blank_rook_bishop": 2,
               "blank_queen_king": 1, "blank_knights": 1}
# The radius a neck may have where it leaves the top of each piece, in mm at
# the nominal 85 mm king: the pawn's ball and the rook's flat take the full
# neck; the bishop's and queen's finials, the knight's ears and the king's
# cross take one that fits inside them. A piece with a fine top goes LAST in
# its blank, where the tailstock carries it.
TOP_NECK_R = {"pawn": 4.25, "rook": 4.25, "knight": 2.5, "bishop": 1.9,
              "queen": 2.1, "king": 1.8}
EMBED = 1.5  # how far a neck runs into the top it leaves, mm
ROTARY_LEN = 300.0  # chuck face to tailstock centre, mm -- NOT MEASURED
GRIP = 20.0         # how much of the blank the chuck's jaws hold, mm
CHUCK_GAP = 14.0    # jaws to the first piece: clearance + a 6 mm cutter + slack
CHUCK_CLEAR = 6.5   # past the jaws no cutter reaches: its radius + 3 mm


def layout(pieces, king_h=85.0):
    """(x of each piece's underside, total model length) at this king_h."""
    neck_l = 0.07 * king_h
    # The first neck is longer: the chuck's jaws keep every cutter a
    # clearance and its own radius off their face, so a piece started one
    # neck from the jaws could not have its foot turned.
    x, out = CHUCK_GAP, []
    for kind in pieces:
        out.append(x)
        x += SIZES[kind][0] * king_h + neck_l
    stub_l = 0.12 * king_h
    return out, x + stub_l, neck_l, stub_l


def stock_side(pieces, king_h=85.0):
    """The square the blank is turned from: the widest base plus 2 mm."""
    widest = max(SIZES[k][1] * king_h for k in pieces)
    return float(int(widest + 2.0 + 0.999))


def build_blank(app, name, pieces, piece_part):
    part = app.part(name)
    part.activate()
    king_h = 85.0
    xs, length, neck_l, stub_l = layout(pieces, king_h)
    assert length + GRIP <= ROTARY_LEN, (name, length + GRIP)
    cfgs = {c["name"]: c["id"] for c in app.call(
        "list_configurations", part=piece_part)["configurations"]} \
        if False else None
    for i, (kind, x) in enumerate(zip(pieces, xs)):
        r = app.add_feature(feature={
            "type": "Derived", "name": f"{kind} {i + 1}",
            "source_part": piece_part, "configuration": kind,
            "reference": False,
            "placement": {"position": [x, 0, 0],
                          "orientation": [0, 0, 0, 1]}})
        assert r["regen"]["ok"], r["regen"]
    # One revolved profile for everything between and around the pieces:
    # the grip (a round stand-in inside the square the chuck holds), each
    # neck, each underside plug, and the tailstock stub. Separate loops,
    # joined to whatever they touch.
    u = king_h / 85.0
    sk = part.sketch(Plane.xz(), name="Necks, plugs and stub")
    prev_top, prev_r = 0.0, 0.05 * king_h  # the grip takes a full neck
    for kind, x in zip(pieces, xs):
        base_r = SIZES[kind][1] / 2 * king_h
        # The plug is the underside's two cuts filled, each to its own size
        # (build_piece.underside): the felt recess, 0.9 base radii across and
        # 0.05 deep, and the weight's bore, SLUG_D across and 0.7 deep, each
        # half a millimetre on into the wood so the union fuses. One cylinder
        # the recess's width down the bore's depth stood OUT of the piece
        # wherever the base narrows below 0.9 base radii -- a collar with two
        # square inside corners, on every piece of every blank, which the
        # finishing ball left as rings and the report read as patches.
        recess_r, recess_d = 0.9 * base_r, 0.05 * base_r + 0.5
        bore_r, bore_d = SLUG_D / 2, 0.7 * base_r + 0.5
        # neck from the previous top (or the grip) into this piece's plug
        pts = [(prev_top - EMBED, 0), (prev_top - EMBED, prev_r),
               (x, prev_r), (x, recess_r), (x + recess_d, recess_r),
               (x + recess_d, bore_r), (x + bore_d, bore_r),
               (x + bore_d, 0)]
        sk.polyline(pts, close=True)
        prev_top = x + SIZES[kind][0] * king_h
        prev_r = TOP_NECK_R[kind] * u
    sk.polyline([(prev_top - EMBED, 0), (prev_top - EMBED, prev_r),
                 (prev_top + neck_l + 0.5, prev_r),
                 (prev_top + neck_l + 0.5, 0)], close=True)
    s = sk.add()
    r = app.add_feature(feature={"type": "Revolve", "name": "Necks",
                                 "sketch_id": s.id, "axis": {"kind": "x"},
                                 "angle_deg": 360, "mode": "add"})
    side = stock_side(pieces, king_h)
    # Both ends stay the square they were bought as: the chuck's jaws, and
    # the clearance every cutter keeps from their face, at one end; the
    # tailstock's centre at the other. Modelled square, the check measures
    # what the machine can cut instead of calling the ends leftover.
    for name_, x0, lx in (("Grip", -GRIP, GRIP + CHUCK_CLEAR),
                          ("Stub", prev_top + neck_l, stub_l)):
        body = [b for b in part.bodies() if b.name not in ("Grip", "Stub")][0]
        box = part.box((x0, -side / 2, -side / 2), lx, side, side, name=name_)
        app.add_feature(feature={"type": "Boolean", "name": f"{name_} on",
                                 "target_id": body.created_by,
                                 "tool_id": box.created_by, "op": "union"})
    st = app.call("get_state")
    return part, st, side, length


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    args = ap.parse_args()
    app = connect(args.port)
    app.call("open_project", path=str(OUT / "piece.dsgn"))
    piece = app.part("piece", create=False)
    report = {}
    for name, pieces in BLANKS.items():
        part, st, side, length = build_blank(app, name, pieces, piece.id)
        bodies = st.get("bodies", [])
        print(f"{name}: {len(bodies)} bodies, regen ok {st['regen']['ok']}, "
              f"stock {side} square, model {length:.1f} mm + grip {GRIP}")
        for f in st["regen"].get("failed", []):
            print("   failed:", f)
        part.render(str(OUT / f"{name}.png"), view="iso")
        report[name] = {"bodies": len(bodies), "side": side,
                        "length": length}
    app.save_project(path=str(OUT / "blanks.dsgn"))
    (OUT / "blanks.json").write_text(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
