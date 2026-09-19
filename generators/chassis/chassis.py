"""A Formula Student chassis: a steel space frame, and the order to make it.

    python3 generators/chassis/chassis.py --port <port>

Builds the frame as ONE part of many bodies — one body per member, because a
member is what gets cut, bent, labelled and welded — then asks the app the
questions a shop asks: what do I cut, what do I bend it round, and is any of
it impossible.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))

import chassis_spec as S          # noqa: E402
from chassis_frame import Frame   # noqa: E402
from designer_remote import Designer  # noqa: E402


def members(f):
    """Every member of the frame, by name, in the order a frame is built.

    Rank 0 is primary (coped to nothing), rank 1 lands on rank 0, and so on.

    The governing idea is that every BAY is a triangle. A quadrilateral bay
    carries its load by bending its welds, which is the one thing a space
    frame exists not to do — so each station (bulkhead, nose ring, front
    hoop, main hoop, rear) is a ring, and every face between two rings gets
    a diagonal: the two sides, the floor and the roof.
    """
    # --- rank 0: the lower rails, the spine of the car -------------------
    # They run unbroken from the bulkhead to the back of the engine bay, and
    # everything else lands on them — including the hoops, whose feet are
    # welded to these. Rank is the order a frame is tacked up in: what runs
    # through is primary, what stops against it is coped.
    # It turns at the TAPER stations, not at the hoops: a station is where
    # five or six members converge, and a bend is the one stretch of a tube
    # with no straight to fit them against.
    f.pair("Lower rail", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_TAPER_FRONT, S.W_TAPER_FRONT, S.Z_FLOOR),
        (S.X_TAPER_REAR, S.W_TAPER_REAR, S.Z_FLOOR),
        (S.X_REAR, S.W_REAR, S.Z_FLOOR),
    ], S.STRUCT, rank=0)

    # --- rank 1: the hoops, each bent from ONE length of tube -------------
    # A main hoop is one tube: up one side, over the top on two big-radius
    # bends, and down the other. That is what makes it a hoop and not three
    # tubes welded into the shape of one.
    f.add("Main hoop", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_MAIN_HOOP_TOP),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_MAIN_HOOP_TOP),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_FLOOR),
    ], S.HOOP, rank=1, former=S.HOOP_FORMER)

    f.add("Front hoop", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FRONT_HOOP_TOP),
        (S.X_FRONT_HOOP, -S.W_FRONT_HOOP, S.Z_FRONT_HOOP_TOP),
        (S.X_FRONT_HOOP, -S.W_FRONT_HOOP, S.Z_FLOOR),
    ], S.HOOP, rank=1, former=S.HOOP_FORMER)

    # --- rank 2: the rings, and the rails that run between them ----------
    # The upper side member runs at the driver's elbow, bulkhead to main
    # hoop, over the nose ring and the front hoop on its way.
    # It climbs to the KNEE and runs level from there, so the front hoop and
    # everything else at that station meets it on a straight.
    f.pair("Side upper", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_SIDE_KNEE, S.W_SIDE_KNEE, S.Z_SIDE_UPPER),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
    ], S.STRUCT, rank=2)

    # The front bulkhead: a square of tube, and the plate the impact
    # attenuator bolts to.
    f.pair("Bulkhead upright", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
    ], S.STRUCT, rank=2)
    f.add("Bulkhead top", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_BULKHEAD, -S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
    ], S.STRUCT, rank=3)
    # The bottom of the square. Without it the bulkhead is a U and the
    # attenuator plate has nothing to bolt to along its lower edge.
    f.add("Bulkhead bottom", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD, -S.W_BULKHEAD, S.Z_FLOOR),
    ], S.STRUCT, rank=3)
    f.add("Bulkhead diagonal", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD, -S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
    ], S.SMALL, rank=4)

    # The nose RING, which is what the nose has to be triangulated against:
    # without a station here the bulkhead-to-front-hoop bay is one long
    # unbraced box, which is exactly what it was.
    f.pair("Nose ring upright", [
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_FLOOR),
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_SUPPORT_TOP),
    ], S.SMALL, rank=3)
    f.add("Nose ring top", [
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_SUPPORT_TOP),
        (S.X_BULKHEAD_SUPPORT, -S.W_SUPPORT, S.Z_SUPPORT_TOP),
    ], S.SMALL, rank=4)

    # --- the nose bay: bulkhead to nose ring, triangulated on every face --
    f.pair("Nose side diagonal", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_SUPPORT_TOP),
    ], S.SMALL, rank=4)
    f.add("Nose roof diagonal", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_BULKHEAD_SUPPORT, -S.W_SUPPORT, S.Z_SUPPORT_TOP),
    ], S.SMALL, rank=5)
    f.add("Nose floor diagonal", [
        (S.X_BULKHEAD, -S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_FLOOR),
    ], S.SMALL, rank=5)

    # --- the front bay: nose ring to front hoop, the one that had NOTHING -
    f.pair("Front bay side diagonal", [
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_FLOOR),
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_SIDE_UPPER),
    ], S.SMALL, rank=4)
    f.add("Front bay roof diagonal", [
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_SUPPORT_TOP),
        (S.X_FRONT_HOOP, -S.W_FRONT_HOOP, S.Z_SIDE_UPPER),
    ], S.SMALL, rank=5)
    f.add("Front bay floor diagonal", [
        (S.X_BULKHEAD_SUPPORT, -S.W_SUPPORT, S.Z_FLOOR),
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
    ], S.SMALL, rank=5)

    # The front hoop is braced FORWARD and down, onto the nose ring — which
    # is what a rulebook asks for and what ties its top to the rest of the
    # car instead of leaving it standing there.
    f.pair("Front hoop brace", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FH_BRACE),
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_SUPPORT_TOP),
    ], S.STRUCT, rank=4)

    # --- the cockpit: front hoop to main hoop ----------------------------
    # The side impact structure: an upper member (above), a middle one at
    # the driver's hip, and the diagonals that make the side a truss rather
    # than a parallelogram.
    f.pair("Side middle", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_SIDE_MID),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_MID),
    ], S.STRUCT, rank=3)
    f.pair("Side diagonal front", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_MID),
    ], S.SMALL, rank=4)
    f.pair("Side diagonal rear", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_SIDE_MID),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
    ], S.SMALL, rank=4)
    # And the floor, which is a face like any other.
    f.add("Cockpit floor diagonal", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_FLOOR),
    ], S.SMALL, rank=4)

    # --- the main hoop and its bracing -----------------------------------
    # The braces attach as high as the bend allows and run back to the rear
    # of the car: the rulebook's reason the hoop stays up in a roll.
    f.pair("Main hoop brace", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_MH_BRACE),
        (S.X_MH_BRACE_FOOT, S.W_REAR, S.Z_FLOOR),
    ], S.STRUCT, rank=3)
    f.add("Main hoop diagonal", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_MH_BRACE),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
    ], S.SMALL, rank=3)
    # The harness bar: what the belts pull against, and a second tie across
    # the hoop's braced height.
    f.add("Harness bar", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
    ], S.STRUCT, rank=3)
    # The brace pair is a triangle of its own, not two struts side by side.
    f.add("Brace cross", [
        (S.X_MH_BRACE_FOOT, S.W_REAR, S.Z_FLOOR),
        (S.X_MH_BRACE_FOOT, -S.W_REAR, S.Z_FLOOR),
    ], S.SMALL, rank=4)

    # --- the rear bay, which carries the engine --------------------------
    f.pair("Rear upright", [
        (S.X_REAR, S.W_REAR, S.Z_FLOOR),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=3)
    f.add("Rear top cross", [
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
        (S.X_REAR, -S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=4)
    f.add("Rear bottom cross", [
        (S.X_REAR, S.W_REAR, S.Z_FLOOR),
        (S.X_REAR, -S.W_REAR, S.Z_FLOOR),
    ], S.SMALL, rank=4)
    f.add("Rear bulkhead diagonal", [
        (S.X_REAR, S.W_REAR, S.Z_FLOOR),
        (S.X_REAR, -S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=5)
    f.pair("Engine bay upper", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=4)
    f.pair("Engine bay diagonal", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_FLOOR),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=5)
    # The bay's ROOF was a plain quadrilateral, and its sides carried one
    # diagonal apiece. It holds the engine and the rear suspension.
    f.add("Engine bay roof diagonal", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
        (S.X_REAR, -S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=5)
    f.add("Engine mount cross", [
        (S.X_MH_BRACE_FOOT, S.W_REAR, S.Z_SIDE_MID),
        (S.X_MH_BRACE_FOOT, -S.W_REAR, S.Z_SIDE_MID),
    ], S.SMALL, rank=4)
    f.pair("Engine mount stay", [
        (S.X_MH_BRACE_FOOT, S.W_REAR, S.Z_SIDE_MID),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=5)
    f.add("Engine bay floor diagonal", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_FLOOR),
        (S.X_REAR, -S.W_REAR, S.Z_FLOOR),
    ], S.SMALL, rank=5)

    # --- the cross members that keep the stations square -----------------
    f.add("Front cross", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_FRONT_HOOP, -S.W_FRONT_HOOP, S.Z_FLOOR),
    ], S.SMALL, rank=3)
    f.add("Seat back cross", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_FLOOR),
    ], S.SMALL, rank=3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", default=None, help="where the order is written")
    ap.add_argument("--contact", action="store_true",
                    help="run the exact contact sweep — it answers whether "
                         "the frame is ONE connected thing, and on 47 members "
                         "that is 1081 pairs and about four minutes")
    args = ap.parse_args()
    d = Designer(port=args.port)
    t0 = time.time()

    d.call("new_project")
    d.call("add_part", name="Chassis")
    d.call("set_active_part", part="Chassis")
    # Declare the stock and the bender BEFORE building, so every question
    # asked afterwards is asked against a real shop.
    # The SHOP, not the stock: the material and the bender's dies. Each
    # member carries its own size, because a chassis is three of them.
    d.call("set_tube_rules", material=S.MATERIAL, former_radii_mm=S.FORMERS)

    f = Frame(d)
    members(f)
    print(f"{len(f.members)} members")
    copes = f.build()
    print(f"{copes} coped members, {time.time() - t0:.1f}s")

    bodies = d.call("get_state")["bodies"]
    print(f"{len(bodies)} bodies")
    for i in range(len(bodies)):
        d.call("set_material", body=i, material=S.MATERIAL)

    # --- what a shop asks -------------------------------------------------
    print("\n-- is it sound --")
    geo = d.call("check_geometry")
    bad = [b for b in geo["bodies"] if not b["valid"]]
    print(f"check_geometry: {len(geo['bodies'])} bodies, "
          f"{len(bad)} invalid" + (f" {[b['name'] for b in bad]}" if bad else ""))

    # The exact sweep answers the question that matters — is this ONE
    # connected thing — and it is expensive: 47 members is 1081 pairs, and
    # about four minutes. Opt in, rather than making every run pay for it.
    if args.contact:
        contact = d.request("contact_report", {}, timeout=1800.0)
        comps = contact.get("components", [])
        print(f"contact_report: {len(comps)} connected component(s), "
              f"{len(comps[0]) if comps else 0} members in the largest, "
              f"{len(contact.get('clashes', []))} overlaps — and an overlap "
              f"between two members IS the weld")
    else:
        print("contact_report: skipped (--contact runs it; ~4 min at this "
              "size). The cheap half of the same question is below: every "
              "cope REFUSES unless it actually cut metal, so a member that "
              "reaches nothing cannot build at all.")

    print("\n-- is it a truss --")
    fr = d.request("check_frame", {}, timeout=600.0)
    kinds = {}
    for v in fr["violations"]:
        kinds[v["kind"]] = kinds.get(v["kind"], 0) + 1
    print(f"check_frame: {fr['members_checked']} members, "
          f"{len(fr['violations'])} violations "
          + (", ".join(f"{k} {n}" for k, n in sorted(kinds.items()))
             if kinds else "(none)"))
    for v in fr["violations"][:3]:
        print(f"  [{v['kind']}] {v['message'][:110]}")
    for n in fr["not_checked"]:
        print(f"  not checked: {n[:100]}")

    print("\n-- can it be bent --")
    chk = d.call("check_tube")
    print(f"check_tube: {chk['members_checked']} members, "
          f"{chk['bends_checked']} bends, "
          f"{len(chk['violations'])} violations")
    for v in chk["violations"][:8]:
        print(f"  [{v['kind']}] {v['message']}")
    for n in chk["not_checked"]:
        print(f"  not checked: {n}")

    print("\n-- what to order --")
    sched = d.request("tube_schedule", {})
    print(f"tube_schedule: {sched['count']} distinct rows, "
          f"{sched['members']} members")
    mass = d.call("mass_properties")
    print(f"frame mass: {mass['total']['mass_g'] / 1000.0:.1f} kg, "
          f"centre of mass at x={mass['total']['center_of_mass'][0]:.0f} mm")

    out = args.out or os.path.join(HERE, "..", "..", "scratch")
    os.makedirs(out, exist_ok=True)
    cut = d.call("export_cut_list",
                 path=os.path.join(out, "chassis_cut_list.csv"),
                 stock_length_mm=S.STOCK_LENGTH, kerf_mm=S.KERF)
    print(f"cut list: {cut['rows']} rows, {cut['members']} members, "
          f"{len(cut['sticks'])} lengths of {S.STOCK_LENGTH:.0f} mm stock, "
          f"{cut['offcut_mm'] / 1000.0:.2f} m of offcut")
    bends = d.call("export_bend_schedule",
                   path=os.path.join(out, "chassis_bends.csv"))
    print(f"bend schedule: {bends['bends']} bends over {bends['rows']} rows")
    print(f"written to {out}")
    return d, f, args


if __name__ == "__main__":
    main()
