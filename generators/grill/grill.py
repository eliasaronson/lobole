"""Build the whole grill against a running Designer instance.

    python3 generators/grill/grill.py --port 8800 [--only firebox]

Always starts from a new project: a half-built tab from a failed run is a
different model from the one this script describes.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
from _lib import connect  # noqa: E402

import grill_cart as cart  # noqa: E402
import grill_firebox as firebox  # noqa: E402
import grill_assemble as assemble  # noqa: E402
import grill_hardware as hw  # noqa: E402
import grill_shop as shop  # noqa: E402
import grill_spec as S  # noqa: E402

STAGES = {
    "firebox": [
        ("Firebox", firebox.firebox_pan),
        ("Hood", firebox.hood),
        ("Hood end cap", firebox.hood_cap),
        ("Heat tent", firebox.heat_tent),
        ("Grease tray", firebox.grease_tray),
    ],
    "cart": [
        ("Control panel", cart.control_panel),
        ("Cart side", cart.cart_side),
        ("Cart back", cart.cart_back),
        ("Cart shelf", cart.cart_shelf),
        ("Cart door", cart.cart_door),
        ("Side shelf", cart.side_shelf),
    ],
    "hardware": [
        ("Burner", hw.burner),
        ("Manifold", hw.manifold),
        ("Valve", hw.valve),
        ("Knob", hw.knob),
        ("Cooking grate", hw.grate),
        ("Warming rack", hw.warming_rack),
        ("Wheel", hw.wheel),
        ("Axle", hw.axle),
        ("Foot", hw.foot),
        ("Propane tank", hw.tank),
        ("Regulator", hw.regulator),
        ("Lid handle", hw.lid_handle),
        ("Thermometer", hw.thermometer),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--keep", action="store_true",
                    help="build into the open project instead of a new one")
    args = ap.parse_args()
    d = connect(args.port)
    if not args.keep:
        d.call("new_project")

    # `assembly` and `shop` are stages too, handled below — asking for one of
    # them alone must not look like a typo for a part stage.
    stages = [s for s in (args.only or list(STAGES)) if s in STAGES]
    unknown = [s for s in args.only
               if s not in STAGES and s not in ("assembly", "shop")]
    if unknown:
        raise SystemExit(f"unknown stage {unknown}; pick from "
                         f"{list(STAGES) + ['assembly', 'shop']}")
    made = []
    for stage in stages:
        for name, fn in STAGES[stage]:
            t0 = time.time()
            fn(d)
            body = d.call("get_state")["bodies"][0]
            made.append((name, body["volume"], time.time() - t0))
            print(f"  {name:16s} {body['volume']:10.1f} mm3  "
                  f"{time.time() - t0:5.1f}s")
    if not args.only or "assembly" in args.only:
        occ = assemble.assemble(d)
        print(f"  assembled {len(occ['occurrences'])} occurrences")
        solved = d.call("solve_assembly")
        bad = [m for m in solved.get("mates", [])
               if m.get("status") not in ("satisfied", "redundant")]
        if bad:
            print("  MATES NOT SOLVED:", bad)
    if not args.only or "shop" in args.only:
        shop.shop(d)
    total = sum(v for _, v, _ in made)
    print(f"{len(made)} parts, {total / 1000.0:.1f} cm3 of metal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
