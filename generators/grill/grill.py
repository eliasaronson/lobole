# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Build the whole grill against a running Designer instance.

    python3 generators/grill/grill.py --port 8800 [--only firebox]

Always starts from a new project: a half-built tab from a failed run is a
different model from the one this script describes.

The order is not arbitrary. Some parts are measured off others — the lid's
end caps off the facets that actually landed on the rim, the lid's hinge ear
off the facet's own plane, the grease tray off the rail it slides on — so
each of those is built after the thing it is fitted to, and the numbers come
from the app rather than from a second copy of the arithmetic.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
from _lib import connect  # noqa: E402

import grill_assemble as assemble  # noqa: E402
import grill_cart as cart  # noqa: E402
import grill_firebox as firebox  # noqa: E402
import grill_hardware as hw  # noqa: E402
import grill_shop as shop  # noqa: E402
import grill_spec as S  # noqa: E402


def firebox_stage(d, ctx):
    # The tent FIRST: the firebox's sockets for it are where its legs land,
    # and that is measured off the tent rather than worked out twice.
    yield "Heat tent", lambda: ctx.update(firebox.heat_tent(d))
    yield "Firebox", lambda: firebox.firebox_pan(d, ctx["tent_drop"])
    yield "Hood", lambda: ctx.update(firebox.hood(d))
    yield "Hood end cap", lambda: firebox.hood_cap(d, ctx["drop"])
    yield "Lid ear", lambda: firebox.lid_ear(d, ctx["facet"])
    yield "Hinge ear", lambda: firebox.box_ear(d)
    yield "Grate ledge", lambda: firebox.grate_ledge(d)
    yield "Tent ledge", lambda: firebox.tent_ledge(d)
    yield "Rack ledge", lambda: firebox.rack_ledge(d)
    yield "Tray rail", lambda: tray_rail(d, ctx)
    yield "Grease tray", lambda: firebox.grease_tray(d)


def tray_rail(d, ctx):
    """The rail, and the socket for the tray that slides on it.

    Where the tray's floor sits is the rail's own lowest lip, which depends
    on two bend allowances — so it is measured off the part and written back
    onto the firebox as the socket the tray is mated to.
    """
    firebox.tray_rail(d)
    # The LIP's top face, which is what the tray rests on: the lowest of the
    # rail's upward-facing faces. Its own bottom is a thickness lower and the
    # web's end lower still, and taking either left the tray hanging 1.5 mm
    # in the air — a gap contact_report reports and nothing else would.
    ups = [f for f in d.call("list_elements")["bodies"][0]["faces"]
           if f["kind"] == "plane" and f["normal"][2] > 0.99]
    lip = min(f["centroid"][2] for f in ups)
    ctx["tray_floor"] = S.FB_Z - S.T_BRK + lip
    d.call("set_active_part", part="Firebox")
    from grill_lib import frame
    frame(d, "tray", (0.0, S.TRAY_CENTRE_Y, ctx["tray_floor"]))
    d.call("set_active_part", part="Tray rail")


def cart_stage(d, ctx):
    yield "Cart side left", lambda: cart.cart_side(d, -1)
    yield "Cart side right", lambda: cart.cart_side(d, +1)
    yield "Cart back", lambda: cart.cart_back(d)
    yield "Cart shelf", lambda: cart.cart_shelf(d)
    yield "Control panel", lambda: cart.control_panel(d)
    yield "Cart door left", lambda: cart.cart_door(d, -1)
    yield "Cart door right", lambda: cart.cart_door(d, +1)
    yield "Side shelf", lambda: cart.side_shelf(d)


def hardware_stage(d, ctx):
    for name, fn in (("Burner", hw.burner), ("Manifold", hw.manifold),
                     ("Valve", hw.valve), ("Knob", hw.knob),
                     ("Cooking grate", hw.grate),
                     ("Warming rack", hw.warming_rack), ("Wheel", hw.wheel),
                     ("Axle bolt", hw.axle_bolt), ("Foot", hw.foot),
                     ("Propane tank", hw.tank), ("Regulator", hw.regulator),
                     ("Lid handle", hw.lid_handle),
                     ("Thermometer", hw.thermometer),
                     ("Lid hinge pin", hw.lid_pin),
                     ("Door hinge pin", hw.door_pin),
                     ("Hinge leaf", hw.hinge_leaf)):
        yield name, (lambda f=fn: f(d))


STAGES = {"firebox": firebox_stage, "cart": cart_stage,
          "hardware": hardware_stage}


def joined(d):
    """The whole point of the round: one structure, and nothing inside
    anything else."""
    contact = d.call("contact_report", deadline_s=240.0, max_pairs=40000,
                     touch_tol=S.SEAT_TOL, near_tol=1.0)
    parts = len(contact.get("components", []))
    print(f"  contact: {parts} connected component(s), "
          f"{len(contact.get('clashes', []))} clashes, "
          f"{len(contact.get('near_misses', []))} near misses")
    for c in contact.get("clashes", [])[:12]:
        print(f"    CLASH {c['a']} / {c['b']}  {c['volume']:.1f} mm3")
    if parts > 1:
        for comp in contact["components"][:20]:
            print(f"    island: {comp}")
    for n in contact.get("near_misses", [])[:12]:
        print(f"    near   {n['a']} / {n['b']}  {n['distance']:.2f} mm")
    fast = d.call("check_fasteners")
    print(f"  fasteners: examined {fast['examined']}, "
          f"skipped {fast.get('skipped', 0)}, findings {fast['findings']}")
    return contact


def recover(d, ctx):
    """Measure what the assembly needs but this run did not build.

    Assembling into a project whose parts are already there is how a round
    like this is iterated, and the two numbers the assembly takes from the
    parts — where the lid's flat ended up, and how low the tray rail's lip
    is — are in the model, so they are read rather than assumed.
    """
    if "z_flat" not in ctx:
        d.call("set_active_part", part="Hood")
        sk = next(f for f in d.call("get_state")["features"]
                  if f.get("type") == "Sketch")
        ctx["z_flat"] = sk["plane"]["origin"][2]
    if "tray_floor" not in ctx:
        d.call("set_active_part", part="Tray rail")
        ctx["tray_floor"] = S.FB_Z + d.call("get_state")["bodies"][0][
            "bbox_min"][2]


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

    stages = [s for s in (args.only or list(STAGES)) if s in STAGES]
    unknown = [s for s in args.only
               if s not in STAGES and s not in ("assembly", "shop")]
    if unknown:
        raise SystemExit(f"unknown stage {unknown}; pick from "
                         f"{list(STAGES) + ['assembly', 'shop']}")
    ctx = {}
    made = []
    for stage in stages:
        for name, fn in STAGES[stage](d, ctx):
            t0 = time.time()
            fn()
            body = d.call("get_state")["bodies"][0]
            made.append((name, body["volume"], time.time() - t0))
            print(f"  {name:18s} {body['volume']:10.1f} mm3  "
                  f"{time.time() - t0:5.1f}s")
    if not args.only or "assembly" in args.only:
        recover(d, ctx)
        a = assemble.assemble(d, ctx)
        print(f"  assembled {len(a.occ)} placed parts")
        joined(d)
    if not args.only or "shop" in args.only:
        shop.shop(d)
    total = sum(v for _, v, _ in made)
    print(f"{len(made)} parts, {total / 1000.0:.1f} cm3 of metal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
