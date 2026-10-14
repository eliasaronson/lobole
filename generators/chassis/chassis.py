# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

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

import chassis_spec as S          # noqa: E402
import chassis_jig                # noqa: E402
import chassis_panels             # noqa: E402
from chassis_frame import Frame   # noqa: E402
from lobole.client import Client  # noqa: E402


def _name(a, b):
    """A member's name from where it is and which way it runs: what a
    fabricator calls it on the jig. The CAD has none to give — a weldment's
    members are nameless solids — so the name is read off the geometry."""
    mid = [(a[k] + b[k]) / 2.0 for k in range(3)]
    if mid[0] < S.X_BULKHEAD_SUPPORT - 1.0:
        zone = "Nose"
    elif mid[0] < S.X_FRONT_HOOP - 1.0:
        zone = "Footwell"
    elif mid[0] < S.X_MAIN_HOOP - 1.0:
        zone = "Cockpit"
    elif mid[0] < S.X_REAR - 150.0:
        zone = "Engine bay"
    else:
        zone = "Rear"
    d = [b[k] - a[k] for k in range(3)]
    n = sum(c * c for c in d) ** 0.5
    u = [abs(c) / n for c in d]
    if u[0] > 0.9:
        low = max(a[2], b[2]) < S.Z_FLOOR + 90.0
        kind = "lower rail" if low else "rail"
    elif u[1] > 0.9:
        kind = "cross"
    elif u[2] > 0.9:
        kind = "upright"
    else:
        kind = "diagonal"
    side = "" if abs(mid[1]) < 5.0 else (" L" if mid[1] > 0 else " R")
    return f"{zone} {kind}{side}"


def _order(table):
    """The order the frame is tacked up in, as a rank per member.

    A member that a node sits in the MIDDLE of runs through it, and is tacked
    before anything that ENDS there — the rule the old generator applied by
    hand, now read off the topology. Where several members END at one node
    (a corner) there is nothing to say which goes first, so the heavier and
    then the longer one does. Every member gets its own rank, so every pair
    that meets is resolved by exactly one cope or one mitre.
    """
    n = len(table)
    before = [set() for _ in range(n)]
    for i, m in enumerate(table):
        for j, o in enumerate(table):
            if i != j and set(o["through"]) & set(m["ends"]):
                before[i].add(j)
    weight = lambda k: (-table[k]["od"], -table[k]["length"])  # noqa: E731
    done, order = set(), []
    while len(order) < n:
        ready = [k for k in range(n) if k not in done
                 and before[k] <= done]
        if not ready:  # a cycle: take the heaviest remaining
            ready = [k for k in range(n) if k not in done]
        k = min(ready, key=weight)
        done.add(k)
        order.append(k)
    return {k: r for r, k in enumerate(order)}


def members(f):
    """Every member of the frame, from the team's CAD.

    fs_chassis_frame.json is the node graph `sldprt_frame.py` read out of
    the team's SolidWorks frame part: straight members as node pairs, the two
    hoops as bent members through their corners on the CAD's own 100 mm
    bends. What this adds is what the CAD cannot say: a name for each piece
    and the order they are tacked in.
    """
    N = S.NODES
    table = []
    for b in S.FRAME["bent"]:
        pts = [N[b["feet"][0]]] + [tuple(c) for c in b["corners"]] + \
              [N[b["feet"][1]]]
        tall = max(c[2] for c in b["corners"])
        name = "Main hoop" if b is S.MAIN_HOOP else "Front hoop"
        table.append({"name": name, "points": pts, "od": b["od"],
                      "size": S.size_name(b["od"], b["wall"]),
                      "ends": set(b["feet"]), "through": set(b["through"]),
                      "former": b["radius"], "length": tall})
    # The members the rules NAME get their names: a scrutineer asks for the
    # main hoop bracing by that name, not for "engine bay diagonal 7".
    on_hoop = {id(b): set(b["feet"]) | set(b["through"])
               for b in S.FRAME["bent"]}
    knee = {id(b): min(c[2] for c in b["corners"]) for b in S.FRAME["bent"]}

    def special(m, a, b):
        side = "" if abs(a[1] + b[1]) < 10.0 else \
            (" L" if a[1] + b[1] > 0 else " R")
        if abs(a[0] - S.X_BULKHEAD) < 5.0 and abs(b[0] - S.X_BULKHEAD) < 5.0:
            return "Bulkhead " + _name(a, b).split(" ", 1)[1]
        if abs(m["od"] - 30.0) < 0.1:
            return ("Shoulder harness bar" if abs(a[1] + b[1]) < 10.0
                    else "Harness bar strut" + side)
        for hoop, label, rearward in ((S.MAIN_HOOP, "Main hoop brace", True),
                                      (S.FRONT_HOOP, "Front hoop brace",
                                       False)):
            for end, other in ((m["nodes"][0], b), (m["nodes"][1], a)):
                p = N[end]
                if (end in on_hoop[id(hoop)] and p[2] > knee[id(hoop)] + 50.0
                        and ((other[0] > p[0] + 100.0) if rearward
                             else (other[0] < p[0] - 100.0))):
                    return label + side
        return None

    for m in S.FRAME["members"]:
        a, b = N[m["nodes"][0]], N[m["nodes"][1]]
        length = sum((a[k] - b[k]) ** 2 for k in range(3)) ** 0.5
        table.append({"name": special(m, a, b) or _name(a, b),
                      "points": [a, b], "od": m["od"],
                      "size": S.size_name(m["od"], m["wall"]),
                      "ends": set(m["nodes"]), "through": set(m["through"]),
                      "former": None, "length": length})
    # Two members read the same name (a nose has four diagonals a side):
    # number them front to back so every piece in the cut list is its own.
    seen = {}
    for t in sorted(table, key=lambda t: min(p[0] for p in t["points"])):
        k = t["name"]
        seen[k] = seen.get(k, 0) + 1
        t["n"] = seen[k]
    for t in table:
        if seen[t["name"]] > 1:
            base, _, side = t["name"].rpartition(" ")
            if side in ("L", "R"):
                t["name"] = f"{base} {t['n']} {side}"
            else:
                t["name"] = f"{t['name']} {t['n']}"
    rank = _order(table)
    for k, t in enumerate(table):
        f.add(t["name"], t["points"], t["size"], rank=rank[k],
              former=t["former"])


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
    d = Client(port=args.port)
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
    # Where the CAD put a joint on a hoop's bend, the node was slid clear of
    # it (chassis_spec._clear_of_bends): say so, since the frame built is
    # then not the frame drawn.
    for n, by, past, arc in S.MOVED:
        print(f"  node {n} moved {by:.0f} mm off a hoop bend: the CAD has it "
              f"on the arc, which runs {arc:.0f} mm from the corner; now "
              f"{past:.0f} mm past where the arc ends")
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
        retried = sum(p["fuzzy_retries"] for p in plates)
        print(f"{len(tabs)} tabs, {len(plates)} jig plates"
              + (f" ({retried} cut(s) through the fuzzy retry)"
                 if retried else ""))
        d.call("set_active_part", part="Chassis")
    for i in range(len(bodies)):
        d.call("set_material", body=i, material=S.MATERIAL)
        d.call("set_appearance", body=i, color=list(S.FINISH),
               roughness=0.35, metallic=0.2)

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
          f"centre of mass at x={mass['total']['center_of_mass_mm'][0]:.0f} mm")

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
