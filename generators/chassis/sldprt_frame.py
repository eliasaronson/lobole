#!/usr/bin/env python3
"""Read a welded tube frame out of a SolidWorks part file, without SolidWorks.

    python3 generators/chassis/sldprt_frame.py FRAME.SLDPRT [--out fs_chassis_frame.json]
    python3 generators/chassis/sldprt_frame.py FRAME.SLDPRT --preview preview.png

A SolidWorks 2015+ part is a container of RAW-deflate streams (a ZIP-like
local header, `14 00 06 00 08 00`, and no zlib wrapper — which is why a search
for zlib streams finds nothing). Two of the streams are Parasolid TRANSMIT
files, each itself zlib-wrapped 28 bytes in: the solid model. This reads the
largest one and takes from it only what a tube frame is made of:

- CYLINDER records — centre, axis, RADIUS, reference, as big-endian doubles in
  that order (a CIRCLE has the radius LAST, and matching that order instead
  finds circles). A tube is an outer cylinder and an inner one on the same
  axis: that gives the member's axis, its OD and its wall.
- TORUS records — the bends, and their radius.
- sampled POINTS — Parasolid keeps points along the curves where tubes meet;
  the ones on a tube's surface bound it.

From those it builds a NODE GRAPH (where axes meet), makes it mirror-symmetric
and writes it as JSON the chassis generator reads. It refuses what does not
add up rather than guessing: a member with no mirror twin, a bend with no two
legs, a joint only one member reaches.

It reads GEOMETRY, not the feature tree: there are no names, no cut list and
no 3D sketch here — just what the tubes are, where, and how big.
"""
import argparse
import json
import math
import re
import struct
import sys
import zlib

SECTION = b"\x14\x00\x06\x00\x08\x00"


# --- the container ---------------------------------------------------------

def streams(blob):
    """Every raw-deflate stream in the container, as (offset, bytes)."""
    out = []
    seen = set()
    for m in re.finditer(re.escape(SECTION), blob):
        o = m.start()
        for skip in range(20, 700):
            try:
                d = zlib.decompressobj(-15)
                data = d.decompress(blob[o + skip:o + skip + 8_000_000])
            except zlib.error:
                continue
            if d.eof and len(data) > 200:
                if o + skip not in seen:
                    seen.add(o + skip)
                    out.append((o + skip, data))
                break
    return out


def parasolid(data):
    """The Parasolid transmit text inside a stream, or None."""
    for skip in range(0, 64):
        try:
            out = zlib.decompressobj(15).decompress(data[skip:])
        except zlib.error:
            continue
        if out.startswith(b"PS") and b"TRANSMIT FILE" in out[:80]:
            return out
    return None


def preview(blob):
    """The container's PNG preview, if it has one."""
    for _, data in streams(blob):
        if data.startswith(b"\x89PNG"):
            return data[:data.find(b"IEND") + 8]
    return None


# --- geometry -----------------------------------------------------------------

def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def add(a, b):
    return [a[i] + b[i] for i in range(3)]


def mul(a, k):
    return [a[i] * k for i in range(3)]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def norm(a):
    return math.sqrt(dot(a, a))


def is_unit(v):
    return abs(norm(v) - 1.0) < 1e-9


def records(xt):
    """CYLINDER and TORUS records, in mm."""
    cyl, tor = [], []
    for o in range(0, len(xt) - 96):
        v = struct.unpack_from(">12d", xt, o)
        p, ax = v[0:3], v[3:6]
        if max(abs(c) for c in p) > 5.0 or not is_unit(ax):
            continue
        r, xa = v[6], v[7:10]
        if 0.002 < r < 0.05 and is_unit(xa) and abs(dot(ax, xa)) < 1e-9:
            cyl.append((mul(p, 1000.0), list(ax), r * 1000.0))
            continue
        big, small, xa = v[6], v[7], v[8:11]
        if (0.01 < big < 0.8 and 0.002 < small < 0.05 and is_unit(xa)
                and abs(dot(ax, xa)) < 1e-9):
            tor.append((mul(p, 1000.0), list(ax), big * 1000.0, small * 1000.0))
    return cyl, tor


