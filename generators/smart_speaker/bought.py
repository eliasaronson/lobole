"""The parts nobody here makes: two drivers, the amplifier, the power brick.

They arrive as STEP from `scratch/smart_speaker/bought/`, which
`extract_bought.py` slices out of the original model's 48 MB STEP export
once. Importing
them is under a second; importing the source would be 27.5 s and 301 tabs.

`import_step` re-centres what it brings in: every one of these arrives with
its bounding box about the part origin, whatever the source assembly said.
Placing them all at the identity therefore piles the drivers, the amplifier
and the supply on top of each other at the cabinet's top corner — 47 clashing
pairs, which is how this was noticed.

So each one is placed here, and placed against the thing that FIXES it rather
than at a coordinate copied from the export:

    the drivers   flange face down in their rebate, on their own axis height
    the flares    by port.py, which owns the port's stack
    the amplifier lying on the bay floor, bolted through the four mounting
                  holes its own case is bored with
    the supply    lying beside it, over two holes drilled for it — its case
                  has none in the face it stands on

Only the last invents anything, and only the hole spacing: the export puts
the supply at (-84.7, 130.1, -933.1) and the amplifier somewhere the group
export did not preserve, so which strip of floor each lies on is a rule here
(`amp_y`, `psu_y`) rather than a transcription. Everything after that is
mates — see mates.py.

The positions in `placements` are only where each occurrence STARTS. The
mates move them onto their bolts, so a number here being 6 mm out changes
nothing; a number here being on the wrong side of the port changes which
solution the mate settles into, which is why they are not arbitrary either.
"""
from pathlib import Path

from designer_sdk import SdkError

HERE = Path(__file__).resolve().parent
BOUGHT = HERE.parent.parent / "scratch/smart_speaker/bought"
REFERENCE = HERE.parent.parent / "scratch/smart_speaker/reference"


def _source(stem):
    """The flares live under reference/ because they started out as something
    to check a revolve against, and ended up being the part itself."""
    here = BOUGHT / f"{stem}.step"
    return here if here.exists() else REFERENCE / f"{stem}.step"

PARTS = [
    ("w170s", "W170S woofer", "Visaton W 170 S, four solids"),
    ("port_flare_top", "port_flare_top", "moulded flare, into the bay"),
    ("port_flare_bottom", "port_flare_bottom", "moulded flare, on the floor"),
    ("alpair7ms", "Alpair 7MS", "Markaudio Alpair 7 MS"),
    ("amp_module", "amp module", "ESP32 amplifier: case halves and board"),
    ("psu", "PSU brick", "the supply that feeds it"),
]


def missing():
    return [f"{stem}.step" for stem, _, _ in PARTS
            if not _source(stem).exists()]


def import_all(app, p):
    gone = missing()
    if gone:
        raise SdkError(
            f"no bought geometry: {', '.join(gone)} — run\n"
            f"    python3 generators/smart_speaker/extract_bought.py\n"
            f"once against {BOUGHT.parent.name}/ to slice them out of the "
            f"original model's STEP export")
    made = []
    for stem, name, _what in PARTS:
        part = app.part(name)
        part.activate()
        app.import_step(path=str(_source(stem)), as_assembly=False)
        made.append(part)
    return made


def _driver(p, axis_z, flange_depth, half_depth):
    """A driver's centre: on the cabinet's centre line, at its own axis
    height, and far enough back that its flange face lands on the rebate.

    The rebate floor is `flange_depth` behind the finished baffle surface, so
    a driver `2 * half_depth` deep sits with its middle `half_depth` behind
    that.
    """
    finished = -(float(p["cabinet_internal_d"]) / 2
                 + float(p["wall_thickness"])
                 + float(p["finish_wall_thickness"]))
    # Flush with the finished surface, not recessed behind it: the rebate
    # takes the flange's THICKNESS, it does not push the driver back. Seated
    # `flange_depth` deeper, the basket ploughs 73,960 mm3 into the baffle.
    return (0.0, finished + float(half_depth), float(axis_z))


def placements(p):
    """Where each bought part goes. The flares are absent on purpose — the
    port's three pieces are one stack and `port.placements` owns all of it."""
    carcass = float(p["carcass_h"])
    floor = -carcass + float(p["wall_thickness"])       # inside the bottom
    back = float(p["cabinet_internal_d"]) / 2

    w170s = _driver(p, -carcass + float(p["w170s_center_from_floor"]),
                    p["w170s_flange_depth"], 42.5)
    alpair = _driver(p, -float(p["alpair_center_from_top"]),
                     p["alpair_flange_depth"], 31.05)
    return [
        ("W170S woofer", "W170S woofer", w170s, None),
        ("Alpair 7MS", "Alpair 7MS", alpair, None),
        # Lying ON the bay floor, over the bolts drilled for them, fore and
        # aft of the port: the tube is 105 across on the cabinet's axis and
        # the bay only 210 wide, so there is no room either side — which is
        # how both of these first came to be inside the floor and inside the
        # tube. `amp_y`/`psu_y` are the same parameters the floor is drilled
        # from, so a unit and its holes cannot drift apart.
        # Placed by their middles, because an import is centred on its origin.
        # Both lie flat, long side across the cabinet: a quarter turn about
        # X lays the amp's case down (its 37 mm is along its own Y), and one
        # about Z turns the supply's 142 mm across. The mates would get them
        # there from anywhere, but a start already the right way round is
        # the difference between one candidate tried and eight.
        ("amp module", "amp module",
         (0.0, float(p["amp_y"]), floor + 18.5),
         (-0.7071067811865476, 0.0, 0.0, 0.7071067811865476)),
        ("PSU brick", "PSU brick", (0.0, float(p["psu_y"]), floor + 19.3),
         (0.0, 0.0, 0.7071067811865476, 0.7071067811865476)),
    ]
