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
    """
    # --- rank 0: the lower rails, the spine of the car -------------------
    # They run unbroken from the bulkhead to the back of the engine bay, and
    # everything else lands on them — including the hoops, whose feet are
    # welded to these. Rank is the order a frame is tacked up in: what runs
    # through is primary, what stops against it is coped.
    f.pair("Lower rail", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD_SUPPORT, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_FLOOR),
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

    # --- rank 2: what lands on the hoops and the rails -------------------
    # The upper side impact member runs at the driver's elbow, from the
    # bulkhead's top corner back to the main hoop.
    f.pair("Side upper", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_SIDE_UPPER),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
    ], S.STRUCT, rank=2)

    # --- rank 2: the front bulkhead, a square of tube --------------------
    f.pair("Bulkhead upright", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
    ], S.STRUCT, rank=2)
    f.add("Bulkhead top", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_BULKHEAD, -S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
    ], S.STRUCT, rank=3)

    # --- rank 3: the bracing ---------------------------------------------
    # Main hoop bracing: the rulebook's reason the hoop stays up in a roll.
    f.pair("Main hoop brace", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_MH_BRACE),
        (S.X_MH_BRACE_FOOT, S.W_REAR, S.Z_FLOOR),
    ], S.STRUCT, rank=3)

    # The middle side impact member, and the diagonals that turn the side
    # into triangles rather than a parallelogram.
    f.pair("Side middle", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_SIDE_MID),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_MID),
    ], S.STRUCT, rank=3)
    f.pair("Side diagonal", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
    ], S.SMALL, rank=4)

    # The bulkhead support: the triangulated cage that stops the nose
    # folding back into the driver's feet.
    f.pair("Bulkhead support upper", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_BULKHEAD_SUPPORT, S.W_FRONT_HOOP, S.Z_SIDE_UPPER),
    ], S.SMALL, rank=3)
    f.pair("Bulkhead support diagonal", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD_SUPPORT, S.W_FRONT_HOOP, S.Z_SIDE_UPPER),
    ], S.SMALL, rank=4)

    # --- the front hoop and main hoop tie together -------------
    f.pair("Front hoop brace", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FH_BRACE),
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
    ], S.SMALL, rank=4)

    # --- the rear bay, which carries the engine ------------------
    f.add("Rear top cross", [
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
        (S.X_REAR, -S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=4)
    f.pair("Rear upright", [
        (S.X_REAR, S.W_REAR, S.Z_FLOOR),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=3)
    f.pair("Engine bay upper", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=3)
    f.pair("Engine bay diagonal", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_FLOOR),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=4)

    # --- rank 2: the cross members that keep it square -------------------
    f.add("Front cross", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_FRONT_HOOP, -S.W_FRONT_HOOP, S.Z_FLOOR),
    ], S.SMALL, rank=3)
    f.add("Seat back cross", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_FLOOR),
    ], S.SMALL, rank=3)
    f.add("Rear bottom cross", [
        (S.X_REAR, S.W_REAR, S.Z_FLOOR),
        (S.X_REAR, -S.W_REAR, S.Z_FLOOR),
    ], S.SMALL, rank=3)
    f.add("Main hoop diagonal", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_MH_BRACE),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_SIDE_UPPER),
    ], S.SMALL, rank=3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", default=None, help="where the order is written")
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

    # A long question, and it says so: 32 members is 496 pairs, of which the
    # prefilters leave 100 for an exact boolean apiece. About 70 s, which is
    # over the client's default patience — so ask for more rather than
    # pretending the answer is quick.
    contact = d.request("contact_report", {}, timeout=600.0)
    comps = contact.get("components", [])
    print(f"contact_report: {len(comps)} connected component(s), "
          f"{len(comps[0]) if comps else 0} members in the largest, "
          f"{len(contact.get('clashes', []))} overlaps — and an overlap "
          f"between two members IS the weld")

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