def points(xt):
    out = []
    for o in range(0, len(xt) - 24):
        x, y, z = struct.unpack_from(">3d", xt, o)
        if (all(1e-4 < abs(c) < 3.0 or c == 0.0 for c in (x, y, z))
                and abs(x) + abs(y) + abs(z) > 0.01):
            out.append((x * 1000.0, y * 1000.0, z * 1000.0))
    return out


def canonical(ax):
    i = max(range(3), key=lambda k: abs(ax[k]))
    return mul(ax, -1.0) if ax[i] < 0 else list(ax)


def same_axis(p, ax, q, bx, tol=0.2):
    return (abs(abs(dot(ax, bx)) - 1.0) < 1e-6
            and norm(cross(sub(q, p), ax)) < tol)


def tubes(cyl):
    """Straight members: an outer cylinder with the largest radius on each
    axis, its wall from the next radius in on the same axis."""
    axes = []
    for p, ax, r in cyl:
        for a in axes:
            if same_axis(a["p"], a["ax"], p, ax):
                a["radii"].add(round(r, 4))
                break
        else:
            axes.append({"p": p, "ax": canonical(ax), "radii": {round(r, 4)}})
    out = []
    for a in axes:
        radii = sorted(a["radii"], reverse=True)
        outer = radii[0]
        inner = next((r for r in radii[1:] if outer - r < 4.0), None)
        if inner is None:
            continue  # a hole or a boss, not a tube
        # To the micron the file holds: 1-1/8" is 28.575, not 28.57, and a
        # designation that is 0.005 off is a tube nobody stocks.
        out.append({"p": a["p"], "ax": a["ax"], "od": round(2 * outer, 3),
                    "wall": round(outer - inner, 3)})
    return out


def extents(tube_list, pts):
    """Bound each tube by the points on its outer surface, dropping isolated
    clusters (three or fewer within 60 mm) — numbers that parse as points."""
    for t in tube_list:
        r = t["od"] / 2.0
        ts = []
        for q in pts:
            d = sub(q, t["p"])
            s = dot(d, t["ax"])
            rad = math.sqrt(max(0.0, dot(d, d) - s * s))
            if abs(rad - r) < 0.05 or rad < 0.05:
                ts.append(s)
        ts.sort()
        if not ts:
            t["ext"] = None
            continue
        groups = [[ts[0]]]
        for s in ts[1:]:
            (groups[-1].append(s) if s - groups[-1][-1] < 60.0
             else groups.append([s]))
        keep = [s for g in groups if len(g) > 3 for s in g] or ts
        t["ext"] = (min(keep), max(keep))
        t["a"] = add(t["p"], mul(t["ax"], t["ext"][0]))
        t["b"] = add(t["p"], mul(t["ax"], t["ext"][1]))
    return [t for t in tube_list if t.get("ext")]


# --- bends: chain straight legs into bent members --------------------------

def line_line(p, u, q, v):
    """Closest points of two infinite lines: (P, Q, s, t)."""
    w = sub(p, q)
    b, d, e = dot(u, v), dot(u, w), dot(v, w)
    den = 1.0 - b * b
    if den < 1e-12:
        return None
    s = (b * e - d) / den
    t = (e - b * d) / den
    return add(p, mul(u, s)), add(q, mul(v, t)), s, t


def bends(tube_list, tor):
    """Each distinct bend (a torus whose tube radius matches a member's OD),
    with the two straight legs it joins."""
    seen = []
    for c, ax, big, small in tor:
        if any(norm(sub(c, s["c"])) < 0.5 for s in seen):
            continue
        legs = []
        for i, t in enumerate(tube_list):
            if abs(t["od"] / 2.0 - small) > 0.05:
                continue
            if abs(dot(t["ax"], ax)) > 1e-4:
                continue  # not in the bend's plane
            d = sub(c, t["p"])
            s = dot(d, t["ax"])
            off = norm(sub(d, mul(t["ax"], s)))
            if abs(off - big) < 0.5:
                legs.append(i)
        # Only a torus that bends a MEMBER counts: the inner surface of the
        # same bend sits at the same centre with the bore's radius, matches
        # no leg, and — recorded first — hid the real one behind it.
        if legs:
            seen.append({"c": c, "ax": ax, "radius": big, "legs": legs})
    return seen


