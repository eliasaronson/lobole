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
import chassis_jig                # noqa: E402
import chassis_panels             # noqa: E402
from chassis_frame import Frame   # noqa: E402
from designer_remote import Designer  # noqa: E402


def members(f):
    """Every member of the LFS19 frame, by name, in the order it is tacked.

    Rank 0 is primary (coped to nothing), rank 1 lands on rank 0, and so on.

    The shape is READ off a photograph of the car on its weld jig; the sizes
    are the rulebook's; the lengths are in chassis_spec.py with their
    provenance beside them. What makes this the LFS car rather than a
    textbook one is in three places: the hoops LEAN IN to a narrow crown,
    the rear is a truss that CONVERGES on a pair of node clusters rather
    than a box, and the main hoop is braced both forward and rearward with
    the braces triangulated to each other.
    """
    fh = lambda z: S.lean(S.FRONT_HOOP, z)   # noqa: E731 - half width at z
    mh = lambda z: S.lean(S.MAIN_HOOP, z)    # noqa: E731

    # --- rank 0: the lower rails, the spine of the car -------------------
    # Unbroken from the bulkhead to the front of the engine bay, with ONE
    # bend, at the front taper. Everything else lands on them, and both
    # hoops' feet sit on the straight stretch — which is why no joint in
    # this frame is on a bend.
    f.pair("Lower rail", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_TAPER_FRONT, S.W_TAPER_FRONT, S.Z_FLOOR),
        (S.X_ENGINE_FRONT, S.W_ENGINE, S.Z_FLOOR),
    ], S.STRUCT, rank=0)

    # --- rank 1: the hoops, each bent from ONE length of tube -------------
    # Vertical off the feet, then leaning inward to a crown half as wide.
    f.add("Main hoop", S.MAIN_HOOP, S.HOOP, rank=1, former=S.HOOP_FORMER)
    f.add("Front hoop", S.FRONT_HOOP, S.HOOP, rank=1, former=S.HOOP_FORMER)

    # --- rank 2: the front bulkhead --------------------------------------
    # Nearly square, and braced across its own opening: the photograph shows
    # a single diagonal inside the bulkhead frame.
    f.pair("Bulkhead upright", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
    ], S.HOOP, rank=2)
    f.add("Bulkhead top", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_BULKHEAD, -S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
    ], S.HOOP, rank=2)
    f.add("Bulkhead bottom", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD, -S.W_BULKHEAD, S.Z_FLOOR),
    ], S.HOOP, rank=2)
    f.add("Bulkhead diagonal", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD, -S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
    ], S.SMALL, rank=3)

    # --- rank 2: the upper rails of the front box ------------------------
    # From the bulkhead's top corners back to the front hoop. They land ON
    # the hoop, which by then is leaning, so the width is asked of the hoop
    # rather than assumed.
    f.pair("Front upper rail", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_FRONT_HOOP, fh(S.Z_SIDE_UPPER), S.Z_SIDE_UPPER),
    ], S.STRUCT, rank=2)

    # --- rank 3: the bulkhead support ------------------------------------
    # A ring on the rails a little behind the bulkhead, and the triangles
    # that tie the bulkhead to it. The rules ask for this to be a truss,
    # and the photograph shows it as one.
    f.pair("Support upright", [
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_FLOOR),
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT_TOP, S.Z_SUPPORT_TOP),
    ], S.STRUCT, rank=3)
    f.add("Support top", [
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT_TOP, S.Z_SUPPORT_TOP),
        (S.X_BULKHEAD_SUPPORT, -S.W_SUPPORT_TOP, S.Z_SUPPORT_TOP),
    ], S.STRUCT, rank=3)
    f.pair("Support side diagonal", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_FLOOR),
    ], S.STRUCT, rank=4)
    f.add("Nose floor diagonal", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_FLOOR),
        (S.X_BULKHEAD_SUPPORT, -S.W_SUPPORT, S.Z_FLOOR),
    ], S.SMALL, rank=4)
    f.add("Nose roof diagonal", [
        (S.X_BULKHEAD, S.W_BULKHEAD, S.Z_BULKHEAD_TOP),
        (S.X_BULKHEAD_SUPPORT, -S.W_SUPPORT_TOP, S.Z_SUPPORT_TOP),
    ], S.SMALL, rank=4)

    # --- rank 4: the footwell, support ring to front hoop -----------------
    f.pair("Footwell side diagonal", [
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_FLOOR),
        (S.X_FRONT_HOOP, fh(S.Z_SIDE_UPPER), S.Z_SIDE_UPPER),
    ], S.STRUCT, rank=4)
    f.add("Footwell floor diagonal", [
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT, S.Z_FLOOR),
        (S.X_FRONT_HOOP, -S.W_FRONT_HOOP, S.Z_FLOOR),
    ], S.SMALL, rank=4)
    f.add("Footwell roof diagonal", [
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT_TOP, S.Z_SUPPORT_TOP),
        (S.X_FRONT_HOOP, -fh(S.Z_SIDE_UPPER), S.Z_SIDE_UPPER),
    ], S.SMALL, rank=4)
    f.add("Front hoop bottom", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_FRONT_HOOP, -S.W_FRONT_HOOP, S.Z_FLOOR),
    ], S.STRUCT, rank=3)

    # --- rank 4: the front hoop's bracing --------------------------------
    # Forward and down onto the support ring, which is where the photograph
    # puts it and what the rules allow. It lands BELOW the crown bend.
    f.pair("Front hoop brace", [
        (S.X_FRONT_HOOP, S.W_FH_BRACE, S.Z_FH_BRACE),
        (S.X_BULKHEAD_SUPPORT, S.W_SUPPORT_TOP, S.Z_SUPPORT_TOP),
    ], S.STRUCT, rank=4)

    # --- rank 3: the side impact structure -------------------------------
    # The rules want three members a side between the hoops. The lower one
    # IS the lower rail; the upper one and the diagonal are these.
    f.pair("Side impact upper", [
        (S.X_FRONT_HOOP, fh(S.Z_SIDE_UPPER), S.Z_SIDE_UPPER),
        (S.X_MAIN_HOOP, mh(S.Z_SIDE_UPPER), S.Z_SIDE_UPPER),
    ], S.STRUCT, rank=3)
    f.pair("Side impact diagonal", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, mh(S.Z_SIDE_UPPER), S.Z_SIDE_UPPER),
    ], S.STRUCT, rank=4)
    f.add("Cockpit floor diagonal", [
        (S.X_FRONT_HOOP, S.W_FRONT_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_FLOOR),
    ], S.SMALL, rank=4)
    f.add("Main hoop bottom", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_FLOOR),
        (S.X_MAIN_HOOP, -S.W_MAIN_HOOP, S.Z_FLOOR),
    ], S.STRUCT, rank=3)
    # The shoulder harness bar: the CAD has a `shoulder harness mount`, and
    # the rules want the belts on a member of their own.
    f.add("Shoulder harness bar", [
        (S.X_MAIN_HOOP, mh(S.Z_HARNESS), S.Z_HARNESS),
        (S.X_MAIN_HOOP, -mh(S.Z_HARNESS), S.Z_HARNESS),
    ], S.STRUCT, rank=3)

    # --- rank 4: the rear truss ------------------------------------------
    # It CONVERGES: everything behind the main hoop runs in toward a pair of
    # node clusters at the back, which is where the rear suspension and the
    # top of the main hoop bracing all land. That convergence is the single
    # biggest difference from a boxed rear bay.
    f.pair("Rear upper rail", [
        (S.X_MAIN_HOOP, mh(S.Z_REAR_TOP), S.Z_REAR_TOP),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.STRUCT, rank=4)
    f.pair("Main hoop brace", [
        (S.X_MAIN_HOOP, S.W_MH_BRACE, S.Z_MH_BRACE),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.HOOP, rank=4)
    f.pair("Rear lower rail", [
        (S.X_ENGINE_FRONT, S.W_ENGINE, S.Z_FLOOR),
        (S.X_REAR, S.W_REAR, S.Z_FLOOR),
    ], S.STRUCT, rank=4)
    f.pair("Rear upright", [
        (S.X_REAR, S.W_REAR, S.Z_FLOOR),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.STRUCT, rank=5)
    f.add("Rear top cross", [
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
        (S.X_REAR, -S.W_REAR, S.Z_REAR_TOP),
    ], S.STRUCT, rank=5)
    f.add("Rear bottom cross", [
        (S.X_REAR, S.W_REAR, S.Z_FLOOR),
        (S.X_REAR, -S.W_REAR, S.Z_FLOOR),
    ], S.STRUCT, rank=5)
    f.pair("Rear side diagonal", [
        (S.X_ENGINE_FRONT, S.W_ENGINE, S.Z_FLOOR),
        (S.X_REAR, S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=5)
    f.add("Rear roof diagonal", [
        (S.X_MAIN_HOOP, mh(S.Z_REAR_TOP), S.Z_REAR_TOP),
        (S.X_REAR, -S.W_REAR, S.Z_REAR_TOP),
    ], S.SMALL, rank=5)
    f.add("Engine bay floor diagonal", [
        (S.X_MAIN_HOOP, S.W_MAIN_HOOP, S.Z_FLOOR),
        (S.X_REAR, -S.W_REAR, S.Z_FLOOR),
    ], S.SMALL, rank=5)
    # The two main hoop braces are triangulated to EACH OTHER. Without this
    # they are two long struts that can only fold sideways together.
    _bx = (S.X_MAIN_HOOP + S.X_REAR) / 2.0
    _by = (S.W_MH_BRACE + S.W_REAR) / 2.0
    _bz = (S.Z_MH_BRACE + S.Z_REAR_TOP) / 2.0
    f.add("Brace cross", [(_bx, _by, _bz), (_bx, -_by, _bz)],
          S.SMALL, rank=5)
    f.pair("Brace stay", [
        (_bx, _by, _bz),
        (S.X_MAIN_HOOP, mh(S.Z_REAR_TOP), S.Z_REAR_TOP),
    ], S.SMALL, rank=5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", default=None, help="where the order is written")
    ap.add_argument("--shop", metavar="DIR",
                    help="write the whole order: a STEP per member, the cut "
                         "list, the bend schedule, a DXF for every flat "
                         "blank, and a manifest")
    ap.add_argument("--save", metavar="PROJECT",
                    help="save the project, so the frame, the panels, the "
                         "tabs and the jig can be opened and looked at")
    ap.add_argument("--contact", action="store_true",
                    help="run the exact contact sweep — it answers whether "
                         "the frame is ONE connected thing, and on 47 members "
                         "that is 1081 pairs and about four minutes")
    args = ap.parse_args()
    d = Designer(port=args.port)
    t0 = time.time()

    d.call("new_project")
    # A new project comes with an empty default part. Rename it rather than
    # adding a second one, or every saved project ships an empty tab.
    d.call("rename_part", part=d.call("list_parts")["parts"][0]["name"],
           new_name="Chassis")
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

    if args.shop:
        # The rest of the car, and the fixture it is welded in.
        chassis_panels.floor(d)
        chassis_panels.firewall(d)
        tabs = chassis_panels.tabs(d, f)
        plates = chassis_jig.build(d, f)
        print(f"{len(tabs)} tabs, {len(plates)} jig plates")
        d.call("set_active_part", part="Chassis")
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

    if args.shop:
        shop(d, f, args.shop)
    if args.save:
        d.call("save_project", path=args.save)
        print(f"project saved to {args.save}")
    return d, f, args


def shop(d, frame, root):
    """Everything a shop needs, in one directory, with a manifest.

    The manifest is what makes the rest usable: a row per file, what it is,
    what it is made of, and how many — so nobody has to infer from a folder
    listing which DXF is a jig plate and which is a floor.
    """
    import csv
    os.makedirs(root, exist_ok=True)
    rows = []

    # 1. The tubes: one solid each, ends already coped and mitred.
    members = d.call("export_members", dir=os.path.join(root, "members"),
                     overwrite=True)
    sizes = {}
    for t in d.request("tube_schedule", {})["tubes"]:
        for b in t["bodies"]:
            sizes[b] = (t.get("outer_diameter_mm"), t.get("wall_mm"))
    for f in members["files"]:
        od, wall = sizes.get(f["member"], (None, None))
        rows.append(["members/" + f["file"], "tube member", f["member"],
                     S.MATERIAL,
                     f"{od:g}x{wall:g}" if od else "", 1])
    print(f"\n-- the order --\n{members['count']} member solids")

    # 2. What to buy, and what to set the bender to.
    for name, call in (("cut_list.csv", "export_cut_list"),
                       ("bend_schedule.csv", "export_bend_schedule")):
        kw = {"path": os.path.join(root, name)}
        if call == "export_cut_list":
            kw.update(stock_length_mm=S.STOCK_LENGTH, kerf_mm=S.KERF)
        d.call(call, **kw)
        rows.append([name, "list", name[:-4].replace("_", " "), "", "", 1])

    # 3. Every flat blank, cut and marked.
    flats = [("Floor", "panels", "FLOOR"), ("Firewall", "panels", "FIREWALL")]
    for p in d.call("list_parts")["parts"]:
        if p.get("type") != "part":
            continue
        if p["name"].startswith("Pickup"):
            flats.append((p["name"], "tabs", p["name"].replace(" ", "-")))
        elif p["name"].startswith("Jig "):
            flats.append((p["name"], "jig", p["name"][4:].upper()))
    for part, where, mark in flats:
        os.makedirs(os.path.join(root, where), exist_ok=True)
        d.call("set_active_part", part=part)
        gauge = d.call("get_sheet_metal_rules")["rules"]
        out = os.path.join(root, where, part.replace(" ", "_") + ".dxf")
        d.call("export_dxf", path=out, mark=mark)
        rows.append([f"{where}/{os.path.basename(out)}", where[:-1]
                     if where.endswith("s") else where, part,
                     gauge["material"], gauge["gauge"], 1])
    print(f"{len(flats)} flat blanks (panels, tabs, jig plates)")

    with open(os.path.join(root, "manifest.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["file", "kind", "part", "material", "size", "quantity"])
        w.writerows(rows)
    print(f"manifest: {len(rows)} rows -> {root}")
    return rows


if __name__ == "__main__":
    main()
