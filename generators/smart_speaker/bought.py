"""The parts nobody here makes: two drivers, the amplifier, the power brick.

They arrive as STEP from `scratch/smart_speaker/bought/`, which
`extract_bought.py` slices out of the 48 MB Onshape export once. Importing
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
    the amplifier and the supply — standing on the cabinet floor at the back
                  of the port bay, clear of the tube

The last of those is the one place this generator invents a position. The
export puts the supply at (-84.7, 130.1, -933.1) and the amplifier somewhere
the group export did not preserve, so both are placed by a rule instead of
transcribed. They are bought boxes in an empty bay; if they need to be
somewhere exact, move the occurrence and it will stay moved.
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
            f"Onshape export")
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
        # Standing ON the bay floor, fore and aft of the port rather than
        # beside it: the tube is 105 across on the cabinet's axis and the bay
        # is only 210 wide, so there is no room either side, which is how
        # both of these came to be inside the floor and inside the tube.
        # Placed by their middles, because an import is centred on its origin.
        ("amp module", "amp module", (0.0, -110.0, floor + 36.0), None),
        ("PSU brick", "PSU brick", (0.0, 120.0, floor + 19.3),
         (0.0, 0.0, 0.7071067811865476, 0.7071067811865476)),
    ]