def chains(tube_list, bend_list):
    """Bent members: straight legs linked by bends, foot to foot."""
    link = {}
    for bd in bend_list:
        if len(bd["legs"]) != 2:
            raise SystemExit(f"a bend at {[round(c) for c in bd['c']]} has "
                             f"{len(bd['legs'])} legs, not two")
        a, b = bd["legs"]
        link.setdefault(a, []).append((b, bd))
        link.setdefault(b, []).append((a, bd))
    used, out = set(), []
    for start in link:
        if start in used or len(link[start]) != 1:
            continue  # begin at a foot: a leg with one bend
        path, prev, cur = [start], None, start
        while True:
            used.add(cur)
            nxt = [(n, bd) for n, bd in link[cur] if n != prev]
            if not nxt:
                break
            prev, cur = cur, nxt[0][0]
            path.append(cur)
        out.append(path)
    return out


# --- joints and nodes -----------------------------------------------------------

def seg_closest(a0, a1, b0, b1):
    u, v, w = sub(a1, a0), sub(b1, b0), sub(a0, b0)
    a, b, c = dot(u, u), dot(u, v), dot(v, v)
    d, e = dot(u, w), dot(v, w)
    den = a * c - b * b
    sc = 0.0 if den < 1e-12 else max(0.0, min(1.0, (b * e - c * d) / den))
    tc = max(0.0, min(1.0, (b * sc + e) / c)) if c > 1e-12 else 0.0
    sc = max(0.0, min(1.0, (b * tc - d) / a)) if a > 1e-12 else 0.0
    p, q = add(a0, mul(u, sc)), add(b0, mul(v, tc))
    return norm(sub(p, q)), p, q


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sldprt")
    ap.add_argument("--out", default=None)
    ap.add_argument("--preview", default=None,
                    help="also write the file's own preview PNG here")
    ap.add_argument("--min-length", type=float, default=60.0,
                    help="shorter tubes are bosses and sleeves, not members")
    a = ap.parse_args()
    blob = open(a.sldprt, "rb").read()
    if a.preview:
        png = preview(blob)
        if png:
            open(a.preview, "wb").write(png)
    xts = [x for x in (parasolid(d) for _, d in streams(blob)) if x]
    if not xts:
        raise SystemExit("no Parasolid partition in this file")
    xt = max(xts, key=len)
    cyl, tor = records(xt)
    tl = extents(tubes(cyl), points(xt))
    short = [t for t in tl if t["ext"][1] - t["ext"][0] < a.min_length]
    tl = [t for t in tl if t["ext"][1] - t["ext"][0] >= a.min_length]
    bl = [b for b in bends(tl, tor) if b["legs"]]
    hoops = chains(tl, bl)
    in_hoop = {i for h in hoops for i in h}
    print(f"{len(tl)} straight tube runs, {len(bl)} bends -> {len(hoops)} "
          f"bent members; {len(short)} short pieces set aside "
          f"(< {a.min_length:.0f} mm: bosses, sleeves)", file=sys.stderr)

    # The segments every joint is looked for on: each straight run, and each
    # leg of each bent member, extended by a radius so a coped end (which
    # stops at its neighbour's SURFACE) still reaches the neighbour's AXIS.
    segs = []
    for i, t in enumerate(tl):
        r = t["od"] / 2.0 + 20.0
        segs.append((i, sub(t["a"], mul(t["ax"], r)),
                     add(t["b"], mul(t["ax"], r))))
    joints = []
    for x in range(len(segs)):
        for y in range(x + 1, len(segs)):
            i, a0, a1 = segs[x]
            j, b0, b1 = segs[y]
            if i in in_hoop and j in in_hoop and any(
                    i in h and j in h for h in hoops):
                continue  # two legs of one bent member meet at its bend
            gap, p, q = seg_closest(a0, a1, b0, b1)
            if gap < tl[i]["od"] / 2.0 + tl[j]["od"] / 2.0 + 2.0:
                joints.append((mul(add(p, q), 0.5), i, j))
    # Cluster joints into nodes.
    nodes = []
    for p, i, j in joints:
        for n in nodes:
            if norm(sub(n["p"], p)) < 25.0:
                n["pts"].append(p)
                n["m"].update((i, j))
                break
        else:
            nodes.append({"p": p, "pts": [p], "m": {i, j}})
    for n in nodes:
        n["p"] = mul([sum(q[k] for q in n["pts"]) for k in range(3)],
                     1.0 / len(n["pts"]))
    # Mirror-symmetric: pair each node with its image across y = 0.
    for n in nodes:
        if abs(n["p"][1]) < 15.0:
            n["p"][1] = 0.0
            continue
        img = [n["p"][0], -n["p"][1], n["p"][2]]
        twin = min(nodes, key=lambda o: norm(sub(o["p"], img)))
        if norm(sub(twin["p"], img)) > 30.0:
            raise SystemExit(f"node {[round(c) for c in n['p']]} has no "
                             f"mirror twin (nearest is "
                             f"{norm(sub(twin['p'], img)):.0f} mm off)")
    for n in nodes:
        if n["p"][1] > 0:
            img = [n["p"][0], -n["p"][1], n["p"][2]]
            twin = min(nodes, key=lambda o: norm(sub(o["p"], img)))
            avg = mul(add(n["p"], [twin["p"][0], -twin["p"][1],
                                   twin["p"][2]]), 0.5)
            n["p"], twin["p"] = avg, [avg[0], -avg[1], avg[2]]

    idx = {id(n): k for k, n in enumerate(nodes)}

    def nodes_on(i):
        """The nodes on tube run i, ordered along it."""
        t = tl[i]
        on = [(dot(sub(n["p"], t["p"]), t["ax"]), idx[id(n)])
              for n in nodes if i in n["m"]]
        return [k for _, k in sorted(on)]

    members = []
    for i, t in enumerate(tl):
        if i in in_hoop:
            continue
        on = nodes_on(i)
        if len(on) < 2:
            raise SystemExit(f"a {t['od']} tube at "
                             f"{[round(c) for c in t['a']]} meets "
                             f"{len(on)} other member(s): a member is welded "
                             "at both ends")
        members.append({"od": t["od"], "wall": t["wall"],
                        "nodes": [on[0], on[-1]], "through": on[1:-1]})
    bent = []
    for h in hoops:
        legs = [tl[i] for i in h]
        corners = [None] * (len(legs) + 1)
        for k in range(len(legs) - 1):
            P, Q, _, _ = line_line(legs[k]["p"], legs[k]["ax"],
                                   legs[k + 1]["p"], legs[k + 1]["ax"])
            corners[k + 1] = mul(add(P, Q), 0.5)
        # The feet: the node nearest each free end of the first/last leg.
        ends = []
        for leg, near in ((legs[0], corners[1]), (legs[-1], corners[-2])):
            far = leg["a"] if norm(sub(leg["a"], near)) > \
                norm(sub(leg["b"], near)) else leg["b"]
            foot = min((n for n in nodes if any(i in n["m"] for i in h)),
                       key=lambda n: norm(sub(n["p"], far)))
            ends.append(idx[id(foot)])
        through = sorted({idx[id(n)] for n in nodes
                          if any(i in n["m"] for i in h)} - set(ends))
        radius = next(b["radius"] for b in bl if b["legs"][0] in h)
        bent.append({"od": legs[0]["od"], "wall": legs[0]["wall"],
                     "radius": round(radius, 1), "feet": ends,
                     "corners": [[round(c, 2) for c in p]
                                 for p in corners[1:-1]],
                     "through": through})
    # One wall per OD, by majority: a bore can pair with a NEIGHBOUR'S
    # cylinder where two tubes' axes nearly coincide at a joint, and one
    # 15.88 tube read the 28.57's 1.47 wall that way.
    walls = {}
    for m in members + bent:
        walls.setdefault(m["od"], []).append(m["wall"])
    for m in members + bent:
        w = walls[m["od"]]
        m["wall"] = max(set(w), key=w.count)
    out = {
        "source": a.sldprt.rsplit("/", 1)[-1],
        "units": "mm; x rearward, z up, y across (the frame is mirrored "
                 "about y = 0)",
        "nodes": [[round(c, 2) for c in n["p"]] for n in nodes],
        "members": members,
        "bent": bent,
        "set_aside": [{"od": t["od"], "wall": t["wall"],
                       "a": [round(c, 1) for c in t["a"]],
                       "b": [round(c, 1) for c in t["b"]]} for t in short],
    }
    text = json.dumps(out, indent=1)
    if a.out:
        open(a.out, "w").write(text + "\n")
    else:
        print(text)
    print(f"{len(nodes)} nodes, {len(members)} straight members, "
          f"{len(bent)} bent members", file=sys.stderr)


if __name__ == "__main__":
    main()
